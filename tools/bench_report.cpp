// bench_report.cpp - run the serial-vs-parallel FEA benchmark and write
// bench.csv, bench_summary.txt and bench_speedup.svg.
// Usage: bench_report [output_dir] [--cases N] [--mesh N] [--reps N]
//                     [--workers 1,2,4] [--seed N] [--warmup N]
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "batch.h"
#include "report_util.h"
#include "svg_plot.h"
#include "sysinfo.h"

using namespace wb;

static std::vector<int> parse_list(const std::string& s) {
    std::vector<int> v;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) v.push_back(std::atoi(tok.c_str()));
    return v;
}

int main(int argc, char** argv) {
    BenchConfig cfg;   // defaults from batch.h
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) != 0) continue;          // output dir, handled below
        if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; return 2; }
        std::string v = argv[++i];
        if (a == "--cases") cfg.cases = std::atoi(v.c_str());
        else if (a == "--mesh") cfg.mesh = std::atoi(v.c_str());
        else if (a == "--reps") cfg.reps = std::atoi(v.c_str());
        else if (a == "--warmup") cfg.warmup = std::atoi(v.c_str());
        else if (a == "--seed") cfg.seed = std::strtoull(v.c_str(), nullptr, 10);
        else if (a == "--workers") cfg.workers = parse_list(v);
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }
    const std::string dir = report::output_dir(argc, argv);

    std::fprintf(stderr, "benchmark: %d cases x %d elements, %d reps, workers", cfg.cases, cfg.mesh, cfg.reps);
    for (int w : cfg.workers) std::fprintf(stderr, " %d", w);
    std::fprintf(stderr, "\n");

    BenchReport r;
    try {
        int last_pct = -1;
        r = run_benchmark(cfg, nullptr, [&](double f) {
            int pct = static_cast<int>(f * 100);
            if (pct != last_pct && pct % 10 == 0) { last_pct = pct; std::fprintf(stderr, "  %d%%\n", pct); }
        });
    } catch (const std::exception& e) {
        std::cerr << "benchmark failed: " << e.what() << "\n";
        return 1;
    }

    // ---- bench.csv: one line per timed repetition ----
    {
        auto f = report::open(dir, "bench.csv");
        f << "workers,rep,time_s,median_s,speedup,efficiency\n";
        for (size_t i = 0; i < r.serial_times_s.size(); ++i)
            f << "serial," << i + 1 << "," << report::g12(r.serial_times_s[i]) << "," << report::g12(r.serial_median_s)
              << ",1,1\n";
        for (const auto& row : r.rows)
            for (size_t i = 0; i < row.times_s.size(); ++i)
                f << row.workers << "," << i + 1 << "," << report::g12(row.times_s[i]) << "," << report::g12(row.median_s)
                  << "," << report::g12(row.speedup) << "," << report::g12(row.efficiency) << "\n";
    }

    // ---- bench_summary.txt ----
    SysInfo si = query_sysinfo();
    {
        auto f = report::open(dir, "bench_summary.txt");
        f << "Serial vs parallel batch FEA benchmark\n\n";
        f << "System\n";
        f << "  OS:              " << si.os << "\n";
        f << "  CPU:             " << si.cpu_model << "\n";
        f << "  logical CPUs:    " << si.logical_cpus << "\n";
        f << "  physical cores:  " << (si.physical_cores ? std::to_string(si.physical_cores) : "unknown") << "\n";
        f << "  RAM:             " << report::fmt("%.1f", si.ram_gb) << " GiB\n";
        f << "  compiler:        " << si.compiler << "\n";
        f << "  build type:      " << si.build_type << "\n";
        f << "  build flags:     " << si.build_flags << "\n\n";
        f << "Configuration\n";
        f << "  cases:           " << r.cfg.cases << "\n";
        f << "  elements/case:   " << r.cfg.mesh << "\n";
        f << "  seed:            " << r.cfg.seed << "\n";
        f << "  repetitions:     " << r.cfg.reps << " (timed), " << r.cfg.warmup << " warm-up (untimed)\n";
        f << "  workers:         ";
        for (size_t i = 0; i < r.cfg.workers.size(); ++i) f << (i ? "," : "") << r.cfg.workers[i];
        f << " (capped at logical CPUs)\n\n";
        f << "Results (median of " << r.cfg.reps << " runs; only the solve loop is timed, no I/O)\n";
        f << "  serial loop:     " << report::fmt("%.4f", r.serial_median_s) << " s\n";
        f << "  workers   median [s]   speedup   efficiency\n";
        for (const auto& row : r.rows) {
            char b[120];
            std::snprintf(b, sizeof b, "  %7d   %10.4f   %7.2f   %9.1f%%\n", row.workers, row.median_s, row.speedup,
                          row.efficiency * 100.0);
            f << b;
        }
        f << "\nCorrectness\n";
        f << "  max |T_serial - T_parallel| over all cases, nodes and runs: " << report::fmt("%.3e", r.max_abs_diff) << " degC\n";
        f << "  verified (<= 1e-8): " << (r.verified ? "yes" : "NO") << "\n";
    }

    // ---- bench_speedup.svg ----
    svgplot::Plot plot;
    plot.title = "Batch FEA: speedup over the serial loop";
    plot.xlabel = "Worker threads p [-]";
    plot.ylabel = "Speedup T_serial / T_p [-]";
    svgplot::Series ideal, meas;
    ideal.label = "ideal (speedup = p)";
    ideal.color = "#777777";
    ideal.dashed = true;
    meas.label = "measured (median of " + std::to_string(r.cfg.reps) + ")";
    meas.color = svgplot::palette(0);
    meas.marker = svgplot::Marker::Circle;
    for (const auto& row : r.rows) {
        meas.x.push_back(row.workers);
        meas.y.push_back(row.speedup);
        ideal.x.push_back(row.workers);
        ideal.y.push_back(row.workers);
    }
    plot.add(ideal);
    plot.add(meas);
    plot.save(report::path(dir, "bench_speedup.svg"));

    std::printf("serial median %.4f s\n", r.serial_median_s);
    for (const auto& row : r.rows)
        std::printf("workers=%d  median %.4f s  speedup %.2f  efficiency %.1f%%\n", row.workers, row.median_s,
                    row.speedup, row.efficiency * 100);
    std::printf("max diff %.3e, verified=%s\nwrote results to %s\n", r.max_abs_diff, r.verified ? "yes" : "NO",
                dir.c_str());
    return r.verified ? 0 : 1;
}
