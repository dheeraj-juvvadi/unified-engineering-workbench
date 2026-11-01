// fea1d.cpp - 1D steady heat conduction, linear 2-node elements.
//
// Element e joins nodes e and e+1 (length h = L/N):
//   Ke = (kA/h) [ 1 -1; -1 1 ]      Fe = (q A h / 2) [1; 1]
// The assembled global K is tridiagonal, so it is stored as three diagonals
// instead of a dense (N+1)x(N+1) matrix. Dirichlet nodes (0 and N) are
// eliminated: their known values are moved to the right-hand side and only the
// interior N-1 unknowns are solved with the Thomas algorithm.
#include "fea1d.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace wb {

namespace {

// Thomas algorithm for a tridiagonal system of size m.
//   lower[i] multiplies x[i-1] (lower[0] unused), diag[i] multiplies x[i],
//   upper[i] multiplies x[i+1] (upper[m-1] unused).
// rhs is overwritten with the solution. `work` must hold m doubles.
// No pivoting: K with Dirichlet rows removed is symmetric positive definite
// (diagonally dominant), so the forward sweep cannot hit a zero pivot.
void thomas(const double* lower, const double* diag, const double* upper,
            double* rhs, double* work, int m) {
    work[0] = upper[0] / diag[0];
    rhs[0] /= diag[0];
    for (int i = 1; i < m; ++i) {
        double denom = diag[i] - lower[i] * work[i - 1];
        work[i] = upper[i] / denom;
        rhs[i] = (rhs[i] - lower[i] * rhs[i - 1]) / denom;
    }
    for (int i = m - 2; i >= 0; --i) rhs[i] -= work[i] * rhs[i + 1];
}

}  // namespace

void validate(const FeaParams& p) {
    if (p.N < 1) throw std::invalid_argument("N must be at least 1 (got " + std::to_string(p.N) + ")");
    if (p.N > 10000000) throw std::invalid_argument("N must be at most 1e7 (got " + std::to_string(p.N) + ")");
    // !(x > 0) also rejects NaN; isfinite rejects +inf.
    if (!(p.L > 0) || !std::isfinite(p.L)) throw std::invalid_argument("L must be a positive finite number [m]");
    if (!(p.A > 0) || !std::isfinite(p.A)) throw std::invalid_argument("A must be a positive finite number [m^2]");
    if (!(p.k > 0) || !std::isfinite(p.k)) throw std::invalid_argument("k must be a positive finite number [W/(m K)]");
    if (!std::isfinite(p.q)) throw std::invalid_argument("q must be a finite number [W/m^3]");
    if (!std::isfinite(p.T0)) throw std::invalid_argument("T0 must be a finite number [degC]");
    if (!std::isfinite(p.TL)) throw std::invalid_argument("TL must be a finite number [degC]");
}

double exact_T(const FeaParams& p, double x) {
    return p.T0 + (p.TL - p.T0) * x / p.L + p.q / (2.0 * p.k) * x * (p.L - x);
}

