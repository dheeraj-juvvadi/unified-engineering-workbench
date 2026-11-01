// report_util.h - small helpers shared by the report tools.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace report {

// Output directory = first argument that is not a --flag (default "results").
// Created if missing. Exits with a message if it cannot be created.
inline std::string output_dir(int argc, char** argv) {
    std::string dir = "results";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) { ++i; continue; }   // skip "--flag value"
        dir = a;
        break;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::cerr << "cannot create output directory '" << dir << "': " << ec.message() << "\n";
        std::exit(1);
    }
    return dir;
}

inline std::string path(const std::string& dir, const std::string& name) {
    return (std::filesystem::path(dir) / name).string();
}

// Open an output file or exit with a message.
inline std::ofstream open(const std::string& dir, const std::string& name) {
    std::ofstream f(path(dir, name));
    if (!f) {
        std::cerr << "cannot write " << path(dir, name) << "\n";
        std::exit(1);
    }
    return f;
}

// printf-style formatting into a std::string (CSV cells, summary lines).
inline std::string fmt(const char* spec, double v) {
    char b[64];
    std::snprintf(b, sizeof b, spec, v);
    return b;
}
// 12 significant digits: enough to show 1e-13 round-off errors in the CSV.
inline std::string g12(double v) { return fmt("%.12g", v); }

}  // namespace report
