// sysinfo.cpp - describe the machine for the benchmark report.
// One implementation per OS, selected at compile time; each fills what it can
// and leaves the rest at its default ("unknown" / 0) rather than failing.
#include "sysinfo.h"

#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
#  include <sys/types.h>
#elif defined(__linux__)
#  include <sys/utsname.h>
#  include <unistd.h>
#endif

// CMake defines these for wb_core; fall back when compiled by hand.
#ifndef WB_BUILD_FLAGS
#  define WB_BUILD_FLAGS "(not recorded)"
#endif
#ifndef WB_BUILD_TYPE
#  define WB_BUILD_TYPE "(not recorded)"
#endif

namespace wb {

namespace {

std::string compiler_string() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "unknown compiler";
#endif
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

#if defined(_WIN32)

void fill_platform(SysInfo& si) {
    // OS version: RtlGetVersion reports the true version, whereas GetVersionEx
    // lies to programs without a manifest. Looked up at run time from ntdll.
    si.os = "Windows";
    if (HMODULE nt = GetModuleHandleA("ntdll.dll")) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
        OSVERSIONINFOW vi;
        ZeroMemory(&vi, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (fn && fn(&vi) == 0)
            si.os = "Windows " + std::to_string(vi.dwMajorVersion) + "." +
                    std::to_string(vi.dwMinorVersion) + "." + std::to_string(vi.dwBuildNumber);
    }

    // CPU name from the registry.
    char name[256] = {0};
    DWORD size = sizeof(name);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS)
        si.cpu_model = trim(name);

    // Physical cores = number of RelationProcessorCore records.
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);   // first call only asks for the size
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
        std::string buf(len, '\0');
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(&buf[0]);
        if (GetLogicalProcessorInformation(info, &len)) {
            int cores = 0;
            for (DWORD i = 0; i < len / sizeof(*info); ++i)
                if (info[i].Relationship == RelationProcessorCore) ++cores;
            si.physical_cores = cores;
        }
    }

    // Logical CPUs: fall back to GetSystemInfo if the C++ runtime reports 0.
    if (si.logical_cpus == 0) {
        SYSTEM_INFO sys;
        GetSystemInfo(&sys);
        si.logical_cpus = static_cast<int>(sys.dwNumberOfProcessors);
    }

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) si.ram_gb = static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
}

#elif defined(__APPLE__)

std::string sysctl_string(const char* key) {
    size_t len = 0;
    if (sysctlbyname(key, nullptr, &len, nullptr, 0) != 0 || len == 0) return "";
    std::string s(len, '\0');
    if (sysctlbyname(key, &s[0], &len, nullptr, 0) != 0) return "";
    return trim(std::string(s.c_str()));   // drop the trailing NUL
}

void fill_platform(SysInfo& si) {
    std::string ver = sysctl_string("kern.osproductversion");
    si.os = ver.empty() ? "macOS" : "macOS " + ver;
    si.cpu_model = sysctl_string("machdep.cpu.brand_string");

    int phys = 0;
    size_t len = sizeof(phys);
    if (sysctlbyname("hw.physicalcpu", &phys, &len, nullptr, 0) == 0) si.physical_cores = phys;

    int logical = 0;
    len = sizeof(logical);
    if (sysctlbyname("hw.logicalcpu", &logical, &len, nullptr, 0) == 0 && si.logical_cpus == 0)
        si.logical_cpus = logical;

    uint64_t mem = 0;
    len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0)
        si.ram_gb = static_cast<double>(mem) / (1024.0 * 1024.0 * 1024.0);
}

#elif defined(__linux__)

void fill_platform(SysInfo& si) {
    struct utsname u;
    if (uname(&u) == 0) si.os = std::string(u.sysname) + " " + u.release;
    else si.os = "Linux";

    // /proc/cpuinfo: model name, plus (physical id, core id) pairs for the
    // number of distinct physical cores.
    std::ifstream cpu("/proc/cpuinfo");
    std::string line, physical_id = "0";
    std::set<std::pair<std::string, std::string>> cores;
    while (std::getline(cpu, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon)), val = trim(line.substr(colon + 1));
        if (key == "model name" && si.cpu_model.empty()) si.cpu_model = val;
        else if (key == "Model" && si.cpu_model.empty()) si.cpu_model = val;       // ARM boards
        else if (key == "Hardware" && si.cpu_model.empty()) si.cpu_model = val;    // older ARM
        else if (key == "physical id") physical_id = val;
        else if (key == "core id") cores.insert({physical_id, val});
    }
    si.physical_cores = static_cast<int>(cores.size());   // 0 if the file has no core ids

    if (si.logical_cpus == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n > 0) si.logical_cpus = static_cast<int>(n);
    }

    // /proc/meminfo: "MemTotal:   16465560 kB"
    std::ifstream mem("/proc/meminfo");
    while (std::getline(mem, line)) {
        if (line.compare(0, 9, "MemTotal:") == 0) {
            std::istringstream is(line.substr(9));
            double kb = 0;
            if (is >> kb) si.ram_gb = kb / (1024.0 * 1024.0);
            break;
        }
    }
}

#else

void fill_platform(SysInfo& si) { si.os = "unknown OS"; }

#endif

}  // namespace

SysInfo query_sysinfo() {
    SysInfo si;
    si.cpu_model = "";
    si.logical_cpus = static_cast<int>(std::thread::hardware_concurrency());
    fill_platform(si);
    if (si.cpu_model.empty()) si.cpu_model = "unknown";
    if (si.os.empty()) si.os = "unknown";
    si.compiler = compiler_string();
    si.build_flags = WB_BUILD_FLAGS;
    si.build_type = WB_BUILD_TYPE;
    return si;
}

}  // namespace wb
