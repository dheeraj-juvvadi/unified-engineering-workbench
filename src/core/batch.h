// batch.h - batch of independent 1D FEA solves: generator, serial run, worker pool.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "fea1d.h"

namespace wb {

struct BatchCase { int id; double k; double q; };   // k [W/(m K)] > 0, q [W/m^3] > 0

// Deterministic: same (cases, seed) -> same list. Uses std::mt19937_64 with an
// explicit uniform mapping (not std::uniform_real_distribution, whose output is
// implementation-defined) so results match across compilers.
// k in [1, 100], q in [1e3, 1e5].
std::vector<BatchCase> generate_batch(int cases, std::uint64_t seed);

struct CaseResult { int id; double k; double q; double T_max; std::vector<double> T; };

using ProgressFn = std::function<void(double)>;   // fraction 0..1, may be called from workers

// Every case uses `base` (L, A, T0, TL) with N=mesh and that case's k and q.
// If *cancel becomes true the run stops early and returns an EMPTY vector
// (never a partial one).
std::vector<CaseResult> run_serial(const std::vector<BatchCase>& cases, int mesh,
                                   const FeaParams& base,
                                   const std::atomic<bool>* cancel = nullptr,
                                   ProgressFn progress = nullptr);
// Bounded pool of `workers` std::threads pulling case indices from an atomic
// counter; result i always lands in slot i, so order is deterministic.
std::vector<CaseResult> run_parallel(const std::vector<BatchCase>& cases, int mesh,
                                     const FeaParams& base, int workers,
                                     const std::atomic<bool>* cancel = nullptr,
                                     ProgressFn progress = nullptr);
// Max |T_a - T_b| over all cases and nodes. Throws if shapes differ.
double max_abs_diff(const std::vector<CaseResult>& a, const std::vector<CaseResult>& b);

struct BenchConfig {
    // 3000 x 5000 = 1.5e7 nodes: about 0.25 s serial on a 4-vCPU box (kept small for memory). A result
    // set holds 1.5e7 doubles (120 MB); the benchmark keeps two at once (peak RSS ~350 MB).
    int cases = 3000;
    int mesh = 5000;
    std::uint64_t seed = 42;
    std::vector<int> workers = {1, 2, 4};   // capped at logical CPUs; 1 = pool with 1 thread
    int reps = 5;
    int warmup = 1;
};
struct BenchRow {
    int workers;
    std::vector<double> times_s;   // one per repetition
    double median_s;
    double speedup;                // T1 / Tp, T1 = median of the serial run
    double efficiency;             // speedup / workers
};
struct BenchReport {
    BenchConfig cfg;
    std::vector<double> serial_times_s;
    double serial_median_s = 0;
    std::vector<BenchRow> rows;
    double max_abs_diff = 0;       // serial vs every parallel run (max)
    bool verified = false;         // max_abs_diff <= 1e-8
    bool cancelled = false;
};
// Times only the solve loop (no I/O). Warm-up runs are not timed.
BenchReport run_benchmark(const BenchConfig& cfg, const std::atomic<bool>* cancel = nullptr,
                          ProgressFn progress = nullptr);

double median(std::vector<double> v);

}  // namespace wb
