// test_embedded.cpp - simulated MCU / FPGA / JTAG adapters.
#include <algorithm>
#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "embedded_mock.h"
#include "wb_test.h"

namespace {

using wb::ConnState;
using wb::HardwareManager;

bool has(const wb::Telemetry& t, const std::string& key) {
    for (auto& kv : t.values) if (kv.first == key) return true;
    return false;
}

double val(const wb::Telemetry& t, const std::string& key) {
    for (auto& kv : t.values) if (kv.first == key) return kv.second;
    throw wbt::Failure{"telemetry has no value " + key};
}

wb::DeviceInfo find(const std::vector<wb::DeviceInfo>& v, const std::string& id) {
    for (auto& d : v) if (d.id == id) return d;
    throw wbt::Failure{"device not found: " + id};
}

std::string cap(const wb::DeviceInfo& d, const std::string& key) {
    for (auto& kv : d.capabilities) if (kv.first == key) return kv.second;
    return {};
}

}  // namespace

WB_TEST(hw_discover_three_simulated) {
    HardwareManager hw;
    auto devs = hw.discover();
    CHECK(devs.size() == 3);
    std::set<std::string> ids;
    for (auto& d : devs) {
        ids.insert(d.id);
        CHECK(d.simulated);
        CHECK(d.name.rfind("SIM", 0) == 0);          // everything is labelled SIM
        CHECK(d.state == ConnState::Disconnected);
        CHECK(!d.transport.empty() && !d.port.empty() && !d.capabilities.empty());
    }
    CHECK(ids == (std::set<std::string>{"mcu0", "fpga0", "jtag0"}));

    auto mcu = find(devs, "mcu0"), fpga = find(devs, "fpga0"), jtag = find(devs, "jtag0");
    CHECK(mcu.kind == wb::DeviceKind::MCU && fpga.kind == wb::DeviceKind::FPGA && jtag.kind == wb::DeviceKind::JTAG);
    CHECK(cap(mcu, "firmware") == "sim-1.0");
    CHECK(!cap(mcu, "flash").empty() && !cap(mcu, "ram").empty() && !cap(mcu, "adc_channels").empty() && !cap(mcu, "uart_baud").empty());
    CHECK(!cap(fpga, "luts").empty() && !cap(fpga, "io_banks").empty() && !cap(fpga, "bitstream_loaded").empty());
    CHECK(cap(jtag, "role").find("debug interface only") != std::string::npos);
    CHECK(cap(jtag, "signals").find("nSRST") != std::string::npos);
    CHECK(std::string(wb::to_string(wb::DeviceKind::JTAG)) == "JTAG");
}

WB_TEST(hw_connect_telemetry_disconnect) {
    HardwareManager hw;
    for (const char* id : {"mcu0", "fpga0", "jtag0"}) {
        auto c = hw.connect(id);
        CHECK(c.ok && c.code == "OK");
        CHECK(find(hw.discover(), id).state == ConnState::Connected);

        auto t1 = hw.telemetry(id);
        auto t2 = hw.telemetry(id);
        CHECK(t1.status.ok && t1.status.code == "OK" && t2.status.ok);
        CHECK(t1.data.device_id == id && t1.data.simulated);
        CHECK(t2.data.seq == t1.data.seq + 1);
        CHECK(t2.data.t_s > t1.data.t_s);
        CHECK(!t1.data.values.empty());
        CHECK(t1.data.units.size() == t1.data.values.size());

        auto d = hw.disconnect(id);
        CHECK(d.ok);
        CHECK(find(hw.discover(), id).state == ConnState::Disconnected);
    }
}

