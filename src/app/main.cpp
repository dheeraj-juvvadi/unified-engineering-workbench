// main.cpp - Launch.exe: starts the local server, opens the browser, exits when
// the browser tab is gone (heartbeat) or the user clicks "Quit".
//
//   Launch [--port N] [--no-browser]
//
// Folder layout it expects next to the executable:
//   Launch.exe   web/   data/        (logs/ is created on first run)
// For developer runs from the build tree it also tries ../src/frontend and ../data.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "api_server.h"

#if defined(_WIN32)
  #include <windows.h>
  #include <shellapi.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
  #include <sys/sysctl.h>
  #include <sys/types.h>
  #include <unistd.h>
#else
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// Fallback clock start if the OS cannot tell us when the process was created.
const Clock::time_point g_main_start = Clock::now();

// ---- logging ------------------------------------------------------------------
class Logger {
public:
    // Tries each candidate folder until one is writable.
    bool open(const std::vector<fs::path>& dirs) {
        for (auto& d : dirs) {
            std::error_code ec;
            fs::create_directories(d, ec);
            fs::path file = d / "workbench.log";
            // Start a fresh file when the old one has grown big.
            auto mode = std::ios::app;
            if (fs::exists(file, ec) && fs::file_size(file, ec) > 1024 * 1024) mode = std::ios::trunc;
            out_.open(file, std::ios::out | mode);
            if (out_) { path_ = file; return true; }
            out_.clear();
        }
        return false;
    }
    void log(const std::string& line) {
        std::lock_guard<std::mutex> lk(mu_);
        std::time_t t = std::time(nullptr);
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        if (out_) { out_ << ts << "  " << line << '\n'; out_.flush(); }
#ifndef _WIN32
        std::fprintf(stderr, "%s  %s\n", ts, line.c_str());
#endif
    }
    const fs::path& path() const { return path_; }
private:
    std::mutex mu_;
    std::ofstream out_;
    fs::path path_;
};

Logger g_log;

// ---- platform helpers -------------------------------------------------------------

fs::path exe_path() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); return fs::path(buf); }
        buf.resize(buf.size() * 2);   // path was truncated: try a bigger buffer
    }
#elif defined(__APPLE__)
    uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::string buf(n, '\0');
    if (_NSGetExecutablePath(&buf[0], &n) != 0) return {};
    std::error_code ec;
    return fs::weakly_canonical(fs::path(buf.c_str()), ec);
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

// Milliseconds since the OS created this process, or -1 if unknown.
double ms_since_process_start() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    GetSystemTimeAsFileTime(&now);
    auto to_u64 = [](FILETIME f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(to_u64(now) - to_u64(created)) / 1e4;   // 100 ns ticks -> ms
#elif defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid())};
    struct kinfo_proc info;
    size_t len = sizeof info;
    if (sysctl(mib, 4, &info, &len, nullptr, 0) != 0 || len == 0) return -1;
    struct timeval start = info.kp_proc.p_un.__p_starttime, now;
    gettimeofday(&now, nullptr);
    return (now.tv_sec - start.tv_sec) * 1e3 + (now.tv_usec - start.tv_usec) / 1e3;
#else
    // /proc/self/stat field 22 = start time in clock ticks since boot; /proc/uptime = seconds since boot.
    std::ifstream st("/proc/self/stat"), up("/proc/uptime");
    std::string line;
    double uptime = 0;
    if (!std::getline(st, line) || !(up >> uptime)) return -1;
    std::size_t rp = line.rfind(')');             // the command name may contain spaces
    if (rp == std::string::npos) return -1;
    std::istringstream rest(line.substr(rp + 2));
    std::string tok;
    for (int i = 3; i <= 22; ++i) if (!(rest >> tok)) return -1;
    long ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) return -1;
    return (uptime - std::stod(tok) / ticks) * 1000.0;
#endif
}

void open_browser(const std::string& url) {
#if defined(_WIN32)
    std::wstring w(url.begin(), url.end());       // URL is plain ASCII
    HINSTANCE r = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        g_log.log("ShellExecute failed; browser not opened");
        std::wstring msg = L"Workbench is running, but your default browser could not be opened.\n\n"
                           L"Open this address in a browser:\n" + w;
        MessageBoxW(nullptr, msg.c_str(), L"Workbench", MB_OK | MB_ICONINFORMATION);
    }
#else
    #ifdef __APPLE__
      const char* opener = "open";
    #else
      const char* opener = "xdg-open";
    #endif
    std::signal(SIGCHLD, SIG_IGN);                // do not leave zombie children
    pid_t pid = fork();
    if (pid == 0) {                               // child: detach and run the opener silently
        setsid();
        if (!std::freopen("/dev/null", "w", stdout)) _exit(127);
        if (!std::freopen("/dev/null", "w", stderr)) _exit(127);
        execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
        _exit(127);                               // opener not installed
    } else if (pid < 0) {
        g_log.log("fork failed; browser not opened");
    }
