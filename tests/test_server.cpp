// test_server.cpp - end-to-end tests of the HTTP layer.
//
// These tests use the REAL server code (wb_srv::ApiServer + JobManager + wb_core),
// started on a free 127.0.0.1 port inside the test process and queried with
// httplib::Client. Nothing is mocked: a passing test means the same code path
// that Launch.exe runs behaves as docs/API.md says.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include <httplib.h>
#include <json.hpp>

#include "api_server.h"
#include "job_manager.h"
#include "wb_test.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

fs::path unique_temp_dir(const char* tag) {
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (std::string("wb_test_") + tag + "_" + std::to_string(t));
}

// Polls `pred` every 20 ms until it is true or `seconds` passed.
template <class F>
bool wait_for(F pred, double seconds) {
    auto t0 = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > seconds) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

const char* kStl =
    "solid tri\n"
    "  facet normal 0 0 1\n"
    "    outer loop\n"
    "      vertex 0 0 0\n"
    "      vertex 1 0 0\n"
    "      vertex 0 1 0\n"
    "    endloop\n"
    "  endfacet\n"
    "endsolid tri\n";

// A server on a temp web folder + temp data folder, running in a background thread.
struct TestServer {
    fs::path root, web, data;
    std::unique_ptr<wb_srv::ApiServer> srv;
    std::thread th;
    std::atomic<bool> returned{false};

    explicit TestServer(double heartbeat_timeout_s = 30.0, const fs::path* data_dir = nullptr) {
        root = unique_temp_dir("srv");
        web = root / "web";
        write_file(web / "index.html", "<html><body>WB-TEST-INDEX</body></html>");
        write_file(web / "app.js", "export const x = 1;\n");
        write_file(web / "style.css", "body{}\n");
        write_file(web / "sub dir" / "a.txt", "hello");
        write_file(root / "secret.txt", "TOP-SECRET");
        data = data_dir ? *data_dir : root / "data";
        if (!data_dir) {
            write_file(data / "tri.stl", kStl);
            write_file(data / "ok.csv", "node_id,x_m,y_m,z_m,temperature_C\n1,0,0,0,20\n2,1,0,0,30\n");
            write_file(data / "bad" / "broken.csv", "node_id,x_m,y_m,z_m,temperature_C\n1,0,0,0,abc\n");
            write_file(data / "notes.txt", "not a sample");
        }
        srv.reset(new wb_srv::ApiServer(web, data));
        srv->set_heartbeat_timeout_s(heartbeat_timeout_s);
        CHECK(srv->listen_free_port());
        CHECK(srv->port() > 0);
        th = std::thread([this] { srv->run(); returned = true; });
        CHECK(srv->wait_ready(5000));
    }
    ~TestServer() {
        srv->stop();
        if (th.joinable()) th.join();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    httplib::Client client() const {
        httplib::Client c("127.0.0.1", srv->port());
        c.set_connection_timeout(5);
        c.set_read_timeout(120);
        return c;
    }
};

json body_of(const httplib::Result& r) {
    CHECK(static_cast<bool>(r));
    json j = json::parse(r->body, nullptr, false);
    CHECK(!j.is_discarded());
    return j;
}

httplib::Result post_json(httplib::Client& c, const std::string& path, const json& j) {
    return c.Post(path, j.dump(), "application/json");
}

// Starts a bench job, returns its id (checks HTTP 200).
std::string start_bench(httplib::Client& c, const json& cfg) {
    auto r = post_json(c, "/api/bench/start", cfg);
    CHECK(r && r->status == 200);
    return body_of(r)["job"].get<std::string>();
}

json job_state(httplib::Client& c, const std::string& id) {
    auto r = c.Get("/api/jobs/" + id);
    CHECK(r && r->status == 200);
    return body_of(r);
}

}  // namespace

// ---- lifecycle, static files, security ---------------------------------------------

