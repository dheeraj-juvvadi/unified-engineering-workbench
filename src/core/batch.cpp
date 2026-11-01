// batch.cpp - many independent FEA solves: deterministic generator, serial loop,
// bounded worker pool, and the benchmark driver (timing, speedup, verification).
#include "batch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

namespace wb {

namespace {

using Clock = std::chrono::steady_clock;

// Map a raw 64-bit draw to [0, 1): keep the top 53 bits (a double's mantissa)
// and scale by 2^-53. Deterministic on every compiler, unlike
// std::uniform_real_distribution.
double unit_from_bits(std::uint64_t r) { return static_cast<double>(r >> 11) * 0x1.0p-53; }

bool is_cancelled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

// Solve one case on a `mesh`-element rod using the case's k and q.
CaseResult solve_case(const BatchCase& c, int mesh, const FeaParams& base) {
    FeaParams p = base;
    p.N = mesh;
    p.k = c.k;
    p.q = c.q;
    CaseResult r;
    r.id = c.id;
    r.k = c.k;
    r.q = c.q;
    r.T = solve_fea1d_nodal(p);
    r.T_max = *std::max_element(r.T.begin(), r.T.end());
    return r;
}

// Reports progress roughly once per percent so a UI polling the callback is
// not flooded. Safe to call from several threads.
class ProgressThrottle {
public:
    ProgressThrottle(std::size_t total, ProgressFn fn)
        : total_(total), step_(std::max<std::size_t>(1, total / 100)), fn_(std::move(fn)) {}
    void case_done() {
        std::size_t n = done_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fn_ && (n % step_ == 0 || n == total_)) fn_(static_cast<double>(n) / static_cast<double>(total_));
    }
private:
    std::size_t total_, step_;
    ProgressFn fn_;
    std::atomic<std::size_t> done_{0};
};

}  // namespace

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

std::vector<BatchCase> generate_batch(int cases, std::uint64_t seed) {
    if (cases < 0) throw std::invalid_argument("cases must be >= 0");
    std::mt19937_64 gen(seed);
    std::vector<BatchCase> out;
    out.reserve(cases);
    for (int i = 0; i < cases; ++i) {
        // Two draws per case, always in the same order (k first, then q).
        double uk = unit_from_bits(gen());
        double uq = unit_from_bits(gen());
        out.push_back({i, 1.0 + 99.0 * uk, 1.0e3 + (1.0e5 - 1.0e3) * uq});
    }
    return out;
}

std::vector<CaseResult> run_serial(const std::vector<BatchCase>& cases, int mesh,
                                   const FeaParams& base, const std::atomic<bool>* cancel,
                                   ProgressFn progress) {
    // Validate once up front so a bad mesh/base fails before any work.
    FeaParams probe = base;
    probe.N = mesh;
    validate(probe);

    ProgressThrottle prog(cases.size(), std::move(progress));
    std::vector<CaseResult> out;
    out.reserve(cases.size());
    for (const BatchCase& c : cases) {
        if (is_cancelled(cancel)) return {};
        out.push_back(solve_case(c, mesh, base));
        prog.case_done();
    }
    if (is_cancelled(cancel)) return {};
    return out;
}

std::vector<CaseResult> run_parallel(const std::vector<BatchCase>& cases, int mesh,
                                     const FeaParams& base, int workers,
                                     const std::atomic<bool>* cancel, ProgressFn progress) {
    if (workers < 1) throw std::invalid_argument("workers must be >= 1");
    FeaParams probe = base;
    probe.N = mesh;
    validate(probe);

    const std::size_t n = cases.size();
    std::vector<CaseResult> out(n);          // result i is written only into slot i
    ProgressThrottle prog(n, std::move(progress));

    std::atomic<std::size_t> next{0};        // next case index to claim
    std::atomic<bool> stopped{false};        // some worker saw cancel (or failed)
    std::exception_ptr first_error;          // first exception thrown inside a worker
    std::mutex err_mutex;

    auto worker = [&]() {
        for (;;) {
            if (stopped.load(std::memory_order_relaxed)) return;
            if (is_cancelled(cancel)) { stopped = true; return; }   // checked between cases
            std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) return;
            try {
                out[i] = solve_case(cases[i], mesh, base);
            } catch (...) {
                // An exception must not escape a std::thread (it would call
                // std::terminate). Keep the first one and rethrow after join.
                std::lock_guard<std::mutex> lock(err_mutex);
                if (!first_error) first_error = std::current_exception();
                stopped = true;
                return;
            }
            prog.case_done();
        }
    };

    // Never start more threads than there are cases.
    const int nthreads = static_cast<int>(std::min<std::size_t>(workers, std::max<std::size_t>(n, 1)));
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    try {
        for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    } catch (...) {
        // Could not start a thread: stop the ones already running, then report.
        stopped = true;
        for (auto& th : pool) th.join();
        throw;
    }
    for (auto& th : pool) th.join();

    if (first_error) std::rethrow_exception(first_error);
    if (stopped || is_cancelled(cancel)) return {};
    return out;
}

