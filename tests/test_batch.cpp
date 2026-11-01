// test_batch.cpp - generator, serial/parallel agreement, cancellation, benchmark driver.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <thread>
#include "batch.h"
#include "wb_test.h"

using namespace wb;

WB_TEST(batch_generator_is_deterministic) {
    auto a = generate_batch(500, 42), b = generate_batch(500, 42), c = generate_batch(500, 43);
    CHECK(a.size() == 500);
    bool same_c = true;
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].id == static_cast<int>(i));
        CHECK(a[i].k == b[i].k && a[i].q == b[i].q);   // bit-identical, not just close
        if (a[i].k != c[i].k) same_c = false;
    }
    CHECK(!same_c);   // a different seed must change the data
    // A shorter batch is a prefix of a longer one (draws are sequential).
    auto d = generate_batch(100, 42);
    for (size_t i = 0; i < d.size(); ++i) CHECK(d[i].k == a[i].k && d[i].q == a[i].q);
}

WB_TEST(batch_generator_ranges) {
    auto v = generate_batch(5000, 7);
    double kmin = 1e9, kmax = -1e9, qmin = 1e9, qmax = -1e9;
    for (auto& c : v) {
        CHECK(c.k >= 1.0 && c.k <= 100.0);
        CHECK(c.q >= 1e3 && c.q <= 1e5);
        kmin = std::min(kmin, c.k); kmax = std::max(kmax, c.k);
        qmin = std::min(qmin, c.q); qmax = std::max(qmax, c.q);
    }
    // 5000 uniform draws should cover nearly the whole range.
    CHECK(kmin < 3 && kmax > 98);
    CHECK(qmin < 3e3 && qmax > 9.8e4);
    CHECK(generate_batch(0, 1).empty());
    CHECK_THROWS(generate_batch(-1, 1));
}

WB_TEST(batch_serial_results_are_correct) {
    auto cases = generate_batch(20, 5);
    FeaParams base;
    auto r = run_serial(cases, 40, base);
    CHECK(r.size() == 20);
    for (size_t i = 0; i < r.size(); ++i) {
        CHECK(r[i].id == cases[i].id);
        CHECK(r[i].T.size() == 41);
        FeaParams p = base; p.N = 40; p.k = cases[i].k; p.q = cases[i].q;
        // Compare against the exact solution (nodally exact FE).
        for (int j = 0; j <= 40; ++j)
            CHECK_NEAR(r[i].T[j], exact_T(p, j * p.L / 40), 1e-8);
        CHECK(r[i].T_max == *std::max_element(r[i].T.begin(), r[i].T.end()));
    }
}

WB_TEST(batch_parallel_equals_serial) {
    auto cases = generate_batch(123, 99);   // not a multiple of any worker count
    FeaParams base;
    auto ref = run_serial(cases, 200, base);
    for (int w : {1, 2, 3, 4}) {
        auto par = run_parallel(cases, 200, base, w);
        CHECK(par.size() == ref.size());
        CHECK(max_abs_diff(ref, par) <= 1e-8);
        for (size_t i = 0; i < par.size(); ++i) {   // slot i holds case i
            CHECK(par[i].id == ref[i].id);
            CHECK(par[i].k == ref[i].k && par[i].q == ref[i].q);
        }
    }
    // more workers than cases is fine
    auto few = generate_batch(2, 1);
    CHECK(run_parallel(few, 10, base, 8).size() == 2);
    CHECK(run_parallel({}, 10, base, 4).empty());
}

WB_TEST(batch_bad_inputs_throw) {
    auto cases = generate_batch(4, 1);
    FeaParams base;
    CHECK_THROWS_AS(run_parallel(cases, 10, base, 0), std::invalid_argument);
    CHECK_THROWS_AS(run_serial(cases, 0, base), std::invalid_argument);
    CHECK_THROWS_AS(run_parallel(cases, 0, base, 2), std::invalid_argument);
    // A bad case inside a worker thread must surface as an exception, not terminate().
    cases[2].k = -1.0;
    CHECK_THROWS_AS(run_parallel(cases, 10, base, 3), std::invalid_argument);
    CHECK_THROWS_AS(run_serial(cases, 10, base), std::invalid_argument);
    auto a = run_serial(generate_batch(2, 1), 10, base), b = run_serial(generate_batch(3, 1), 10, base);
    CHECK_THROWS_AS(max_abs_diff(a, b), std::invalid_argument);
}