WB_TEST(server_health_and_static_files) {
    TestServer s;
    auto c = s.client();
    auto h = c.Get("/api/health");
    CHECK(h && h->status == 200);
    json j = body_of(h);
    CHECK(j["ok"].get<bool>());
    CHECK(j["version"] == "1.0.0");
    CHECK(j["simulated_hw"].get<bool>());
    CHECK(j.contains("launch_to_ready_ms"));

    auto idx = c.Get("/");
    CHECK(idx && idx->status == 200);
    CHECK(idx->body.find("WB-TEST-INDEX") != std::string::npos);
    CHECK(idx->get_header_value("Content-Type").rfind("text/html", 0) == 0);

    auto js = c.Get("/app.js");   // ES modules need a JavaScript MIME type
    CHECK(js && js->status == 200);
    CHECK(js->get_header_value("Content-Type").rfind("text/javascript", 0) == 0);
    auto css = c.Get("/style.css");
    CHECK(css && css->get_header_value("Content-Type").rfind("text/css", 0) == 0);
    auto sp = c.Get("/sub%20dir/a.txt");
    CHECK(sp && sp->status == 200 && sp->body == "hello");

    auto nf = c.Get("/nope.html");
    CHECK(nf && nf->status == 404);
    CHECK(body_of(nf)["code"] == "NOT_FOUND");
    auto nf2 = c.Get("/api/does-not-exist");
    CHECK(nf2 && nf2->status == 404);
    CHECK(body_of(nf2).contains("error"));
}

WB_TEST(server_blocks_path_traversal_and_foreign_origin) {
    TestServer s;
    auto c = s.client();
    for (const char* p : {"/../secret.txt", "/%2e%2e/secret.txt", "/..%2fsecret.txt", "/sub%20dir/../../secret.txt"}) {
        auto r = c.Get(p);
        CHECK(r);
        CHECK(r->status != 200);
        CHECK(r->body.find("TOP-SECRET") == std::string::npos);
    }
    // A web page from another site must not be able to call the API.
    httplib::Headers evil = {{"Origin", "http://evil.example"}};
    auto r = c.Post("/api/shutdown", evil, "", "application/json");
    CHECK(r && r->status == 403);
    CHECK(!s.returned.load());
    // Wrong Host header (DNS rebinding) is refused as well.
    httplib::Headers rebind = {{"Host", "evil.example"}};
    auto r2 = c.Get("/api/health", rebind);
    CHECK(r2 && r2->status == 403);
}

WB_TEST(server_shutdown_endpoint_stops_run) {
    TestServer s;
    auto c = s.client();
    auto r = c.Post("/api/shutdown", "", "application/json");
    CHECK(r && r->status == 200);
    CHECK(wait_for([&] { return s.returned.load(); }, 5.0));
}

WB_TEST(server_heartbeat_watchdog) {
    // No heartbeat yet -> must keep running even past the timeout.
    TestServer s(1.0);
    auto c = s.client();
    std::this_thread::sleep_for(1500ms);
    CHECK(!s.returned.load());
    // After the first heartbeat, silence longer than the timeout stops the server.
    auto r = c.Post("/api/heartbeat", "", "application/json");
    CHECK(r && r->status == 200);
    CHECK(wait_for([&] { return s.returned.load(); }, 5.0));
}

// ---- FEA -----------------------------------------------------------------------------------

WB_TEST(server_fea_solve_default_matches_hand_calculation) {
    TestServer s;
    auto c = s.client();
    auto r = post_json(c, "/api/fea/solve", json::object());
    CHECK(r && r->status == 200);
    json j = body_of(r);
    // midpoint error of linear elements = q h^2 / (8 k) = 1e4 * 0.01 / 80 = 1.25 degC
    CHECK_NEAR(j["max_mid_err"].get<double>(), 1.25, 1e-9);
    CHECK(j["max_nodal_err"].get<double>() < 1e-9);
    CHECK(j["x"].size() == 11);
    CHECK(j["x_mid"].size() == 10);
    CHECK_NEAR(j["T"][0].get<double>(), 100.0, 1e-12);
    CHECK_NEAR(j["T"][10].get<double>(), 20.0, 1e-12);
    CHECK(j["params"]["N"] == 10);
    CHECK(j["units"]["T"] == "degC");
    // explicit empty body is accepted too (all parameters optional)
    auto r2 = c.Post("/api/fea/solve", "", "application/json");
    CHECK(r2 && r2->status == 200);
}

