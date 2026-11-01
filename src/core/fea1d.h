// fea1d.h - 1D steady heat conduction in a uniform rod, 2-node linear elements.
// Solves -d/dx(k A dT/dx) = q A on [0, L] with T(0)=T0, T(L)=TL (Dirichlet).
// Units: x [m], A [m^2], k [W/(m K)], q [W/m^3], T [degC].
#pragma once
#include <vector>

namespace wb {

struct FeaParams {
    int N = 10;          // number of elements (>= 1)
    double L = 1.0;      // rod length [m]
    double A = 1e-4;     // cross-section [m^2]
    double k = 10.0;     // conductivity [W/(m K)]
    double q = 1.0e4;    // volumetric heat source [W/m^3]
    double T0 = 100.0;   // T(0) [degC]
    double TL = 20.0;    // T(L) [degC]
};

struct FeaResult {
    double h = 0;                    // element length [m]
    std::vector<double> x;           // N+1 node coordinates [m]
    std::vector<double> T;           // N+1 FE nodal temperatures [degC]
    std::vector<double> T_exact;     // exact solution at nodes
    std::vector<double> x_mid;       // N element midpoints [m]
    std::vector<double> T_mid_fe;    // FE interpolation at midpoints (avg of 2 nodes)
    std::vector<double> T_mid_exact; // exact at midpoints
    double max_nodal_err = 0;        // max |T - T_exact| over nodes
    double max_mid_err = 0;          // max |T_mid_fe - T_mid_exact| over midpoints
};

// Throws std::invalid_argument for N<1, N>1e7, non-positive/non-finite L, A, k,
// or non-finite q/T0/TL.
void validate(const FeaParams& p);
// Exact solution T(x) = T0 + (TL-T0) x/L + q/(2k) x (L-x).
double exact_T(const FeaParams& p, double x);
// Assemble global K and F, impose Dirichlet BCs, solve tridiagonal system.
FeaResult solve_fea1d(const FeaParams& p);
// Same solve but returns only nodal T (no exact/midpoint data) - used by the batch.
std::vector<double> solve_fea1d_nodal(const FeaParams& p);

struct ConvergencePoint {
    int N; double h; double max_mid_err; double max_nodal_err;
};
struct ConvergenceResult {
    std::vector<ConvergencePoint> points;
    double slope = 0;   // least-squares slope of log(err_mid) vs log(h)
};
ConvergenceResult convergence(const FeaParams& base, const std::vector<int>& Ns);

}  // namespace wb