WB_TEST(batch_cancel_returns_empty) {
    auto cases = generate_batch(200, 3);
    FeaParams base;
    std::atomic<bool> cancel{true};   // already cancelled
    CHECK(run_serial(cases, 100, base, &cancel).empty());
    CHECK(run_parallel(cases, 100, base, 4, &cancel).empty());

    // Cancel from the progress callback half way through.
    for (int w : {0, 3}) {
        std::atomic<bool> stop{false};
        auto cb = [&](double f) { if (f >= 0.5) stop = true; };
        auto r = w == 0 ? run_serial(cases, 2000, base, &stop, cb)
                        : run_parallel(cases, 2000, base, w, &stop, cb);
        CHECK(r.empty());
    }
}

WB_TEST(batch_progress_is_monotone_and_ends_at_one) {
    auto cases = generate_batch(300, 3);
    FeaParams base;
    std::atomic<int> calls{0};
    std::atomic<double> last{0};
    std::atomic<bool> bad{false};
    auto cb = [&](double f) {
        if (f < 0 || f > 1) bad = true;
        ++calls;
        last = f;
    };
    run_serial(cases, 20, base, nullptr, cb);
    CHECK(!bad);
    CHECK(last == 1.0);
    CHECK(calls <= 110);   // throttled: about one call per percent
    calls = 0; last = 0;
    run_parallel(cases, 20, base, 4, nullptr, cb);
    CHECK(!bad);
    CHECK(last == 1.0);
    CHECK(calls <= 110);
}

WB_TEST(batch_median) {
    CHECK_NEAR(median({3, 1, 2}), 2.0, 0);
    CHECK_NEAR(median({4, 1, 3, 2}), 2.5, 0);
    CHECK_NEAR(median({7}), 7.0, 0);
    CHECK_NEAR(median({}), 0.0, 0);
}

WB_TEST(bench_small_run_is_consistent) {
    BenchConfig cfg;
    cfg.cases = 60; cfg.mesh = 500; cfg.reps = 3; cfg.warmup = 1;
    cfg.workers = {1, 2, 4, 1024};   // 1024 is capped to the CPU count and de-duplicated
    BenchReport r = run_benchmark(cfg);
    CHECK(!r.cancelled);
    CHECK(r.verified);
    CHECK(r.max_abs_diff <= 1e-8);
    CHECK(r.serial_times_s.size() == 3);
    CHECK(r.serial_median_s > 0);
    CHECK(!r.rows.empty());
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    int prev = 0;
    for (auto& row : r.rows) {
        CHECK(row.workers > prev);                   // sorted, unique
        prev = row.workers;
        if (hw > 0) CHECK(row.workers <= hw);
        CHECK(row.times_s.size() == 3);
        CHECK(row.median_s == median(row.times_s));
        CHECK_NEAR(row.speedup, r.serial_median_s / row.median_s, 1e-12);
        CHECK_NEAR(row.efficiency, row.speedup / row.workers, 1e-12);
    }
    CHECK(r.cfg.workers.size() == r.rows.size());
}

WB_TEST(bench_cancel_gives_empty_rows) {
    BenchConfig cfg;
    cfg.cases = 200; cfg.mesh = 1000; cfg.reps = 3;
    std::atomic<bool> stop{false};
    BenchReport r = run_benchmark(cfg, &stop, [&](double f) { if (f > 0.2) stop = true; });
    CHECK(r.cancelled);
    CHECK(r.rows.empty());
    CHECK(!r.verified);
    std::atomic<bool> already{true};
    CHECK(run_benchmark(cfg, &already).cancelled);
}

WB_TEST(bench_rejects_bad_config) {
    BenchConfig c;
    c.cases = 0;       CHECK_THROWS_AS(run_benchmark(c), std::invalid_argument);
    c = {}; c.mesh = 0;    CHECK_THROWS_AS(run_benchmark(c), std::invalid_argument);
    c = {}; c.reps = 0;    CHECK_THROWS_AS(run_benchmark(c), std::invalid_argument);
    c = {}; c.workers = {}; CHECK_THROWS_AS(run_benchmark(c), std::invalid_argument);
    c = {}; c.workers = {0}; CHECK_THROWS_AS(run_benchmark(c), std::invalid_argument);
}