WB_TEST(server_fea_solve_csv_export) {
    TestServer s;
    auto c = s.client();
    auto r = post_json(c, "/api/fea/solve?format=csv", json{{"N", 4}});
    CHECK(r && r->status == 200);
    CHECK(r->get_header_value("Content-Type").rfind("text/csv", 0) == 0);
    CHECK(r->get_header_value("Content-Disposition").find("fea_N4.csv") != std::string::npos);
    CHECK(r->body.rfind("x_m,T_fe_C,T_exact_C,abs_err_C\n", 0) == 0);
    CHECK(std::count(r->body.begin(), r->body.end(), '\n') == 1 + 5);   // header + N+1 nodes
}

WB_TEST(server_fea_rejects_bad_parameters) {
    TestServer s;
    auto c = s.client();
    struct Bad { json body; const char* what; };
    std::vector<Bad> bad = {
        {json{{"N", 0}}, "N=0"}, {json{{"N", 2.5}}, "non-integer N"}, {json{{"N", "ten"}}, "string N"},
        {json{{"L", -1}}, "negative L"}, {json{{"k", 0}}, "zero k"}, {json{{"foo", 1}}, "unknown key"},
        {json{{"N", 100000000}}, "huge N"}};
    for (auto& b : bad) {
        auto r = post_json(c, "/api/fea/solve", b.body);
        if (!r || r->status != 400) WB_FAIL_("expected HTTP 400 for " << b.what);
        json j = body_of(r);
        CHECK(j["code"] == "BAD_PARAM");
        CHECK(!j["error"].get<std::string>().empty());
    }
    auto r = c.Post("/api/fea/solve", "{not json", "application/json");
    CHECK(r && r->status == 400);
    CHECK(body_of(r)["code"] == "BAD_PARAM");
    auto r2 = c.Post("/api/fea/solve", "[1,2]", "application/json");
    CHECK(r2 && r2->status == 400);
}

WB_TEST(server_fea_convergence_is_second_order) {
    TestServer s;
    auto c = s.client();
    auto r = post_json(c, "/api/fea/convergence", json{{"Ns", {10, 20, 40, 80}}});
    CHECK(r && r->status == 200);
    json j = body_of(r);
    CHECK(j["points"].size() == 4);
    CHECK_NEAR(j["slope"].get<double>(), 2.0, 0.05);
    CHECK_NEAR(j["points"][0]["max_mid_err"].get<double>(), 1.25, 1e-9);
    auto csv = post_json(c, "/api/fea/convergence?format=csv", json{{"Ns", {10, 20}}});
    CHECK(csv && csv->status == 200);
    CHECK(csv->body.rfind("N,h_m,max_mid_err_C,max_nodal_err_C\n", 0) == 0);
    // bad Ns
    for (json bad : {json{{"Ns", {10}}}, json{{"Ns", {10, 10}}}, json{{"Ns", {10, 0}}}, json{{"Ns", "x"}}}) {
        auto b = post_json(c, "/api/fea/convergence", bad);
        CHECK(b && b->status == 400);
    }
}

// ---- parsers over HTTP --------------------------------------------------------------------

WB_TEST(server_stl_upload_ok_and_error_has_line) {
    TestServer s;
    auto c = s.client();
    auto r = c.Post("/api/geometry/stl", kStl, "text/plain");
    CHECK(r && r->status == 200);
    json j = body_of(r);
    CHECK(j["triangle_count"] == 1);
    CHECK(j["positions"].size() == 9);
    CHECK(j["normals"].size() == 3);
    CHECK(j["name"] == "tri");
    CHECK_NEAR(j["bbox_max"][0].get<double>(), 1.0, 1e-12);

    // vertex line broken on line 5 of the file
    std::string broken = kStl;
    broken.replace(broken.find("vertex 1 0 0"), 12, "vertex 1 x 0");
    auto e = c.Post("/api/geometry/stl", broken, "text/plain");
    CHECK(e && e->status == 422);
    json ej = body_of(e);
    CHECK(ej["code"] == "PARSE_ERROR");
    CHECK(ej["line"] == 5);
    CHECK(!ej["hint"].get<std::string>().empty());

    auto empty = c.Post("/api/geometry/stl", "", "text/plain");
    CHECK(empty && empty->status == 422);
    CHECK(body_of(empty)["line"] == 0);
}

