// test_fea.cpp - 1D FEA: exactness at nodes, midpoint error law, convergence rate.
#include <cmath>
#include <limits>
#include <stdexcept>
#include "fea1d.h"
#include "wb_test.h"

using namespace wb;

// For this problem the exact T is quadratic, and linear elements are nodally
// exact in 1D, so nodal error should only be round-off.
WB_TEST(fea_nodal_values_are_exact) {
    for (int N : {10, 20, 40, 80}) {
        FeaParams p;
        p.N = N;
        FeaResult r = solve_fea1d(p);
        CHECK(r.max_nodal_err < 1e-10);
        CHECK(static_cast<int>(r.T.size()) == N + 1);
        CHECK(static_cast<int>(r.x_mid.size()) == N);
        CHECK_NEAR(r.T.front(), p.T0, 1e-12);
        CHECK_NEAR(r.T.back(), p.TL, 1e-12);
        CHECK_NEAR(r.h, p.L / N, 1e-15);
    }
}

// Midpoint (average of two nodes) overshoots the parabola by q h^2 / (8 k).
WB_TEST(fea_midpoint_error_matches_formula) {
    const double expected[] = {1.25, 0.3125, 0.078125, 0.01953125};
    const int Ns[] = {10, 20, 40, 80};
    for (int i = 0; i < 4; ++i) {
        FeaParams p;
        p.N = Ns[i];
        FeaResult r = solve_fea1d(p);
        double h = p.L / p.N;
        CHECK_NEAR(r.max_mid_err, expected[i], 1e-9);
        CHECK_NEAR(r.max_mid_err, p.q * h * h / (8.0 * p.k), 1e-9);
    }
}

WB_TEST(fea_convergence_slope_is_two) {
    ConvergenceResult c = convergence(FeaParams{}, {10, 20, 40, 80});
    CHECK(c.points.size() == 4);
    CHECK_NEAR(c.slope, 2.0, 0.01);
    CHECK(c.points[0].N == 10);
    CHECK_NEAR(c.points[0].h, 0.1, 1e-15);
    // Error must shrink by 4x each time N doubles.
    for (size_t i = 1; i < c.points.size(); ++i)
        CHECK_NEAR(c.points[i - 1].max_mid_err / c.points[i].max_mid_err, 4.0, 1e-6);
}

WB_TEST(fea_convergence_needs_two_usable_points) {
    CHECK_NEAR(convergence(FeaParams{}, {10}).slope, 0.0, 0.0);
    FeaParams noload;
    noload.q = 0;   // FE is exact everywhere: errors are zero, no log-log fit possible
    CHECK_NEAR(convergence(noload, {10, 20}).slope, 0.0, 0.0);
}

WB_TEST(fea_temperature_at_midrod) {
    // x = 0.5: T = 100 - 80*0.5 + (1e4/(2*10))*0.5*0.5 = 100 - 40 + 125 = 185 degC.
    // N = 10 puts a node exactly there.
    FeaParams p;
    p.N = 10;
    CHECK_NEAR(exact_T(p, 0.5), 185.0, 1e-12);
    FeaResult r = solve_fea1d(p);
    CHECK_NEAR(r.T[5], 185.0, 1e-9);
    CHECK_NEAR(r.x[5], 0.5, 1e-15);
}

WB_TEST(fea_other_parameters_and_boundary_values) {
    // Different k, A, L, BCs and a negative source (sink) - still nodally exact.
    FeaParams p;
    p.N = 33; p.L = 2.5; p.A = 3e-3; p.k = 45.0; p.q = -2.0e4; p.T0 = -10.0; p.TL = 250.0;
    FeaResult r = solve_fea1d(p);
    CHECK(r.max_nodal_err < 1e-9);
    // No source: temperature is the straight line between the BCs.
    p.q = 0;
    r = solve_fea1d(p);
    for (size_t i = 0; i < r.x.size(); ++i)
        CHECK_NEAR(r.T[i], p.T0 + (p.TL - p.T0) * r.x[i] / p.L, 1e-9);
    CHECK_NEAR(r.max_mid_err, 0.0, 1e-9);
}

WB_TEST(fea_single_element_has_no_unknowns) {
    FeaParams p;
    p.N = 1;
    FeaResult r = solve_fea1d(p);
    CHECK(r.T.size() == 2);
    CHECK_NEAR(r.T[0], 100.0, 0);
    CHECK_NEAR(r.T[1], 20.0, 0);
    CHECK_NEAR(r.T_mid_fe[0], 60.0, 1e-12);
}

WB_TEST(fea_nodal_solver_matches_full_solver) {
    FeaParams p;
    p.N = 25;
    FeaResult r = solve_fea1d(p);
    std::vector<double> T = solve_fea1d_nodal(p);
    CHECK(T == r.T);
}

WB_TEST(fea_rejects_invalid_parameters) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    FeaParams p;
    p.N = 0;       CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.N = -3;   CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.N = 10000001; CHECK_THROWS_AS(validate(p), std::invalid_argument);
    p = {}; p.k = 0;    CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.k = -1;   CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.q = nan;  CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.q = inf;  CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.k = nan;  CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.L = 0;    CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.A = -1e-4; CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.T0 = nan; CHECK_THROWS_AS(solve_fea1d(p), std::invalid_argument);
    p = {}; p.TL = inf; CHECK_THROWS_AS(solve_fea1d_nodal(p), std::invalid_argument);
    // convergence() reports a bad N too
    CHECK_THROWS_AS(convergence(FeaParams{}, {10, 0}), std::invalid_argument);
    // defaults are valid
    validate(FeaParams{});
}