std::vector<double> solve_fea1d_nodal(const FeaParams& p) {
    validate(p);
    const int N = p.N;
    const double h = p.L / N;

    std::vector<double> T(N + 1);
    T[0] = p.T0;
    T[N] = p.TL;
    if (N == 1) return T;   // both nodes are prescribed, nothing to solve

    // --- assemble: loop over elements, add each element's contribution ---
    const double ke = p.k * p.A / h;          // element stiffness kA/h
    const double fe = p.q * p.A * h / 2.0;    // element load share per node
    std::vector<double> diag(N + 1, 0.0), off(N, 0.0), F(N + 1, 0.0);
    for (int e = 0; e < N; ++e) {
        diag[e] += ke;
        diag[e + 1] += ke;
        off[e] -= ke;          // K(e, e+1) = K(e+1, e) = -kA/h (symmetric)
        F[e] += fe;
        F[e + 1] += fe;
    }

    // --- Dirichlet elimination: unknowns are nodes 1..N-1 (m = N-1 of them) ---
    // Row i of K T = F reads K(i,i-1) T[i-1] + K(i,i) T[i] + K(i,i+1) T[i+1] = F[i].
    // For i = 1 and i = N-1 one neighbour is a known value; move it to the RHS.
    const int m = N - 1;
    double* rhs = F.data() + 1;          // F[1..N-1] becomes the reduced RHS (in place)
    rhs[0] -= off[0] * p.T0;             // K(1,0)   * T0
    rhs[m - 1] -= off[N - 1] * p.TL;     // K(N-1,N) * TL

    // Interior block, row j <-> node j+1:
    //   lower = K(j+1, j)   = off[j]      -> off.data()
    //   diag  = K(j+1, j+1) = diag[j+1]   -> diag.data() + 1
    //   upper = K(j+1, j+2) = off[j+1]    -> off.data() + 1
    std::vector<double> work(m);
    thomas(off.data(), diag.data() + 1, off.data() + 1, rhs, work.data(), m);

    for (int j = 0; j < m; ++j) T[j + 1] = rhs[j];
    return T;
}

FeaResult solve_fea1d(const FeaParams& p) {
    FeaResult r;
    r.T = solve_fea1d_nodal(p);   // validates p
    const int N = p.N;
    r.h = p.L / N;

    r.x.resize(N + 1);
    r.T_exact.resize(N + 1);
    for (int i = 0; i <= N; ++i) {
        r.x[i] = (i == N) ? p.L : i * r.h;       // avoid round-off at the right end
        r.T_exact[i] = exact_T(p, r.x[i]);
        r.max_nodal_err = std::fmax(r.max_nodal_err, std::fabs(r.T[i] - r.T_exact[i]));
    }

    r.x_mid.resize(N);
    r.T_mid_fe.resize(N);
    r.T_mid_exact.resize(N);
    for (int e = 0; e < N; ++e) {
        r.x_mid[e] = 0.5 * (r.x[e] + r.x[e + 1]);
        r.T_mid_fe[e] = 0.5 * (r.T[e] + r.T[e + 1]);   // linear interpolation at the midpoint
        r.T_mid_exact[e] = exact_T(p, r.x_mid[e]);
        r.max_mid_err = std::fmax(r.max_mid_err, std::fabs(r.T_mid_fe[e] - r.T_mid_exact[e]));
    }
    return r;
}

ConvergenceResult convergence(const FeaParams& base, const std::vector<int>& Ns) {
    ConvergenceResult out;
    for (int N : Ns) {
        FeaParams p = base;
        p.N = N;
        FeaResult r = solve_fea1d(p);   // throws on a bad N
        out.points.push_back({N, r.h, r.max_mid_err, r.max_nodal_err});
    }

    // Least-squares line through (log h, log err): slope = Sxy / Sxx.
    // Errors at round-off level (e.g. q == 0, where FE is exact and the "error"
    // is only floating-point noise) say nothing about the rate, so points below
    // a floor relative to the temperature scale are skipped. With fewer than
    // two usable points the slope stays 0.
    const double scale = std::fmax(1.0, std::fmax(std::fabs(base.T0), std::fmax(std::fabs(base.TL),
                         std::fabs(base.q) * base.L * base.L / (8.0 * base.k))));
    const double floor_err = 1e-12 * scale;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (const auto& pt : out.points) {
        if (!(pt.max_mid_err > floor_err)) continue;
        double lx = std::log(pt.h), ly = std::log(pt.max_mid_err);
        sx += lx; sy += ly; sxx += lx * lx; sxy += lx * ly;
        ++n;
    }
    if (n >= 2) {
        double denom = n * sxx - sx * sx;
        if (denom > 0) out.slope = (n * sxy - sx * sy) / denom;
    }
    return out;
}

}  // namespace wb
