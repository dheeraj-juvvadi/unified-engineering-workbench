// api_server.cpp - HTTP routes (cpp-httplib) + JSON mapping (nlohmann).
//
// Handlers stay thin: validate parameters -> call wb_core -> map to JSON.
// Errors: wb::ParseError -> 422, std::invalid_argument/BadParam -> 400,
// busy -> 409, anything else -> 500, all with the {"error","code",...} body.
#include "api_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

#include <httplib.h>
#include <json.hpp>

#include "ansys_csv.h"
#include "backprop.h"
#include "batch.h"
#include "embedded_mock.h"
#include "fea1d.h"
#include "job_manager.h"
#include "stl_ascii.h"
#include "sysinfo.h"

namespace fs = std::filesystem;

namespace wb_srv {
namespace {

using Clock = std::chrono::steady_clock;
constexpr const char* kVersion = "1.0.0";
constexpr std::size_t kPayloadMax = 256u * 1024u * 1024u;   // a bit above the 200 MB file limit
constexpr int kMaxFeaN = 1'000'000;                         // keeps responses a sane size

// ---- errors ----------------------------------------------------------------

// A bad request parameter; the message says which one and what is allowed.
struct BadParam : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

// Dump that never throws on invalid UTF-8 (user text can end up in messages).
std::string dump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

void reply_json(httplib::Response& res, int status, const json& j) {
    res.status = status;
    res.set_content(dump(j), "application/json");
}

void reply_error(httplib::Response& res, int status, const char* code, const std::string& msg,
                 const std::string& hint = {}, long line = -1) {
    json j = {{"error", msg}, {"code", code}};
    if (line >= 0) j["line"] = line;
    if (!hint.empty()) j["hint"] = hint;
    reply_json(res, status, j);
}

// ---- parameter helpers -----------------------------------------------------

json parse_body_object(const httplib::Request& req) {
    if (req.body.empty()) return json::object();
    json b = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (b.is_discarded()) throw BadParam("request body is not valid JSON");
    if (!b.is_object()) throw BadParam("request body must be a JSON object");
    return b;
}

void reject_unknown_keys(const json& b, const std::set<std::string>& allowed) {
    for (auto it = b.begin(); it != b.end(); ++it)
        if (!allowed.count(it.key()))
            throw BadParam("unknown parameter '" + it.key() + "'");
}

std::string fmt_g(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

double get_double(const json& b, const char* key, double def, double lo, double hi) {
    auto it = b.find(key);
    if (it == b.end()) return def;
    if (!it->is_number()) throw BadParam(std::string(key) + " must be a number");
    double v = it->get<double>();
    if (!std::isfinite(v) || v < lo || v > hi)
        throw BadParam(std::string(key) + " must be a finite number between " + fmt_g(lo) +
                       " and " + fmt_g(hi) + " (got " + fmt_g(v) + ")");
    return v;
}

long long as_int(const json& v, const std::string& name, long long lo, long long hi) {
    if (!v.is_number()) throw BadParam(name + " must be an integer");
    double d = v.get<double>();
    if (d != std::floor(d)) throw BadParam(name + " must be an integer (got " + fmt_g(d) + ")");
    if (d < static_cast<double>(lo) || d > static_cast<double>(hi))
        throw BadParam(name + " must be between " + std::to_string(lo) + " and " +
                       std::to_string(hi) + " (got " + fmt_g(d) + ")");
    return static_cast<long long>(d);
}

long long get_int(const json& b, const char* key, long long def, long long lo, long long hi) {
    auto it = b.find(key);
    if (it == b.end()) return def;
    return as_int(*it, key, lo, hi);
}

// ---- JSON mapping ------------------------------------------------------------

constexpr double kBig = 1e300;

wb::FeaParams fea_params_from(const json& b) {
    wb::FeaParams p;
    p.N  = static_cast<int>(get_int(b, "N", p.N, 1, kMaxFeaN));
    p.L  = get_double(b, "L",  p.L,  1e-12, kBig);
    p.A  = get_double(b, "A",  p.A,  1e-12, kBig);
    p.k  = get_double(b, "k",  p.k,  1e-12, kBig);
    p.q  = get_double(b, "q",  p.q,  -kBig, kBig);
    p.T0 = get_double(b, "T0", p.T0, -kBig, kBig);
    p.TL = get_double(b, "TL", p.TL, -kBig, kBig);
    wb::validate(p);   // core's own checks (throws std::invalid_argument)
    return p;
}

json fea_params_json(const wb::FeaParams& p) {
    return {{"N", p.N}, {"L", p.L}, {"A", p.A}, {"k", p.k}, {"q", p.q}, {"T0", p.T0}, {"TL", p.TL}};
}

json sysinfo_json(const wb::SysInfo& s) {
    return {{"os", s.os}, {"cpu_model", s.cpu_model}, {"logical_cpus", s.logical_cpus},
            {"physical_cores", s.physical_cores}, {"ram_gb", s.ram_gb}, {"compiler", s.compiler},
            {"build_flags", s.build_flags}, {"build_type", s.build_type}};
}

const wb::SysInfo& cached_sysinfo() {
    static const wb::SysInfo s = wb::query_sysinfo();   // thread-safe one-time init
    return s;
}

json bp_params_json(const wb::BpParams& p) {
    return {{"w", p.w}, {"b", p.b}, {"v", p.v}, {"c", p.c}};
}
json bp_grads_json(const wb::BpGrads& g) {
    return {{"dw", g.dw}, {"db", g.db}, {"dv", g.dv}, {"dc", g.dc}};
}

json device_json(const wb::DeviceInfo& d) {
    json caps = json::object();
    for (auto& kv : d.capabilities) caps[kv.first] = kv.second;
    return {{"id", d.id}, {"kind", wb::to_string(d.kind)}, {"name", d.name},
            {"transport", d.transport}, {"port", d.port}, {"state", wb::to_string(d.state)},
            {"capabilities", caps}, {"simulated", d.simulated}};
}

json op_json(const wb::OpResult& r) {
    return {{"ok", r.ok}, {"code", r.code}, {"message", r.message},
            {"hint", r.hint}, {"recoverable", r.recoverable}};
}

json bench_json(const wb::BenchReport& r, const std::vector<int>& requested,
                const std::vector<int>& used) {
    json rows = json::array();
    for (auto& row : r.rows)
        rows.push_back({{"workers", row.workers}, {"times_s", row.times_s},
                        {"median_s", row.median_s}, {"speedup", row.speedup},
                        {"efficiency", row.efficiency}});
    return {{"config", {{"cases", r.cfg.cases}, {"mesh", r.cfg.mesh}, {"seed", r.cfg.seed},
                        {"workers", r.cfg.workers}, {"reps", r.cfg.reps}, {"warmup", r.cfg.warmup}}},
            {"serial_times_s", r.serial_times_s}, {"serial_median_s", r.serial_median_s},
            {"rows", rows}, {"max_abs_diff", r.max_abs_diff}, {"verified", r.verified},
            {"sysinfo", sysinfo_json(cached_sysinfo())},
            {"requested_workers", requested}, {"capped_workers", used}};
}

// ---- fast number output for big arrays --------------------------------------
// nlohmann would print a float as the digits of its double value (0.100000001490116...),
// and is slow for millions of values, so the two big payloads (STL, CSV) are
// written by hand with the shortest round-trip representation.

template <class T>
void put_num(std::string& out, T v) {
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(v)) { out += "null"; return; }
    }
    char buf[48];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
}

template <class T>
void put_array(std::string& out, const std::vector<T>& v) {
    out += '[';
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ',';
        put_num(out, v[i]);
    }
    out += ']';
}

