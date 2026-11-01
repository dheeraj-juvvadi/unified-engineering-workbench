// test_backprop.cpp - hand-derived gradients vs reference numbers and finite differences.
#include <algorithm>
#include <cmath>
#include "backprop.h"
#include "wb_test.h"

using namespace wb;

WB_TEST(backprop_forward_reference_values) {
    BpParams p;   // w=0.5 b=0 v=1 c=0
    BpForward f = bp_forward(p, 1.0, 2.0);
    CHECK_NEAR(f.z, 0.5, 1e-7);
    CHECK_NEAR(f.h, 0.46211716, 1e-7);
    CHECK_NEAR(f.yhat, 0.46211716, 1e-7);
    CHECK_NEAR(f.e, 0.46211716 - 2.0, 1e-7);
    CHECK_NEAR(f.J, 1.18254182, 1e-7);
}

WB_TEST(backprop_gradient_reference_values) {
    BpGrads g = bp_grads(BpParams{}, 1.0, 2.0);
    CHECK_NEAR(g.dw, -1.20946448, 1e-7);
    CHECK_NEAR(g.db, -1.20946448, 1e-7);
    CHECK_NEAR(g.dv, -0.71068205, 1e-7);
    CHECK_NEAR(g.dc, -1.53788284, 1e-7);
}

WB_TEST(backprop_update_reference_values) {
    BpParams p0;
    BpParams p1 = bp_step(p0, bp_grads(p0, 1.0, 2.0), 0.1);
    CHECK_NEAR(p1.w, 0.62094645, 1e-7);
    CHECK_NEAR(p1.b, 0.12094645, 1e-7);
    CHECK_NEAR(p1.v, 1.07106820, 1e-7);
    CHECK_NEAR(p1.c, 0.15378828, 1e-7);
    CHECK_NEAR(bp_forward(p1, 1.0, 2.0).J, 0.68577377, 1e-7);
}

WB_TEST(backprop_matches_finite_differences) {
    BpReport r = run_backprop(100);
    CHECK(r.max_fd_err <= 1e-5);
    CHECK_NEAR(r.g.dw, r.g_fd.dw, 1e-5);
    CHECK_NEAR(r.g.db, r.g_fd.db, 1e-5);
    CHECK_NEAR(r.g.dv, r.g_fd.dv, 1e-5);
    CHECK_NEAR(r.g.dc, r.g_fd.dc, 1e-5);
    // Another point, with the non-trivial (x, y, params) combination.
    BpParams p{-0.7, 0.3, 1.9, -0.4};
    BpGrads a = bp_grads(p, -1.3, 0.8), n = bp_fd_grads(p, -1.3, 0.8, 1e-6);
    CHECK_NEAR(a.dw, n.dw, 1e-5);
    CHECK_NEAR(a.db, n.db, 1e-5);
    CHECK_NEAR(a.dv, n.dv, 1e-5);
    CHECK_NEAR(a.dc, n.dc, 1e-5);
}

WB_TEST(backprop_report_fields) {
    BpReport r = run_backprop(100);
    CHECK_NEAR(r.J1, 0.68577377, 1e-7);
    CHECK_NEAR(r.f0.J, 1.18254182, 1e-7);
    CHECK_NEAR(r.p1.w, 0.62094645, 1e-7);
    CHECK(r.loss.size() == 101);
    CHECK_NEAR(r.loss[0], r.J1, 1e-15);
}

WB_TEST(backprop_loss_decreases_monotonically) {
    BpReport r = run_backprop(100);
    CHECK(r.loss.back() < r.J1);
    for (size_t i = 1; i < r.loss.size(); ++i)
        CHECK(r.loss[i] <= r.loss[i - 1]);
    // zero further steps still gives the single post-update loss
    CHECK(run_backprop(0).loss.size() == 1);
}