WB_TEST(server_csv_upload_ok_and_malformed_gives_422_with_line) {
    TestServer s;
    auto c = s.client();
    auto ok = c.Post("/api/results/csv", "node_id,x_m,y_m,z_m,temperature_C\n1,0,0,0,20\n2,1,0,0,30.5\n", "text/csv");
    CHECK(ok && ok->status == 200);
    json j = body_of(ok);
    CHECK(j["count"] == 2);
    CHECK(j["nodes"]["id"].size() == 2);
    CHECK_NEAR(j["t_max"].get<double>(), 30.5, 1e-12);
    CHECK(j["units"]["t"] == "degC");

    // line 3 has a non-numeric temperature
    auto bad = c.Post("/api/results/csv", "node_id,x_m,y_m,z_m,temperature_C\n1,0,0,0,20\n2,1,0,0,hot\n", "text/csv");
    CHECK(bad && bad->status == 422);
    json bj = body_of(bad);
    CHECK(bj["code"] == "PARSE_ERROR");
    CHECK(bj["line"] == 3);
    CHECK(!bj["error"].get<std::string>().empty());
    CHECK(!bj["hint"].get<std::string>().empty());

    // a form-style content type must not change how the body is read
    auto form = c.Post("/api/results/csv", "node_id,x_m,y_m,z_m,temperature_C\n1,0,0,0,20\n", "application/x-www-form-urlencoded");
    CHECK(form && form->status == 200);
}

// ---- samples -------------------------------------------------------------------------------------

WB_TEST(server_samples_are_whitelisted) {
    TestServer s;
    auto c = s.client();
    json list = body_of(c.Get("/api/samples"));
    std::vector<std::string> names;
    for (auto& f : list["files"]) names.push_back(f["name"]);
    CHECK(std::find(names.begin(), names.end(), "tri.stl") != names.end());
    CHECK(std::find(names.begin(), names.end(), "ok.csv") != names.end());
    CHECK(std::find(names.begin(), names.end(), "bad/broken.csv") != names.end());
    CHECK(std::find(names.begin(), names.end(), "notes.txt") == names.end());   // wrong extension

    auto r = c.Get("/api/samples/tri.stl");
    CHECK(r && r->status == 200 && r->body == kStl);
    auto b = c.Get("/api/samples/bad/broken.csv");
    CHECK(b && b->status == 200);
    for (const char* p : {"/api/samples/notes.txt", "/api/samples/..%2fweb%2findex.html", "/api/samples/../web/index.html",
                          "/api/samples/bad/../tri.stl", "/api/samples/nothing.stl"}) {
        auto x = c.Get(p);
        CHECK(x && x->status == 404);
    }
}

// If the real data/ folder is populated, every sample must behave as its name says:
// good files parse (HTTP 200), files under bad/ are rejected with a line number (422).
WB_TEST(server_real_data_samples_parse_or_fail_as_labelled) {
    fs::path data = WB_DATA_DIR;
    if (!fs::exists(data)) return;
    TestServer s(30.0, &data);
    auto c = s.client();
    json list = body_of(c.Get("/api/samples"));
    for (auto& f : list["files"]) {
        std::string name = f["name"], kind = f["kind"];
        auto file = c.Get("/api/samples/" + name);
        CHECK(file && file->status == 200);
        auto up = c.Post(kind == "stl" ? "/api/geometry/stl" : "/api/results/csv", file->body, "text/plain");
        CHECK(up);
        bool is_bad = name.rfind("bad/", 0) == 0;
        if (is_bad != (up->status == 422)) WB_FAIL_("sample " << name << " gave HTTP " << up->status);
        if (is_bad) CHECK(body_of(up)["code"] == "PARSE_ERROR");
    }
}