double max_abs_diff(const std::vector<CaseResult>& a, const std::vector<CaseResult>& b) {
    if (a.size() != b.size()) throw std::invalid_argument("max_abs_diff: different number of cases");
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].T.size() != b[i].T.size())
            throw std::invalid_argument("max_abs_diff: different mesh size in case " + std::to_string(i));
        for (std::size_t j = 0; j < a[i].T.size(); ++j)
            worst = std::max(worst, std::fabs(a[i].T[j] - b[i].T[j]));
    }
    return worst;
}

BenchReport run_benchmark(const BenchConfig& cfg_in, const std::atomic<bool>* cancel,
                          ProgressFn progress) {
    BenchConfig cfg = cfg_in;
    if (cfg.cases < 1) throw std::invalid_argument("cases must be >= 1");
    if (cfg.mesh < 1) throw std::invalid_argument("mesh must be >= 1");
    if (cfg.reps < 1) throw std::invalid_argument("reps must be >= 1");
    if (cfg.warmup < 0) throw std::invalid_argument("warmup must be >= 0");
    if (cfg.workers.empty()) throw std::invalid_argument("workers list is empty");
    for (int w : cfg.workers)
        if (w < 1) throw std::invalid_argument("each worker count must be >= 1");

    // Cap at the number of logical CPUs, drop duplicates the cap creates, sort.
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw > 0)
        for (int& w : cfg.workers) w = std::min(w, hw);
    std::sort(cfg.workers.begin(), cfg.workers.end());
    cfg.workers.erase(std::unique(cfg.workers.begin(), cfg.workers.end()), cfg.workers.end());

    BenchReport rep;
    rep.cfg = cfg;   // records the worker counts actually used

    const FeaParams base;   // default L, A, T0, TL; N/k/q are set per case
    const std::vector<BatchCase> cases = generate_batch(cfg.cases, cfg.seed);

    // Overall progress = finished runs / all runs (warm-ups included).
    const int configs = 1 + static_cast<int>(cfg.workers.size());   // serial + each worker count
    const double total_runs = static_cast<double>(configs) * (cfg.warmup + cfg.reps);
    double runs_done = 0;   // only modified by this thread, between runs
    auto run_progress = [&]() -> ProgressFn {
        if (!progress) return nullptr;
        return [&, total_runs](double f) { progress((runs_done + f) / total_runs); };
    };

    auto cancelled_report = [&]() {
        BenchReport c;
        c.cfg = rep.cfg;
        c.cancelled = true;   // rows stay empty: never a partial table
        return c;
    };
    auto seconds_since = [](Clock::time_point t0) {
        return std::chrono::duration<double>(Clock::now() - t0).count();
    };

    // ---- serial reference ----
    for (int i = 0; i < cfg.warmup; ++i) {
        auto r = run_serial(cases, cfg.mesh, base, cancel, run_progress());
        if (r.empty()) return cancelled_report();
        runs_done += 1;
    }
    std::vector<CaseResult> reference;
    for (int i = 0; i < cfg.reps; ++i) {
        auto t0 = Clock::now();
        auto r = run_serial(cases, cfg.mesh, base, cancel, run_progress());
        double dt = seconds_since(t0);   // stop the clock before anything else touches r
        if (r.empty()) return cancelled_report();
        rep.serial_times_s.push_back(dt);
        if (i == 0) reference = std::move(r);   // keep one copy to verify against
        runs_done += 1;
    }
    rep.serial_median_s = median(rep.serial_times_s);

    // ---- each worker count ----
    for (int p : cfg.workers) {
        for (int i = 0; i < cfg.warmup; ++i) {
            auto r = run_parallel(cases, cfg.mesh, base, p, cancel, run_progress());
            if (r.empty()) return cancelled_report();
            runs_done += 1;
        }
        BenchRow row;
        row.workers = p;
        for (int i = 0; i < cfg.reps; ++i) {
            auto t0 = Clock::now();
            auto r = run_parallel(cases, cfg.mesh, base, p, cancel, run_progress());
            double dt = seconds_since(t0);
            if (r.empty()) return cancelled_report();
            row.times_s.push_back(dt);
            // Verification happens after the clock stopped, so it is not timed.
            rep.max_abs_diff = std::max(rep.max_abs_diff, max_abs_diff(reference, r));
            runs_done += 1;
        }
        row.median_s = median(row.times_s);
        row.speedup = rep.serial_median_s / row.median_s;
        row.efficiency = row.speedup / p;
        rep.rows.push_back(std::move(row));
    }
    rep.verified = rep.max_abs_diff <= 1e-8;
    if (progress) progress(1.0);
    return rep;
}

}  // namespace wb