WB_TEST(hw_telemetry_values_per_kind) {
    HardwareManager hw;
    hw.connect("mcu0"); hw.connect("fpga0"); hw.connect("jtag0");
    for (int i = 0; i < 20; ++i) {
        auto m = hw.telemetry("mcu0").data;
        CHECK(has(m, "vdd_V") && has(m, "die_temp_C") && has(m, "adc0_V") && has(m, "uptime_s"));
        CHECK_NEAR(val(m, "vdd_V"), 3.30, 0.05);
        CHECK(val(m, "die_temp_C") > 25 && val(m, "die_temp_C") < 60);
        CHECK(val(m, "adc0_V") >= 0.0 && val(m, "adc0_V") <= 3.3);
        CHECK(val(m, "uptime_s") > 0);

        auto f = hw.telemetry("fpga0").data;
        CHECK(has(f, "vccint_V") && has(f, "vccio_V") && has(f, "core_temp_C") && has(f, "link_ok"));
        CHECK_NEAR(val(f, "vccint_V"), 1.00, 0.05);
        CHECK_NEAR(val(f, "vccio_V"), 3.30, 0.05);
        CHECK(val(f, "link_ok") == 1.0);

        auto j = hw.telemetry("jtag0").data;
        CHECK(has(j, "vtref_V") && has(j, "tck_kHz"));
        CHECK_NEAR(val(j, "vtref_V"), 3.30, 0.05);
        CHECK(val(j, "tck_kHz") > 0);
    }
}

WB_TEST(hw_deterministic_noise) {
    // Two fresh managers fed the same calls must produce identical numbers.
    HardwareManager a, b;
    a.connect("mcu0"); b.connect("mcu0");
    bool any_variation = false;
    double first = 0;
    for (int i = 0; i < 10; ++i) {
        auto ta = a.telemetry("mcu0").data, tb = b.telemetry("mcu0").data;
        CHECK(ta.values == tb.values);
        double v = val(ta, "vdd_V");
        if (i == 0) first = v; else if (v != first) any_variation = true;
    }
    CHECK(any_variation);   // it is noise, not a constant
}

WB_TEST(hw_not_connected) {
    HardwareManager hw;
    auto t = hw.telemetry("mcu0");
    CHECK(!t.status.ok && t.status.code == "NOT_CONNECTED" && t.status.recoverable);
    CHECK(!t.status.hint.empty());
    CHECK(t.data.values.empty());

    hw.connect("mcu0");
    hw.disconnect("mcu0");
    CHECK(hw.telemetry("mcu0").status.code == "NOT_CONNECTED");
    auto again = hw.disconnect("mcu0");
    CHECK(!again.ok && again.code == "NOT_CONNECTED");
    auto inj = hw.inject_timeout("mcu0");
    CHECK(!inj.ok && inj.code == "NOT_CONNECTED");
}

WB_TEST(hw_already_connected) {
    HardwareManager hw;
    CHECK(hw.connect("jtag0").code == "OK");
    auto r = hw.connect("jtag0");
    CHECK(r.ok && r.code == "ALREADY_CONNECTED");
    CHECK(hw.telemetry("jtag0").status.ok);   // still usable
}

WB_TEST(hw_unknown_device) {
    HardwareManager hw;
    for (auto r : {hw.connect("nope"), hw.disconnect("nope"), hw.inject_timeout("nope"), hw.telemetry("nope").status}) {
        CHECK(!r.ok);
        CHECK(r.code == "UNKNOWN_DEVICE");
        CHECK(!r.recoverable);
        CHECK(!r.hint.empty());
    }
    CHECK(hw.connect("").code == "UNKNOWN_DEVICE");
}

WB_TEST(hw_timeout_and_recovery) {
    HardwareManager hw;
    hw.connect("fpga0");
    CHECK(hw.telemetry("fpga0").status.ok);

    auto inj = hw.inject_timeout("fpga0");
    CHECK(inj.ok);
    CHECK(find(hw.discover(), "fpga0").state == ConnState::Connected);   // fault is armed, not yet tripped

    auto t = hw.telemetry("fpga0");
    CHECK(!t.status.ok);
    CHECK(t.status.code == "TIMEOUT");
    CHECK(t.status.recoverable);
    CHECK(t.status.hint == "Click Reconnect (disconnect + connect) to recover");
    CHECK(t.data.values.empty());
    CHECK(find(hw.discover(), "fpga0").state == ConnState::Fault);

    // still faulted until the user reconnects
    CHECK(hw.telemetry("fpga0").status.code == "TIMEOUT");

    // other devices are unaffected
    hw.connect("mcu0");
    CHECK(hw.telemetry("mcu0").status.ok);

    // Reconnect = disconnect + connect ...
    CHECK(hw.disconnect("fpga0").ok);
    CHECK(hw.connect("fpga0").ok);
    auto ok = hw.telemetry("fpga0");
    CHECK(ok.status.ok && ok.status.code == "OK");
    CHECK(find(hw.discover(), "fpga0").state == ConnState::Connected);
    CHECK(ok.data.seq == 1);

    // ... and a plain connect() straight from Fault also recovers.
    hw.inject_timeout("fpga0");
    CHECK(hw.telemetry("fpga0").status.code == "TIMEOUT");
    auto c = hw.connect("fpga0");
    CHECK(c.ok && c.code == "OK");
    CHECK(hw.telemetry("fpga0").status.ok);

    // the fault is one-shot: no second TIMEOUT without a new inject
    for (int i = 0; i < 5; ++i) CHECK(hw.telemetry("fpga0").status.ok);
}