// ---- benchmark job -----------------------------------------------------------------------------

WB_TEST(server_bench_tiny_job_finishes_verified) {
    TestServer s;
    auto c = s.client();
    std::string id = start_bench(c, json{{"cases", 40}, {"mesh", 50}, {"workers", {1, 2}}, {"reps", 2}, {"warmup", 0}});
    CHECK(id == "j1");
    json j;
    CHECK(wait_for([&] { j = job_state(c, id); return j["state"] != "running"; }, 60.0));
    CHECK(j["state"] == "done");
    CHECK(j["error"].is_null());
    CHECK_NEAR(j["progress"].get<double>(), 1.0, 1e-12);
    const json& r = j["result"];
    CHECK(r.is_object());
    CHECK(r["verified"].get<bool>());
    CHECK(r["max_abs_diff"].get<double>() <= 1e-8);
    CHECK(r["serial_times_s"].size() == 2);
    CHECK(r["rows"].size() >= 1);
    CHECK(r["rows"][0]["times_s"].size() == 2);
    CHECK(r["config"]["cases"] == 40);
    CHECK(r["sysinfo"]["logical_cpus"].get<int>() >= 1);
    CHECK(r["capped_workers"].is_array());
    for (auto& w : r["capped_workers"]) CHECK(w.get<int>() <= r["sysinfo"]["logical_cpus"].get<int>());

    auto csv = c.Get(("/api/jobs/" + id + "?format=csv").c_str());
    CHECK(csv && csv->status == 200);
    CHECK(csv->body.rfind("workers,rep,time_s,median_s,speedup,efficiency\n", 0) == 0);
    CHECK(csv->get_header_value("Content-Disposition").find("bench.csv") != std::string::npos);

    // finished -> a new job may start, ids keep counting
    std::string id2 = start_bench(c, json{{"cases", 5}, {"mesh", 20}, {"workers", {1}}, {"reps", 1}, {"warmup", 0}});
    CHECK(id2 == "j2");
    CHECK(wait_for([&] { return job_state(c, id2)["state"] != "running"; }, 60.0));
}

WB_TEST(server_bench_rejects_bad_config) {
    TestServer s;
    auto c = s.client();
    for (json bad : {json{{"cases", 0}}, json{{"cases", 100001}}, json{{"mesh", 1}}, json{{"reps", 21}},
                     json{{"cases", 100000}, {"mesh", 100000}}, json{{"cases", 20001}, {"mesh", 1000}}, json{{"workers", {0}}}, json{{"workers", {65}}},
                     json{{"workers", json::array()}}, json{{"workers", "4"}}, json{{"seed", -1}},
                     json{{"cases", 1.5}}, json{{"bogus", 1}}}) {
        auto r = post_json(c, "/api/bench/start", bad);
        if (!r || r->status != 400) WB_FAIL_("expected 400 for " << bad.dump());
        CHECK(body_of(r)["code"] == "BAD_PARAM");
    }
    auto nf = c.Get("/api/jobs/j999");
    CHECK(nf && nf->status == 404);
    auto nc = c.Post("/api/jobs/j999/cancel", "", "application/json");
    CHECK(nc && nc->status == 404);
}

