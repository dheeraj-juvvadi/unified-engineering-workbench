// embedded_mock.cpp - SIMULATED MCU / FPGA / JTAG devices. Nothing here touches real hardware.
#include "embedded_mock.h"

#include <cmath>
#include <cstdio>

namespace wb {
namespace {

constexpr std::size_t kMaxLogLines = 200;
constexpr double kTickS = 0.1;   // simulated seconds per operation

// Deterministic pseudo-noise in [-1, 1): a 32-bit LCG (Numerical Recipes constants) seeded
// from the device seed, the sample number and the channel. Stateless, so the same
// (device, seq, channel) always gives the same value no matter which thread asks.
double noise(std::uint32_t seed, std::uint64_t seq, unsigned channel) {
    std::uint32_t s = seed ^ static_cast<std::uint32_t>(seq * 2654435761u) ^ (channel * 40503u);
    for (int i = 0; i < 3; ++i) s = s * 1664525u + 1013904223u;   // a few rounds to mix the bits
    return (static_cast<double>(s >> 8) / 8388608.0) - 1.0;       // top 24 bits -> [-1, 1)
}

std::uint32_t seed_for(const std::string& id) {
    std::uint32_t h = 2166136261u;                                // FNV-1a of the id
    for (char c : id) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return h;
}

OpResult make_result(bool ok, const char* code, std::string msg, std::string hint = {}, bool recoverable = true) {
    OpResult r;
    r.ok = ok;
    r.code = code;
    r.message = std::move(msg);
    r.hint = std::move(hint);
    r.recoverable = recoverable;
    return r;
}

OpResult unknown_device(const std::string& id) {
    return make_result(false, "UNKNOWN_DEVICE", "Unknown device id '" + id + "' (SIMULATED devices: mcu0, fpga0, jtag0)",
                       "Use one of mcu0, fpga0, jtag0.", false);
}

DeviceInfo make_mcu() {
    DeviceInfo d;
    d.id = "mcu0";
    d.kind = DeviceKind::MCU;
    d.name = "SIM Cortex-M4-class MCU board";
    d.transport = "USB-CDC (simulated)";
    d.port = "SIM-COM3";
    d.capabilities = {{"core", "ARM Cortex-M4F-class, 80 MHz (simulated)"},
                      {"flash", "512 KiB"},
                      {"ram", "128 KiB"},
                      {"io_voltage", "3.3 V"},
                      {"adc_channels", "4 x 12-bit, 0-3.3 V"},
                      {"uart_baud", "115200"},
                      {"firmware", "sim-1.0"}};
    return d;
}

DeviceInfo make_fpga() {
    DeviceInfo d;
    d.id = "fpga0";
    d.kind = DeviceKind::FPGA;
    d.name = "SIM small FPGA board (iCE40/Artix-class)";
    d.transport = "USB-FTDI (simulated)";
    d.port = "SIM-USB1";
    d.capabilities = {{"device_class", "generic iCE40/Artix-class FPGA (simulated)"},
                      {"io_banks", "bank0 3.3 V, bank1 3.3 V, bank2 1.8 V"},
                      {"luts", "7680"},
                      {"core_voltage", "1.00 V"},
                      {"bitstream_loaded", "yes (sim blink.bit)"},
                      {"link", "USB-FTDI (simulated)"}};
    return d;
}

DeviceInfo make_jtag() {
    DeviceInfo d;
    d.id = "jtag0";
    d.kind = DeviceKind::JTAG;
    d.name = "SIM JTAG/SWD debug probe";
    d.transport = "USB (simulated)";
    d.port = "SIM-USB2";
    d.capabilities = {{"signals", "TCK, TMS, TDI, TDO, nTRST, nSRST"},
                      {"protocols", "JTAG, SWD"},
                      {"target_voltage_sense", "VTref, 1.2-5.0 V"},
                      {"max_tck", "10000 kHz"},
                      {"target_idcode", "0x4BA00477 (SIM, Cortex-M4-class DAP)"},
                      {"role", "debug interface only \xE2\x80\x93 does not run user compute"}};
    return d;
}

}  // namespace

const char* to_string(DeviceKind k) {
    switch (k) {
        case DeviceKind::MCU: return "MCU";
        case DeviceKind::FPGA: return "FPGA";
        case DeviceKind::JTAG: return "JTAG";
    }
    return "?";
}

const char* to_string(ConnState s) {
    switch (s) {
        case ConnState::Disconnected: return "Disconnected";
        case ConnState::Connected: return "Connected";
        case ConnState::Fault: return "Fault";
    }
    return "?";
}

HardwareManager::HardwareManager() {
    for (DeviceInfo d : {make_mcu(), make_fpga(), make_jtag()}) {
        std::string id = d.id;
        devs_[id] = Dev{std::move(d)};
    }
    add_log("SIMULATED hardware layer ready: mcu0, fpga0, jtag0 (no real devices are used)");
}

// Callers hold mu_. Lines look like "[t=   1.30s] SIM mcu0: connected".
void HardwareManager::add_log(const std::string& line) {
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "[t=%7.2fs] ", sim_t_);
    log_.push_back(stamp + line);
    if (log_.size() > kMaxLogLines) log_.erase(log_.begin(), log_.begin() + (log_.size() - kMaxLogLines));
}

