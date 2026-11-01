// sysinfo.h - host description for the benchmark report.
#pragma once
#include <string>

namespace wb {

struct SysInfo {
    std::string os;            // e.g. "Windows 10.0.22631", "Linux 6.x", "macOS"
    std::string cpu_model;
    int logical_cpus = 0;      // std::thread::hardware_concurrency() or OS query
    int physical_cores = 0;    // 0 if unknown
    double ram_gb = 0;         // total physical RAM
    std::string compiler;      // from __VERSION__ / _MSC_VER
    std::string build_flags;   // WB_BUILD_FLAGS (set by CMake)
    std::string build_type;    // WB_BUILD_TYPE
};
SysInfo query_sysinfo();

}  // namespace wb