WB_TEST(server_bench_cancel_gives_cancelled_and_null_result) {
    TestServer s;
    auto c = s.client();
    // big enough to run for many seconds if it were not cancelled
    std::string id = start_bench(c, json{{"cases", 20000}, {"mesh", 1000}, {"workers", {1}}, {"reps", 20}, {"warmup", 1}});
    // only one benchmark at a time
    auto busy = post_json(c, "/api/bench/start", json{{"cases", 5}, {"mesh", 20}});
    CHECK(busy && busy->status == 409);
    CHECK(body_of(busy)["code"] == "BUSY");
    // running: result must be null (never partial)
    json running = job_state(c, id);
    CHECK(running["state"] == "running");
    CHECK(running["result"].is_null());
    // csv of an unfinished job is refused
    auto early = c.Get(("/api/jobs/" + id + "?format=csv").c_str());
    CHECK(early && early->status == 409);

    std::this_thread::sleep_for(100ms);
    auto t0 = std::chrono::steady_clock::now();
    auto cr = c.Post(("/api/jobs/" + id + "/cancel").c_str(), "", "application/json");
    CHECK(cr && cr->status == 200 && body_of(cr)["ok"].get<bool>());
    json j;
    CHECK(wait_for([&] { j = job_state(c, id); return j["state"] != "running"; }, 15.0));
    double react_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(j["state"] == "cancelled");
    CHECK(j["result"].is_null());
    CHECK(react_s < 10.0);

    // cancelling twice is harmless, and a new benchmark can start afterwards
    auto cr2 = c.Post(("/api/jobs/" + id + "/cancel").c_str(), "", "application/json");
    CHECK(cr2 && cr2->status == 200);
    std::string id2 = start_bench(c, json{{"cases", 5}, {"mesh", 20}, {"workers", {1}}, {"reps", 1}, {"warmup", 0}});
    CHECK(wait_for([&] { return job_state(c, id2)["state"] != "running"; }, 60.0));
}

WB_TEST(server_shutdown_cancels_running_job) {
    TestServer s;
    auto c = s.client();
    start_bench(c, json{{"cases", 20000}, {"mesh", 1000}, {"workers", {1}}, {"reps", 20}});
    std::this_thread::sleep_for(100ms);
    auto r = c.Post("/api/shutdown", "", "application/json");
    CHECK(r && r->status == 200);
    CHECK(wait_for([&] { return s.returned.load(); }, 15.0));   // run() joins the job before returning
}

// ---- hardware (simulated) ---------------------------------------------------------------------

WB_TEST(server_hw_timeout_then_reconnect) {
    TestServer s;
    auto c = s.client();
    json d = body_of(c.Get("/api/hw/discover"));
    CHECK(d["simulated"].get<bool>());
    CHECK(d["devices"].size() == 3);
    for (auto& dev : d["devices"]) CHECK(dev["simulated"].get<bool>());

    // telemetry before connecting is a clean, non-throwing answer
    auto pre = c.Get("/api/hw/mcu0/telemetry");
    CHECK(pre && pre->status == 200);
    CHECK(!body_of(pre)["ok"].get<bool>());

    auto con = c.Post("/api/hw/mcu0/connect", "", "application/json");
    CHECK(con && con->status == 200);
    json cj = body_of(con);
    CHECK(cj["ok"].get<bool>());
    CHECK(cj["device"]["state"] == "Connected");

    auto t1 = c.Get("/api/hw/mcu0/telemetry");
    json t1j = body_of(t1);
    CHECK(t1j["ok"].get<bool>());
    CHECK(t1j["code"] == "OK");
    CHECK(t1j["telemetry"]["values"].is_object() && !t1j["telemetry"]["values"].empty());
    CHECK(t1j["telemetry"]["simulated"].get<bool>());

    auto inj = c.Post("/api/hw/mcu0/inject-timeout", "", "application/json");
    CHECK(inj && inj->status == 200 && body_of(inj)["ok"].get<bool>());
    auto t2 = c.Get("/api/hw/mcu0/telemetry");
    CHECK(t2 && t2->status == 200);        // HTTP 200 so the UI can show it inline
    json t2j = body_of(t2);
    CHECK(!t2j["ok"].get<bool>());
    CHECK(t2j["code"] == "TIMEOUT");
    CHECK(t2j["recoverable"].get<bool>());
    CHECK(!t2j["hint"].get<std::string>().empty());

    auto re = c.Post("/api/hw/mcu0/connect", "", "application/json");   // user clicks Reconnect
    CHECK(re && body_of(re)["ok"].get<bool>());
    json t3 = body_of(c.Get("/api/hw/mcu0/telemetry"));
    CHECK(t3["ok"].get<bool>());

    auto dis = c.Post("/api/hw/mcu0/disconnect", "", "application/json");
    CHECK(dis && body_of(dis)["device"]["state"] == "Disconnected");

    auto unk = c.Post("/api/hw/nope/connect", "", "application/json");
    CHECK(unk && unk->status == 404);
    CHECK(body_of(unk)["code"] == "UNKNOWN_DEVICE");

    json log = body_of(c.Get("/api/hw/log"));
    CHECK(log["lines"].is_array() && !log["lines"].empty());
}