WB_TEST(hw_disconnect_clears_armed_fault) {
    HardwareManager hw;
    hw.connect("mcu0");
    hw.inject_timeout("mcu0");
    hw.disconnect("mcu0");
    hw.connect("mcu0");
    CHECK(hw.telemetry("mcu0").status.ok);
}

WB_TEST(hw_event_log) {
    HardwareManager hw;
    hw.connect("mcu0");
    hw.telemetry("mcu0");
    hw.inject_timeout("mcu0");
    hw.telemetry("mcu0");
    hw.connect("mcu0");
    hw.connect("ghost");
    auto log = hw.log();
    CHECK(log.size() >= 5);
    bool timeout = false, recovered = false, unknown = false;
    for (auto& l : log) {
        CHECK(l.rfind("[t=", 0) == 0);                       // every line starts with a sim timestamp
        if (l.find("TIMEOUT") != std::string::npos) timeout = true;
        if (l.find("fault cleared") != std::string::npos) recovered = true;
        if (l.find("ghost") != std::string::npos) unknown = true;
    }
    CHECK(timeout && recovered && unknown);
    CHECK(log.front().find("SIMULATED") != std::string::npos);

    // the log is bounded
    for (int i = 0; i < 1000; ++i) hw.connect("ghost");
    CHECK(hw.log().size() <= 200);
}

WB_TEST(hw_thread_safety_smoke) {
    HardwareManager hw;
    for (const char* id : {"mcu0", "fpga0", "jtag0"}) hw.connect(id);

    const int kThreads = 4, kReads = 2000;
    std::atomic<int> bad{0};
    std::vector<std::vector<std::uint64_t>> seqs(kThreads);
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t)
        pool.emplace_back([&, t] {
            for (int i = 0; i < kReads; ++i) {
                auto r = hw.telemetry("mcu0");
                if (!r.status.ok) ++bad; else seqs[t].push_back(r.data.seq);
                if (hw.telemetry(i % 2 ? "fpga0" : "jtag0").status.code != "OK") ++bad;
                if (i % 100 == 0) { hw.discover(); hw.log(); }
            }
        });
    for (auto& th : pool) th.join();
    CHECK(bad == 0);

    // No lost or duplicated samples: the sequence numbers 1..N must each appear exactly once.
    std::vector<std::uint64_t> all;
    for (auto& s : seqs) all.insert(all.end(), s.begin(), s.end());
    std::sort(all.begin(), all.end());
    CHECK(all.size() == std::size_t(kThreads) * kReads);
    for (std::size_t i = 0; i < all.size(); ++i) CHECK(all[i] == i + 1);
}

WB_TEST(hw_thread_safety_connect_fault_mix) {
    // Threads that connect / inject / read at the same time must never crash or deadlock.
    HardwareManager hw;
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; ++t)
        pool.emplace_back([&, t] {
            for (int i = 0; i < 1000; ++i) {
                switch ((i + t) % 4) {
                    case 0: hw.connect("jtag0"); break;
                    case 1: hw.inject_timeout("jtag0"); break;
                    case 2: hw.telemetry("jtag0"); break;
                    case 3: hw.disconnect("jtag0"); break;
                }
            }
        });
    for (auto& th : pool) th.join();
    CHECK(hw.connect("jtag0").ok);   // always recoverable afterwards
    CHECK(hw.telemetry("jtag0").status.ok || hw.telemetry("jtag0").status.code == "TIMEOUT");
}