template <class T>
void put_vec3(std::string& out, const std::array<T, 3>& a) {
    out += '[';
    for (int i = 0; i < 3; ++i) { if (i) out += ','; put_num(out, a[i]); }
    out += ']';
}

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---- CSV output -----------------------------------------------------------------

std::string csv_num(double v) {
    std::string s;
    put_num(s, v);
    return s == "null" ? std::string("nan") : s;
}

// ---- MIME types ------------------------------------------------------------------

const char* mime_for(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    if (e == ".html" || e == ".htm") return "text/html; charset=utf-8";
    if (e == ".js" || e == ".mjs")   return "text/javascript; charset=utf-8";   // required for ES modules
    if (e == ".css")   return "text/css; charset=utf-8";
    if (e == ".json")  return "application/json; charset=utf-8";
    if (e == ".svg")   return "image/svg+xml";
    if (e == ".png")   return "image/png";
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    if (e == ".ico")   return "image/x-icon";
    if (e == ".woff2") return "font/woff2";
    if (e == ".woff")  return "font/woff";
    if (e == ".wasm")  return "application/wasm";
    if (e == ".csv")   return "text/csv; charset=utf-8";
    if (e == ".txt" || e == ".stl" || e == ".md") return "text/plain; charset=utf-8";
    if (e == ".map")   return "application/json";
    return "application/octet-stream";
}

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);   // fs::path keeps non-ASCII folders working on Windows
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