// ---- backprop, sysinfo -------------------------------------------------------------------------

WB_TEST(server_backprop_and_sysinfo) {
    TestServer s;
    auto c = s.client();
    auto r = post_json(c, "/api/backprop/run", json{{"steps", 100}});
    CHECK(r && r->status == 200);
    json j = body_of(r);
    CHECK(j["loss"].size() == 101);
    CHECK(j["max_fd_err"].get<double>() < 1e-6);
    CHECK(j["loss"][100].get<double>() < j["loss"][0].get<double>());
    CHECK(j.contains("grads") && j.contains("grads_fd") && j.contains("p1"));
    CHECK(post_json(c, "/api/backprop/run", json{{"steps", -1}})->status == 400);

    json si = body_of(c.Get("/api/sysinfo"));
    CHECK(si["logical_cpus"].get<int>() >= 1);
    CHECK(si.contains("os") && si.contains("compiler") && si.contains("build_type"));
}

// ---- JobManager on its own -----------------------------------------------------------------------

WB_TEST(jobmanager_ids_busy_cancel_and_atomic_result) {
    wb_srv::JobManager jm;
    std::atomic<bool> release{false};
    std::string a = jm.start("bench", [&](wb_srv::JobControl& jc) -> json {
        while (!release.load() && !jc.cancel().load()) std::this_thread::sleep_for(5ms);
        return json{{"answer", 42}};
    });
    CHECK(a == "j1");
    CHECK_THROWS_AS(jm.start("bench", [](wb_srv::JobControl&) { return json(); }), wb_srv::BusyError);
    wb_srv::JobSnapshot snap;
    CHECK(jm.snapshot(a, snap));
    CHECK(snap.state == wb_srv::JobState::Running);
    CHECK(!snap.result);
    CHECK(!jm.snapshot("j77", snap));
    CHECK(!jm.cancel("j77"));

    release = true;   // normal completion -> Done with the result
    CHECK(wait_for([&] { jm.snapshot(a, snap); return snap.state != wb_srv::JobState::Running; }, 5.0));
    CHECK(snap.state == wb_srv::JobState::Done);
    CHECK(snap.result && (*snap.result)["answer"] == 42);

    // cancelled job: result is dropped even though the function returned a value
    std::string b = jm.start("bench", [](wb_srv::JobControl& jc) -> json {
        while (!jc.cancel().load()) std::this_thread::sleep_for(5ms);
        return json{{"partial", true}};
    });
    CHECK(b == "j2");
    CHECK(jm.cancel(b));
    CHECK(wait_for([&] { jm.snapshot(b, snap); return snap.state != wb_srv::JobState::Running; }, 5.0));
    CHECK(snap.state == wb_srv::JobState::Cancelled);
    CHECK(!snap.result);

    // an exception becomes state Error with the message
    std::string e = jm.start("bench", [](wb_srv::JobControl&) -> json { throw std::runtime_error("boom"); });
    CHECK(wait_for([&] { jm.snapshot(e, snap); return snap.state != wb_srv::JobState::Running; }, 5.0));
    CHECK(snap.state == wb_srv::JobState::Error);
    CHECK(snap.error == "boom");
    CHECK(!snap.result);

    // shutdown cancels and joins a job that is still running
    jm.start("bench", [](wb_srv::JobControl& jc) -> json {
        while (!jc.cancel().load()) std::this_thread::sleep_for(5ms);
        return json();
    });
    jm.shutdown();
    CHECK(!jm.any_running());
    CHECK_THROWS_AS(jm.start("bench", [](wb_srv::JobControl&) { return json(); }), wb_srv::BusyError);
}