std::vector<DeviceInfo> HardwareManager::discover() {
    std::lock_guard<std::mutex> lk(mu_);
    sim_t_ += kTickS;
    std::vector<DeviceInfo> out;
    for (const auto& kv : devs_) out.push_back(kv.second.info);
    add_log("SIM discover: found " + std::to_string(out.size()) + " simulated devices");
    return out;
}

OpResult HardwareManager::connect(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    sim_t_ += kTickS;
    auto it = devs_.find(id);
    if (it == devs_.end()) { add_log("SIM connect " + id + ": unknown device"); return unknown_device(id); }
    Dev& d = it->second;
    if (d.info.state == ConnState::Connected)
        return make_result(true, "ALREADY_CONNECTED", d.info.name + " is already connected");
    bool recovering = d.info.state == ConnState::Fault;
    d.info.state = ConnState::Connected;
    d.fault_armed = false;
    d.seq = 0;
    d.t_connect = sim_t_;
    add_log("SIM " + id + (recovering ? ": reconnected, fault cleared" : ": connected") + " via " + d.info.port);
    return make_result(true, "OK", d.info.name + (recovering ? " reconnected; fault cleared" : " connected"));
}

OpResult HardwareManager::disconnect(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    sim_t_ += kTickS;
    auto it = devs_.find(id);
    if (it == devs_.end()) { add_log("SIM disconnect " + id + ": unknown device"); return unknown_device(id); }
    Dev& d = it->second;
    if (d.info.state == ConnState::Disconnected)
        return make_result(false, "NOT_CONNECTED", d.info.name + " is not connected", "Nothing to do; click Connect to use it.");
    d.info.state = ConnState::Disconnected;
    d.fault_armed = false;
    add_log("SIM " + id + ": disconnected");
    return make_result(true, "OK", d.info.name + " disconnected");
}

OpResult HardwareManager::inject_timeout(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    sim_t_ += kTickS;
    auto it = devs_.find(id);
    if (it == devs_.end()) { add_log("SIM inject-timeout " + id + ": unknown device"); return unknown_device(id); }
    Dev& d = it->second;
    if (d.info.state != ConnState::Connected)
        return make_result(false, "NOT_CONNECTED", d.info.name + " is not connected, so there is nothing to time out",
                           "Click Connect first, then inject the timeout.");
    d.fault_armed = true;
    add_log("SIM " + id + ": timeout fault armed (next telemetry read will fail)");
    return make_result(true, "OK", "next telemetry read will time out");
}

TelemetryResult HardwareManager::telemetry(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    sim_t_ += kTickS;
    TelemetryResult res;
    res.data.device_id = id;

    auto it = devs_.find(id);
    if (it == devs_.end()) { add_log("SIM telemetry " + id + ": unknown device"); res.status = unknown_device(id); return res; }
    Dev& d = it->second;

    if (d.info.state == ConnState::Disconnected) {
        res.status = make_result(false, "NOT_CONNECTED", d.info.name + " is not connected", "Click Connect first.");
        return res;
    }
    // A fault is armed (or already tripped): the read times out until the user reconnects.
    if (d.fault_armed || d.info.state == ConnState::Fault) {
        bool first = d.info.state != ConnState::Fault;
        d.fault_armed = false;
        d.info.state = ConnState::Fault;
        if (first) add_log("SIM " + id + ": telemetry read TIMEOUT, device in Fault state");
        res.status = make_result(false, "TIMEOUT", "SIMULATED " + d.info.name + ": no response within 500 ms",
                                 "Click Reconnect (disconnect + connect) to recover");
        return res;
    }

    ++d.seq;
    const std::uint32_t seed = seed_for(id);
    const double t = sim_t_;
    const double up = t - d.t_connect;
    Telemetry& tm = res.data;
    tm.seq = d.seq;
    tm.t_s = t;

    auto add = [&tm](const char* name, double v, const char* unit) {
        tm.values.emplace_back(name, v);
        tm.units.emplace_back(name, unit);
    };
    switch (d.info.kind) {
        case DeviceKind::MCU:
            add("vdd_V", 3.30 + 0.02 * noise(seed, d.seq, 0), "V");
            add("die_temp_C", 32.0 + 6.0 * (1.0 - std::exp(-up / 60.0)) + 0.3 * noise(seed, d.seq, 1), "degC");
            add("adc0_V", 1.65 + 0.50 * std::sin(0.4 * up) + 0.01 * noise(seed, d.seq, 2), "V");
            add("uptime_s", up, "s");
            break;
        case DeviceKind::FPGA:
            add("vccint_V", 1.00 + 0.01 * noise(seed, d.seq, 0), "V");
            add("vccio_V", 3.30 + 0.02 * noise(seed, d.seq, 1), "V");
            add("core_temp_C", 41.0 + 8.0 * (1.0 - std::exp(-up / 90.0)) + 0.4 * noise(seed, d.seq, 2), "degC");
            add("link_ok", 1.0, "bool");
            break;
        case DeviceKind::JTAG:
            add("vtref_V", 3.30 + 0.02 * noise(seed, d.seq, 0), "V");
            add("tck_kHz", 4000.0, "kHz");
            add("target_present", 1.0, "bool");
            break;
    }
    res.status = make_result(true, "OK", "SIMULATED telemetry");
    return res;
}

std::vector<std::string> HardwareManager::log() const {
    std::lock_guard<std::mutex> lk(mu_);
    return log_;
}

}  // namespace wb