struct Sample { std::string name, kind, group; fs::path path; };

}  // namespace

// ============================================================================
struct ApiServer::Impl {
    fs::path web_dir, data_dir;
    httplib::Server svr;
    JobManager jobs;
    wb::HardwareManager hw;

    int port = 0;
    std::atomic<double> launch_ms{0.0};
    std::function<void(const std::string&)> logger;

    // lifecycle
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> hb_seen{false};
    std::atomic<long long> hb_last_ns{0};          // steady_clock ticks of the last heartbeat
    std::atomic<double> hb_timeout_s{180.0};  // hidden Chrome tabs may only fire timers once a minute
    std::atomic<double> force_exit_s{0.0};
    std::mutex mon_mu;
    std::condition_variable mon_cv;
    bool mon_quit = false;                         // guarded by mon_mu

    Impl(fs::path web, fs::path data) : web_dir(std::move(web)), data_dir(std::move(data)) {
        svr.set_payload_max_length(kPayloadMax);
        svr.set_read_timeout(15, 0);
        svr.set_write_timeout(60, 0);
        svr.set_keep_alive_timeout(2);             // short, so shutdown is not held up by idle sockets
        install_hooks();
        install_routes();
    }

    void log(const std::string& s) { if (logger) logger(s); }

    static long long now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }

    // ---- hooks: security checks, headers, error pages, access log -----------------
    void install_hooks() {
        // Only pages served from this very server may call the API: this blocks
        // other websites in the browser from posting to localhost (CSRF) and DNS rebinding.
        svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            const std::string p = std::to_string(port);
            std::string host = req.get_header_value("Host");
            if (!host.empty() && host != "127.0.0.1:" + p && host != "localhost:" + p &&
                host != "[::1]:" + p) {
                reply_error(res, 403, "BAD_PARAM", "unexpected Host header", "Open the app at http://127.0.0.1:" + p + "/");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.has_header("Origin")) {
                std::string o = req.get_header_value("Origin");
                if (o != "http://127.0.0.1:" + p && o != "http://localhost:" + p) {
                    reply_error(res, 403, "BAD_PARAM", "cross-origin requests are not allowed");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        svr.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
            res.set_header("Cache-Control", "no-store");
            res.set_header("X-Content-Type-Options", "nosniff");
        });
        // Unmatched routes (404/405) and bodiless errors get the standard JSON body.
        svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
            if (!res.body.empty()) return;
            const char* code = res.status == 404 ? "NOT_FOUND" : res.status >= 500 ? "INTERNAL" : "BAD_PARAM";
            std::string msg = res.status == 404 ? "not found"
                            : res.status == 405 ? "method not allowed"
                            : res.status == 413 ? "request body too large"
                            : "request failed (HTTP " + std::to_string(res.status) + ")";
            int st = res.status;
            reply_error(res, st, code, msg);
        });
        // Last resort for exceptions that escape a handler (most are caught by guard()).
        svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            std::string what = "unknown error";
            try { if (ep) std::rethrow_exception(ep); } catch (const std::exception& e) { what = e.what(); } catch (...) {}
            reply_error(res, 500, "INTERNAL", what);
        });
        svr.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
            if (req.path == "/api/heartbeat") return;   // every 5 s, would flood the log
            log(req.method + " " + req.path + " -> " + std::to_string(res.status));
        });
    }

    // Wraps a handler so exceptions become the documented error responses.
    template <class F>
    httplib::Server::Handler guard(F f) {
        return [f](const httplib::Request& req, httplib::Response& res) {
            try {
                f(req, res);
            } catch (const wb::ParseError& e) {
                reply_error(res, 422, "PARSE_ERROR", e.what(),
                            e.hint.empty() ? "Check the file against docs/FORMATS.md" : e.hint,
                            static_cast<long>(e.line));
            } catch (const std::invalid_argument& e) {   // includes BadParam
                reply_error(res, 400, "BAD_PARAM", e.what());
            } catch (const std::bad_alloc&) {
                reply_error(res, 500, "INTERNAL", "out of memory", "Try a smaller input");
            } catch (const std::exception& e) {
                reply_error(res, 500, "INTERNAL", e.what());
            }
        };
    }

    // ---- samples ----------------------------------------------------------------
    // The whitelist is the directory listing itself: a requested name is only
    // served if it equals one of these entries, so no path can escape data/.
    std::vector<Sample> list_samples() const {
        std::vector<Sample> out;
        auto scan = [&](const fs::path& dir, const std::string& prefix, const char* group) {
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                if (ext != ".stl" && ext != ".csv") continue;
                out.push_back({prefix + it->path().filename().string(), ext == ".stl" ? "stl" : "csv",
                               group, it->path()});
            }
        };
        scan(data_dir, "", "good");
        scan(data_dir / "bad", "bad/", "bad");
        std::sort(out.begin(), out.end(), [](const Sample& a, const Sample& b) { return a.name < b.name; });
        return out;
    }

    // ---- static files -------------------------------------------------------------
    void serve_static(const httplib::Request& req, httplib::Response& res) {
        std::string rel = req.path;                      // already percent-decoded by httplib
        fs::path p = web_dir;
        std::size_t i = 0;
        while (i <= rel.size()) {                        // walk the '/'-separated segments
            std::size_t j = rel.find('/', i);
            if (j == std::string::npos) j = rel.size();
            std::string seg = rel.substr(i, j - i);
            i = j + 1;
            if (seg.empty() || seg == ".") continue;
            if (seg == ".." || seg.find_first_of("\\:\0", 0, 3) != std::string::npos) {
                reply_error(res, 404, "NOT_FOUND", "not found");
                return;
            }
            p /= fs::u8path(seg);
        }
        std::error_code ec;
        if (fs::is_directory(p, ec)) p /= "index.html";
        std::string data;
        if (!fs::is_regular_file(p, ec) || !read_file(p, data)) {
            reply_error(res, 404, "NOT_FOUND", "file not found: " + req.path);
            return;
        }
        res.set_content(std::move(data), mime_for(p));
    }

    // ---- routes -------------------------------------------------------------------
    void install_routes() {
        install_lifecycle();
        install_geometry();
        install_fea();
        install_bench();
        install_hw();
        install_backprop();
        svr.Get(R"(/(.*))", [this](const httplib::Request& req, httplib::Response& res) {
            serve_static(req, res);
        });
    }

    void install_lifecycle() {
        svr.Get("/api/health", guard([this](const httplib::Request&, httplib::Response& res) {
            reply_json(res, 200, {{"ok", true}, {"version", kVersion},
                                  {"launch_to_ready_ms", launch_ms.load()}, {"simulated_hw", true}});
        }));
        svr.Post("/api/heartbeat", guard([this](const httplib::Request&, httplib::Response& res) {
            hb_last_ns.store(now_ns());
            hb_seen.store(true);
            reply_json(res, 200, {{"ok", true}});
        }));
        svr.Post("/api/shutdown", guard([this](const httplib::Request&, httplib::Response& res) {
            reply_json(res, 200, {{"ok", true}});
            stop_requested.store(true);   // the monitor thread stops the server after this reply is sent
        }));
        svr.Get("/api/sysinfo", guard([](const httplib::Request&, httplib::Response& res) {
            reply_json(res, 200, sysinfo_json(cached_sysinfo()));
        }));
    }

    void install_geometry() {
        svr.Post("/api/geometry/stl", guard([](const httplib::Request& req, httplib::Response& res) {
            auto t0 = Clock::now();
            wb::StlMesh m = wb::parse_stl_ascii(req.body);
            double parse_ms = ms_since(t0);
            std::string out;
            out.reserve(m.positions.size() * 12 + m.normals.size() * 12 + 512);
            out += "{\"name\":" + dump(json(m.name)) + ",\"triangle_count\":";
            put_num(out, static_cast<unsigned long long>(m.triangle_count));
            out += ",\"positions\":";  put_array(out, m.positions);
            out += ",\"normals\":";    put_array(out, m.normals);
            out += ",\"bbox_min\":";   put_vec3(out, m.bbox_min);
            out += ",\"bbox_max\":";   put_vec3(out, m.bbox_max);
            out += ",\"parse_ms\":";   put_num(out, parse_ms);
            out += '}';
            res.status = 200;
            res.set_content(std::move(out), "application/json");
        }));

        svr.Post("/api/results/csv", guard([](const httplib::Request& req, httplib::Response& res) {
            auto t0 = Clock::now();
            wb::ThermalField f = wb::parse_ansys_csv(req.body);
            double parse_ms = ms_since(t0);
            // Column arrays (id[], x[], ...) are much smaller than an array of objects.
            const std::size_t n = f.nodes.size();
            std::vector<long long> id(n);
            std::vector<double> x(n), y(n), z(n), t(n);
            for (std::size_t i = 0; i < n; ++i) {
                id[i] = f.nodes[i].id; x[i] = f.nodes[i].x; y[i] = f.nodes[i].y;
                z[i] = f.nodes[i].z;   t[i] = f.nodes[i].t;
            }
            std::string out;
            out.reserve(n * 60 + 512);
            out += "{\"count\":";  put_num(out, static_cast<unsigned long long>(n));
            out += ",\"nodes\":{\"id\":"; put_array(out, id);
            out += ",\"x\":"; put_array(out, x);
            out += ",\"y\":"; put_array(out, y);
            out += ",\"z\":"; put_array(out, z);
            out += ",\"t\":"; put_array(out, t);
            out += "},\"t_min\":"; put_num(out, f.t_min);
            out += ",\"t_max\":";  put_num(out, f.t_max);
            out += ",\"bbox_min\":"; put_vec3(out, f.bbox_min);
            out += ",\"bbox_max\":"; put_vec3(out, f.bbox_max);
            out += ",\"units\":{\"xyz\":\"m\",\"t\":\"degC\"},\"parse_ms\":"; put_num(out, parse_ms);
            out += '}';
            res.status = 200;
            res.set_content(std::move(out), "application/json");
        }));

        svr.Get("/api/samples", guard([this](const httplib::Request&, httplib::Response& res) {
            json files = json::array();
            for (auto& s : list_samples()) files.push_back({{"name", s.name}, {"kind", s.kind}, {"group", s.group}});
            reply_json(res, 200, {{"files", files}});
        }));
        svr.Get(R"(/api/samples/(.+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            const std::string name = req.matches[1];
            for (auto& s : list_samples()) {
                if (s.name != name) continue;
                std::string data;
                if (!read_file(s.path, data)) break;
                res.status = 200;
                res.set_content(std::move(data), "text/plain; charset=utf-8");
                return;
            }
            reply_error(res, 404, "NOT_FOUND", "no such sample file: " + name,
                        "GET /api/samples lists the available names");
        }));
    }

    void install_fea() {
        static const std::set<std::string> kFeaKeys = {"N", "L", "A", "k", "q", "T0", "TL"};

        svr.Post("/api/fea/solve", guard([](const httplib::Request& req, httplib::Response& res) {
            json b = parse_body_object(req);
            reject_unknown_keys(b, kFeaKeys);
            wb::FeaParams p = fea_params_from(b);
            wb::FeaResult r = wb::solve_fea1d(p);

            if (req.get_param_value("format") == "csv") {
                std::string out = "x_m,T_fe_C,T_exact_C,abs_err_C\n";
                for (std::size_t i = 0; i < r.x.size(); ++i)
                    out += csv_num(r.x[i]) + "," + csv_num(r.T[i]) + "," + csv_num(r.T_exact[i]) + "," +
                           csv_num(std::fabs(r.T[i] - r.T_exact[i])) + "\n";
                res.status = 200;
                res.set_header("Content-Disposition", "attachment; filename=\"fea_N" + std::to_string(p.N) + ".csv\"");
                res.set_content(std::move(out), "text/csv");
                return;
            }
            reply_json(res, 200, {
                {"params", fea_params_json(p)}, {"h", r.h}, {"x", r.x}, {"T", r.T}, {"T_exact", r.T_exact},
                {"x_mid", r.x_mid}, {"T_mid_fe", r.T_mid_fe}, {"T_mid_exact", r.T_mid_exact},
                {"max_nodal_err", r.max_nodal_err}, {"max_mid_err", r.max_mid_err},
                {"units", {{"x", "m"}, {"T", "degC"}}}});
        }));

        svr.Post("/api/fea/convergence", guard([](const httplib::Request& req, httplib::Response& res) {
            json b = parse_body_object(req);
            std::set<std::string> keys = kFeaKeys;
            keys.insert("Ns");
            reject_unknown_keys(b, keys);
            json base_body = b;
            base_body.erase("Ns");
            wb::FeaParams base = fea_params_from(base_body);   // any "N" given is ignored: Ns drives the sweep
            std::vector<int> Ns = {10, 20, 40, 80};
            if (b.contains("Ns")) {
                const json& a = b["Ns"];
                if (!a.is_array() || a.size() < 2 || a.size() > 40)
                    throw BadParam("Ns must be an array of 2 to 40 integers");
                Ns.clear();
                for (std::size_t i = 0; i < a.size(); ++i)
                    Ns.push_back(static_cast<int>(as_int(a[i], "Ns[" + std::to_string(i) + "]", 1, kMaxFeaN)));
                if (std::set<int>(Ns.begin(), Ns.end()).size() < 2)
                    throw BadParam("Ns needs at least two different values to estimate a slope");
            }
            wb::ConvergenceResult c = wb::convergence(base, Ns);

            if (req.get_param_value("format") == "csv") {
                std::string out = "N,h_m,max_mid_err_C,max_nodal_err_C\n";
                for (auto& pt : c.points)
                    out += std::to_string(pt.N) + "," + csv_num(pt.h) + "," + csv_num(pt.max_mid_err) + "," +
                           csv_num(pt.max_nodal_err) + "\n";
                res.status = 200;
                res.set_header("Content-Disposition", "attachment; filename=\"convergence.csv\"");
                res.set_content(std::move(out), "text/csv");
                return;
            }
            json pts = json::array();
            for (auto& pt : c.points)
                pts.push_back({{"N", pt.N}, {"h", pt.h}, {"max_mid_err", pt.max_mid_err},
                               {"max_nodal_err", pt.max_nodal_err}});
            reply_json(res, 200, {{"points", pts}, {"slope", c.slope}});
        }));
    }

    void install_bench() {
        svr.Post("/api/bench/start", guard([this](const httplib::Request& req, httplib::Response& res) {
            json b = parse_body_object(req);
            reject_unknown_keys(b, {"cases", "mesh", "seed", "workers", "reps", "warmup"});
            wb::BenchConfig cfg;
            cfg.cases  = static_cast<int>(get_int(b, "cases", cfg.cases, 1, 100000));
            cfg.mesh   = static_cast<int>(get_int(b, "mesh", cfg.mesh, 2, 100000));
            cfg.seed   = static_cast<std::uint64_t>(get_int(b, "seed", static_cast<long long>(cfg.seed), 0, 9007199254740991LL));
            cfg.reps   = static_cast<int>(get_int(b, "reps", cfg.reps, 1, 20));
            cfg.warmup = static_cast<int>(get_int(b, "warmup", cfg.warmup, 0, 10));
            if (static_cast<double>(cfg.cases) * cfg.mesh > 2e7)
                throw BadParam("cases * mesh must be at most 2e7 (got " + fmt_g(static_cast<double>(cfg.cases) * cfg.mesh) + ")");

            std::vector<int> requested = cfg.workers;
            if (b.contains("workers")) {
                const json& a = b["workers"];
                if (!a.is_array() || a.empty() || a.size() > 16)
                    throw BadParam("workers must be an array of 1 to 16 integers between 1 and 64");
                requested.clear();
                for (std::size_t i = 0; i < a.size(); ++i)
                    requested.push_back(static_cast<int>(as_int(a[i], "workers[" + std::to_string(i) + "]", 1, 64)));
            }
            // Never run more workers than logical CPUs; the list actually used is reported.
            const int cap = std::max(1, cached_sysinfo().logical_cpus);
            std::vector<int> used;
            for (int w : requested) {
                int c = std::min(w, cap);
                if (std::find(used.begin(), used.end(), c) == used.end()) used.push_back(c);
            }
            cfg.workers = used;

            std::string id;
            try {
                id = jobs.start("bench", [cfg, requested, used](JobControl& jc) -> json {
                    jc.set_message("running benchmark (" + std::to_string(cfg.cases) + " cases, mesh " +
                                   std::to_string(cfg.mesh) + ")");
                    auto last_pct = std::make_shared<std::atomic<int>>(-1);
                    wb::BenchReport rep = wb::run_benchmark(cfg, &jc.cancel(), [&jc, last_pct](double f) {
                        jc.set_progress(f);
                        int pct = static_cast<int>(f * 100);
                        if (last_pct->exchange(pct) != pct) jc.set_message(std::to_string(pct) + "% done");
                    });
                    if (rep.cancelled) return json();         // JobManager sees the cancel flag
                    return bench_json(rep, requested, used);
                });
            } catch (const BusyError& e) {
                reply_error(res, 409, "BUSY", e.what(), "Wait for the running benchmark or cancel it");
                return;
            }
            reply_json(res, 200, {{"job", id}});
        }));

        svr.Get(R"(/api/jobs/([^/]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            JobSnapshot s;
            if (!jobs.snapshot(req.matches[1], s)) {
                reply_error(res, 404, "NOT_FOUND", "no such job: " + std::string(req.matches[1]));
                return;
            }
            if (req.get_param_value("format") == "csv") {
                if (s.state != JobState::Done) {
                    const char* code = s.state == JobState::Running ? "BUSY"
                                     : s.state == JobState::Cancelled ? "CANCELLED" : "INTERNAL";
                    reply_error(res, 409, code, std::string("job is ") + to_string(s.state) + ", no result to export");
                    return;
                }
                res.status = 200;
                res.set_header("Content-Disposition", "attachment; filename=\"bench.csv\"");
                res.set_content(bench_csv(*s.result), "text/csv");
                return;
            }
            json j = {{"id", s.id}, {"kind", s.kind}, {"state", to_string(s.state)},
                      {"progress", s.progress}, {"message", s.message},
                      {"result", s.result ? *s.result : json(nullptr)},
                      {"error", s.state == JobState::Error ? json(s.error) : json(nullptr)}};
            reply_json(res, 200, j);
        }));

        svr.Post(R"(/api/jobs/([^/]+)/cancel)", guard([this](const httplib::Request& req, httplib::Response& res) {
            if (!jobs.cancel(req.matches[1])) {
                reply_error(res, 404, "NOT_FOUND", "no such job: " + std::string(req.matches[1]));
                return;
            }
            reply_json(res, 200, {{"ok", true}});
        }));
    }

    // workers,rep,time_s,median_s,speedup,efficiency (serial baseline first)
    static std::string bench_csv(const json& r) {
        std::string out = "workers,rep,time_s,median_s,speedup,efficiency\n";
        const double smed = r["serial_median_s"].get<double>();
        int rep = 1;
        for (auto& t : r["serial_times_s"])
            out += "serial," + std::to_string(rep++) + "," + csv_num(t.get<double>()) + "," + csv_num(smed) + ",1,1\n";
        for (auto& row : r["rows"]) {
            rep = 1;
            for (auto& t : row["times_s"])
                out += std::to_string(row["workers"].get<int>()) + "," + std::to_string(rep++) + "," +
                       csv_num(t.get<double>()) + "," + csv_num(row["median_s"].get<double>()) + "," +
                       csv_num(row["speedup"].get<double>()) + "," + csv_num(row["efficiency"].get<double>()) + "\n";
        }
        return out;
    }

    void install_hw() {
        svr.Get("/api/hw/discover", guard([this](const httplib::Request&, httplib::Response& res) {
            json devs = json::array();
            for (auto& d : hw.discover()) devs.push_back(device_json(d));
            reply_json(res, 200, {{"simulated", true}, {"devices", devs}});
        }));
        svr.Get("/api/hw/log", guard([this](const httplib::Request&, httplib::Response& res) {
            reply_json(res, 200, {{"lines", hw.log()}});
        }));

        auto op_with_device = [this](bool connect) {
            return guard([this, connect](const httplib::Request& req, httplib::Response& res) {
                const std::string id = req.matches[1];
                wb::OpResult r = connect ? hw.connect(id) : hw.disconnect(id);
                json j = op_json(r);
                for (auto& d : hw.discover())
                    if (d.id == id) j["device"] = device_json(d);
                respond_op(res, r, std::move(j));
            });
        };
        svr.Post(R"(/api/hw/([^/]+)/connect)", op_with_device(true));
        svr.Post(R"(/api/hw/([^/]+)/disconnect)", op_with_device(false));

        svr.Get(R"(/api/hw/([^/]+)/telemetry)", guard([this](const httplib::Request& req, httplib::Response& res) {
            wb::TelemetryResult t = hw.telemetry(req.matches[1]);
            if (!t.status.ok) {   // TIMEOUT etc. are HTTP 200 so the UI can show them inline
                respond_op(res, t.status, op_json(t.status));
                return;
            }
            json values = json::object(), units = json::object();
            for (auto& kv : t.data.values) values[kv.first] = kv.second;
            for (auto& kv : t.data.units) units[kv.first] = kv.second;
            reply_json(res, 200, {{"ok", true}, {"code", t.status.code},
                {"telemetry", {{"device_id", t.data.device_id}, {"seq", t.data.seq}, {"t_s", t.data.t_s},
                               {"values", values}, {"units", units}, {"simulated", t.data.simulated}}}});
        }));
        svr.Post(R"(/api/hw/([^/]+)/inject-timeout)", guard([this](const httplib::Request& req, httplib::Response& res) {
            wb::OpResult r = hw.inject_timeout(req.matches[1]);
            json j = op_json(r);
            if (r.ok) j["message"] = "next telemetry read will time out";
            respond_op(res, r, std::move(j));
        }));
    }

    // Unknown device -> 404; every other outcome is HTTP 200 with ok true/false.
    static void respond_op(httplib::Response& res, const wb::OpResult& r, json j) {
        if (r.code == "UNKNOWN_DEVICE") {
            j["error"] = r.message;
            reply_json(res, 404, j);
        } else {
            reply_json(res, 200, j);
        }
    }

    void install_backprop() {
        svr.Post("/api/backprop/run", guard([](const httplib::Request& req, httplib::Response& res) {
            json b = parse_body_object(req);
            reject_unknown_keys(b, {"steps"});
            int steps = static_cast<int>(get_int(b, "steps", 100, 0, 100000));
            wb::BpReport r = wb::run_backprop(steps);
            reply_json(res, 200, {
                {"x", r.x}, {"y", r.y}, {"lr", r.lr}, {"eps", r.eps},
                {"p0", bp_params_json(r.p0)},
                {"forward", {{"z", r.f0.z}, {"h", r.f0.h}, {"yhat", r.f0.yhat}, {"e", r.f0.e}, {"J", r.f0.J}}},
                {"grads", bp_grads_json(r.g)}, {"grads_fd", bp_grads_json(r.g_fd)},
                {"max_fd_err", r.max_fd_err}, {"p1", bp_params_json(r.p1)}, {"J1", r.J1},
                {"loss", r.loss}});
        }));
    }

    // ---- run loop ------------------------------------------------------------------
    // The monitor thread owns "when to stop": explicit stop request, or no heartbeat
    // for hb_timeout_s (only after the first one, and not while a job is running).
    void monitor() {
        std::unique_lock<std::mutex> lk(mon_mu);
        bool hb_logged = false;
        Clock::time_point stop_seen{};
        bool stop_seen_set = false;
        while (!mon_quit) {
            mon_cv.wait_for(lk, std::chrono::milliseconds(100));
            if (mon_quit) break;
            bool expired = false;
            if (hb_seen.load()) {
                double idle_s = (now_ns() - hb_last_ns.load()) / 1e9;
                expired = idle_s > hb_timeout_s.load() && !jobs.any_running();
            }
            if (expired && !hb_logged) {
                log("no heartbeat for " + fmt_g(hb_timeout_s.load()) + " s: browser closed, exiting");
                hb_logged = true;
                stop_requested.store(true);
            }
            // Repeat until run() has really left listen (stop() before listening is a no-op).
            if (stop_requested.load()) {
                svr.stop();
                if (!stop_seen_set) { stop_seen = Clock::now(); stop_seen_set = true; }
                double lim = force_exit_s.load();
                if (lim > 0 && ms_since(stop_seen) > lim * 1000) {
                    log("shutdown is taking too long, forcing exit");
                    std::_Exit(0);
                }
            }
        }
    }
};

