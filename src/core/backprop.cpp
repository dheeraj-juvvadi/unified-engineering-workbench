// backprop.cpp - one neuron: z = w x + b, h = tanh(z), yhat = v h + c,
// J = 0.5 (yhat - y)^2. Gradients by hand with the chain rule:
//   dJ/dyhat = e                (e = yhat - y)
//   dJ/dv    = e * h            dJ/dc = e
//   dJ/dh    = e * v
//   dJ/dz    = dJ/dh * (1 - h^2)   (derivative of tanh)
//   dJ/dw    = dJ/dz * x        dJ/db = dJ/dz
#include "backprop.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace wb {

BpForward bp_forward(const BpParams& p, double x, double y) {
    BpForward f;
    f.z = p.w * x + p.b;
    f.h = std::tanh(f.z);
    f.yhat = p.v * f.h + p.c;
    f.e = f.yhat - y;
    f.J = 0.5 * f.e * f.e;
    return f;
}

BpGrads bp_grads(const BpParams& p, double x, double y) {
    BpForward f = bp_forward(p, x, y);
    double dh = f.e * p.v;
    double dz = dh * (1.0 - f.h * f.h);
    BpGrads g;
    g.dv = f.e * f.h;
    g.dc = f.e;
    g.dw = dz * x;
    g.db = dz;
    return g;
}

BpGrads bp_fd_grads(const BpParams& p, double x, double y, double eps) {
    if (!(eps > 0)) throw std::invalid_argument("eps must be positive");
    // Central difference for one parameter, selected by member pointer.
    auto fd = [&](double BpParams::*member) {
        BpParams plus = p, minus = p;
        plus.*member += eps;
        minus.*member -= eps;
        return (bp_forward(plus, x, y).J - bp_forward(minus, x, y).J) / (2.0 * eps);
    };
    return {fd(&BpParams::w), fd(&BpParams::b), fd(&BpParams::v), fd(&BpParams::c)};
}

BpParams bp_step(const BpParams& p, const BpGrads& g, double lr) {
    // All four parameters move using the gradients computed at the old point.
    return {p.w - lr * g.dw, p.b - lr * g.db, p.v - lr * g.dv, p.c - lr * g.dc};
}

BpReport run_backprop(int further_steps) {
    if (further_steps < 0) throw std::invalid_argument("steps must be >= 0");
    BpReport r;   // defaults: x=1, y=2, lr=0.1, eps=1e-6, p0={0.5,0,1,0}
    r.f0 = bp_forward(r.p0, r.x, r.y);
    r.g = bp_grads(r.p0, r.x, r.y);
    r.g_fd = bp_fd_grads(r.p0, r.x, r.y, r.eps);
    r.max_fd_err = std::max({std::fabs(r.g.dw - r.g_fd.dw), std::fabs(r.g.db - r.g_fd.db),
                             std::fabs(r.g.dv - r.g_fd.dv), std::fabs(r.g.dc - r.g_fd.dc)});

    r.p1 = bp_step(r.p0, r.g, r.lr);
    r.J1 = bp_forward(r.p1, r.x, r.y).J;

    BpParams p = r.p1;
    r.loss.reserve(further_steps + 1);
    r.loss.push_back(r.J1);
    for (int i = 0; i < further_steps; ++i) {
        p = bp_step(p, bp_grads(p, r.x, r.y), r.lr);
        r.loss.push_back(bp_forward(p, r.x, r.y).J);
    }
    return r;
}

}  // namespace wb
