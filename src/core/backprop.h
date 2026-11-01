// backprop.h - one-neuron network, manual backprop (no autodiff).
// h = tanh(w x + b), yhat = v h + c, J = 0.5 (yhat - y)^2
#pragma once
#include <vector>

namespace wb {

struct BpParams { double w = 0.5, b = 0.0, v = 1.0, c = 0.0; };
struct BpForward { double z, h, yhat, e, J; };   // e = yhat - y
struct BpGrads { double dw, db, dv, dc; };

BpForward bp_forward(const BpParams& p, double x, double y);
BpGrads bp_grads(const BpParams& p, double x, double y);                 // analytic chain rule
BpGrads bp_fd_grads(const BpParams& p, double x, double y, double eps);  // central differences
BpParams bp_step(const BpParams& p, const BpGrads& g, double lr);        // simultaneous update

struct BpReport {
    double x = 1, y = 2, lr = 0.1, eps = 1e-6;
    BpParams p0;
    BpForward f0;
    BpGrads g;          // analytic at p0
    BpGrads g_fd;       // finite difference at p0
    double max_fd_err;  // max |g - g_fd|
    BpParams p1;        // after one update
    double J1;
    std::vector<double> loss;   // loss[0] = J at p1, then after each of `steps` further updates
};
BpReport run_backprop(int further_steps = 100);

}  // namespace wb
