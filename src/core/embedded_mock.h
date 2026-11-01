// embedded_mock.h - SIMULATED adapters for an MCU board, an FPGA board and a
// JTAG debug adapter. No real hardware is touched; everything is labelled SIMULATED.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wb {

enum class DeviceKind { MCU, FPGA, JTAG };
enum class ConnState { Disconnected, Connected, Fault };
const char* to_string(DeviceKind k);
const char* to_string(ConnState s);

struct DeviceInfo {
    std::string id;           // "mcu0", "fpga0", "jtag0"
    DeviceKind kind;
    std::string name;         // e.g. "SIM STM32F4-class MCU board"
    std::string transport;    // e.g. "USB-CDC (simulated)"
    std::string port;         // e.g. "SIM-COM3"
    ConnState state = ConnState::Disconnected;
    std::vector<std::pair<std::string, std::string>> capabilities;
    bool simulated = true;
};

struct OpResult {
    bool ok = false;
    std::string code;      // "OK", "TIMEOUT", "NOT_CONNECTED", "UNKNOWN_DEVICE", "ALREADY_CONNECTED"
    std::string message;   // human readable
    std::string hint;      // what the user can do (e.g. "Click Reconnect")
    bool recoverable = true;
};

struct Telemetry {
    std::string device_id;
    std::uint64_t seq = 0;
    double t_s = 0;                                         // simulated time
    std::vector<std::pair<std::string, double>> values;     // e.g. {"vdd_V", 3.30}
    std::vector<std::pair<std::string, std::string>> units; // name -> unit
    bool simulated = true;
};

struct TelemetryResult { OpResult status; Telemetry data; };

// Thread-safe. Deterministic (seeded) telemetry values.
class HardwareManager {
public:
    HardwareManager();
    std::vector<DeviceInfo> discover();            // returns all 3 simulated devices
    OpResult connect(const std::string& id);
    OpResult disconnect(const std::string& id);
    TelemetryResult telemetry(const std::string& id);
    // Arms a one-shot fault: the next telemetry() on `id` returns TIMEOUT and
    // the device enters Fault. A subsequent connect() recovers it.
    OpResult inject_timeout(const std::string& id);
    std::vector<std::string> log() const;          // recent event log lines
private:
    struct Dev { DeviceInfo info; bool fault_armed = false; std::uint64_t seq = 0; double t_connect = 0; };
    mutable std::mutex mu_;
    std::map<std::string, Dev> devs_;
    std::vector<std::string> log_;
    double sim_t_ = 0;                 // simulated clock, advances with every operation
    void add_log(const std::string& line);
};

}  // namespace wb
