// api_server.h - the local HTTP server (routes in docs/API.md).
//
// Thin layer only: every route parses the request, calls wb_core, and maps the
// result to JSON. No numerical or parsing logic lives here.
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace wb_srv {

class ApiServer {
public:
    // web_dir: folder with index.html etc.   data_dir: folder with sample files
    // (data_dir/*.stl|csv and data_dir/bad/*.stl|csv are served as samples).
    ApiServer(std::filesystem::path web_dir, std::filesystem::path data_dir);
    ~ApiServer();
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // Binds 127.0.0.1 only. preferred_port = 0 picks any free port; if a
    // non-zero port is busy we fall back to a free one. Returns false on failure.
    bool listen_free_port(int preferred_port = 0);
    int port() const;

    // Waits (polling) until run() is accepting connections; false on timeout.
    bool wait_ready(int timeout_ms);

    // Blocks until stop() / POST /api/shutdown / heartbeat timeout. Running
    // jobs are cancelled and joined before it returns.
    void run();
    // Thread-safe: asks run() to return.
    void stop();
    // Same, but only sets an atomic flag, so it may be called from a signal handler.
    void request_stop();

    // Shown by /api/health (process start -> listening).
    void set_launch_to_ready_ms(double ms);
    // Seconds without heartbeat before the server exits (default 180 - background tabs are throttled to ~1 timer/min; tests use less).
    void set_heartbeat_timeout_s(double s);
    // If run() has not returned this many seconds after a stop was requested (e.g. a
    // client holds a connection open), the process ends via std::_Exit. 0 = never (default).
    void set_force_exit_after_stop_s(double s);
    // Optional log sink (one line per call, no trailing newline).
    void set_logger(std::function<void(const std::string&)> fn);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace wb_srv