// ============================================================================
ApiServer::ApiServer(fs::path web_dir, fs::path data_dir)
    : p_(new Impl(std::move(web_dir), std::move(data_dir))) {}

ApiServer::~ApiServer() {
    p_->svr.stop();
    p_->jobs.shutdown();
}

bool ApiServer::listen_free_port(int preferred_port) {
    if (preferred_port > 0 && p_->svr.bind_to_port("127.0.0.1", preferred_port)) {
        p_->port = preferred_port;
        return true;
    }
    if (preferred_port > 0) p_->log("port " + std::to_string(preferred_port) + " is busy, using a free port");
    int port = p_->svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) return false;
    p_->port = port;
    return true;
}

int ApiServer::port() const { return p_->port; }

void ApiServer::run() {
    std::thread mon([this] { p_->monitor(); });
    p_->svr.listen_after_bind();
    {
        std::lock_guard<std::mutex> lk(p_->mon_mu);
        p_->mon_quit = true;
    }
    p_->mon_cv.notify_all();
    mon.join();
    p_->jobs.shutdown();   // cancel + join running jobs
    p_->log("server stopped");
}

bool ApiServer::wait_ready(int timeout_ms) {
    auto t0 = Clock::now();
    while (!p_->svr.is_running()) {
        if (ms_since(t0) > timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void ApiServer::stop() {
    p_->stop_requested.store(true);
    p_->mon_cv.notify_all();
}

void ApiServer::request_stop() { p_->stop_requested.store(true); }

void ApiServer::set_launch_to_ready_ms(double ms) { p_->launch_ms.store(ms); }
void ApiServer::set_force_exit_after_stop_s(double s) { p_->force_exit_s.store(s); }
void ApiServer::set_heartbeat_timeout_s(double s) { p_->hb_timeout_s.store(s); }
void ApiServer::set_logger(std::function<void(const std::string&)> fn) { p_->logger = std::move(fn); }

}  // namespace wb_srv