#endif
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
#endif

// Reports an unrecoverable problem: dialog on Windows (there is no console), stderr elsewhere.
int fatal(const std::string& message) {
    g_log.log("FATAL: " + message);
#ifdef _WIN32
    MessageBoxW(nullptr, widen(message).c_str(), L"Workbench cannot start", MB_OK | MB_ICONERROR);
#else
    std::fprintf(stderr, "Workbench cannot start: %s\n", message.c_str());
#endif
    return 1;
}

// First candidate folder that contains `marker`.
fs::path first_with(const std::vector<fs::path>& candidates, const char* marker) {
    for (auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c / marker, ec)) return c;
    }
    return {};
}

std::vector<fs::path> log_dirs(const fs::path& exe_dir) {
    std::vector<fs::path> dirs = {exe_dir / "logs"};
#ifdef _WIN32
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA")) dirs.push_back(fs::path(la) / "Workbench" / "logs");
#endif
    std::error_code ec;
    dirs.push_back(fs::temp_directory_path(ec) / "Workbench" / "logs");
    return dirs;
}

volatile std::sig_atomic_t g_signalled = 0;
wb_srv::ApiServer* g_server = nullptr;
void on_signal(int) {
    g_signalled = 1;
    if (g_server) g_server->request_stop();       // only sets an atomic flag
}

}  // namespace

int main(int argc, char** argv) {
    int want_port = 0;
    bool no_browser = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-browser") {
            no_browser = true;
        } else if (a == "--port" && i + 1 < argc) {
            want_port = std::atoi(argv[++i]);
            if (want_port < 1 || want_port > 65535) want_port = 0;
        }
        // unknown arguments are ignored on purpose (double-click launches have none)
    }

    fs::path exe = exe_path();
    fs::path exe_dir = exe.empty() ? fs::current_path() : exe.parent_path();
    if (!g_log.open(log_dirs(exe_dir))) std::fprintf(stderr, "warning: cannot write a log file\n");
    g_log.log("Workbench starting, exe=" + exe_dir.string());

    // The packaged layout comes first; the other candidates are for running from a build tree.
    fs::path web = first_with({exe_dir / "web", exe_dir / ".." / "src" / "frontend",
                               exe_dir / ".." / ".." / "src" / "frontend"}, "index.html");
    if (web.empty())
        return fatal("The 'web' folder was not found next to Launch.exe.\n\n"
                     "Keep Launch.exe inside the unzipped Workbench folder (together with the 'web' and "
                     "'data' folders) and start it from there. Do not run it from inside the ZIP file.");
    fs::path data = first_with({exe_dir / "data", exe_dir / ".." / "data", exe_dir / ".." / ".." / "data"}, ".");
    if (data.empty()) {
        g_log.log("warning: 'data' folder not found; sample files will be unavailable");
        data = exe_dir / "data";
    }
    web = fs::weakly_canonical(web);
    g_log.log("web=" + web.string() + "  data=" + data.string());

    try {
        wb_srv::ApiServer server(web, data);
        server.set_force_exit_after_stop_s(3.0);   // never leave a zombie window-less process behind
        server.set_logger([](const std::string& s) { g_log.log(s); });
        if (!server.listen_free_port(want_port))
            return fatal("Could not open a local network port on 127.0.0.1.\n\n"
                         "Another program or a security tool may be blocking local connections. "
                         "Check the log (" + g_log.path().string() + ") and try again.");

        g_server = &server;
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

        const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
        // Once the accept loop runs: record launch-to-ready time, then open the browser.
        std::thread opener([&] {
            if (!server.wait_ready(5000)) return;
            double ms = ms_since_process_start();
            // The OS clock can be coarse (10 ms on Linux), so never report less than the time since main().
            double since_main = std::chrono::duration<double, std::milli>(Clock::now() - g_main_start).count();
            if (ms < since_main || ms > 120000) ms = since_main;
            server.set_launch_to_ready_ms(ms);
            g_log.log("listening on " + url + "  launch_to_ready_ms=" + std::to_string(ms));
            if (!no_browser) open_browser(url);
        });

        server.run();                              // blocks until quit / heartbeat timeout
        opener.join();
        g_server = nullptr;
        g_log.log("Workbench exited normally");
    } catch (const std::exception& e) {
        return fatal(std::string("Unexpected error: ") + e.what() + "\n\nDetails are in " + g_log.path().string());
    }
    return 0;
}
