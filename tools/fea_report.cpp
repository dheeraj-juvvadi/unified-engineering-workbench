// fea_report.cpp - regenerate the FEA results: per-mesh CSVs, convergence table,
// temperature profile plot and log-log convergence plot.
// Usage: fea_report [output_dir]      (default: results)
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include "fea1d.h"
#include "report_util.h"
#include "svg_plot.h"

using namespace wb;

int main(int argc, char** argv) {
    const std::string dir = report::output_dir(argc, argv);
    const FeaParams base;                       // L=1 m, A=1e-4 m^2, k=10, q=1e4, T0=100, TL=20
    const std::vector<int> Ns = {10, 20, 40, 80};

    svgplot::Plot temp;
    temp.title = "1D rod: FE temperature vs exact solution";
    temp.xlabel = "Position x [m]";
    temp.ylabel = "Temperature T [degC]";

    // Exact curve, sampled densely.
    svgplot::Series exact;
    exact.label = "Exact";
    exact.color = "#000000";
    exact.width = 3.0;
    for (int i = 0; i <= 200; ++i) {
        double x = base.L * i / 200.0;
        exact.x.push_back(x);
        exact.y.push_back(exact_T(base, x));
    }
    temp.add(exact);

    const svgplot::Marker markers[] = {svgplot::Marker::Circle, svgplot::Marker::Square,
                                       svgplot::Marker::Triangle, svgplot::Marker::None};
    for (size_t s = 0; s < Ns.size(); ++s) {
        FeaParams p = base;
        p.N = Ns[s];
        FeaResult r = solve_fea1d(p);
        const std::string n = std::to_string(p.N);

        // Nodal values.
        {
            auto f = report::open(dir, "fea_N" + n + ".csv");
            f << "x_m,T_fe_C,T_exact_C,abs_err_C\n";
            for (size_t i = 0; i < r.x.size(); ++i)
                f << report::g12(r.x[i]) << "," << report::g12(r.T[i]) << "," << report::g12(r.T_exact[i]) << ","
                  << report::g12(std::fabs(r.T[i] - r.T_exact[i])) << "\n";
        }
        // Element midpoints (FE value = average of the two nodes).
        {
            auto f = report::open(dir, "fea_midpoints_N" + n + ".csv");
            f << "x_m,T_fe_C,T_exact_C,abs_err_C\n";
            for (size_t i = 0; i < r.x_mid.size(); ++i)
                f << report::g12(r.x_mid[i]) << "," << report::g12(r.T_mid_fe[i]) << "," << report::g12(r.T_mid_exact[i])
                  << "," << report::g12(std::fabs(r.T_mid_fe[i] - r.T_mid_exact[i])) << "\n";
        }

        svgplot::Series fe;
        fe.label = "FE N=" + n + " (h=" + report::fmt("%.4g", r.h) + " m)";
        fe.color = svgplot::palette(s);
        fe.width = 1.6;
        fe.marker = markers[s];
        fe.x = r.x;
        fe.y = r.T;
        temp.add(fe);
        std::printf("N=%-3d  h=%.5f m  max nodal err=%.3e degC  max midpoint err=%.6f degC\n", p.N, r.h,
                    r.max_nodal_err, r.max_mid_err);
    }
    temp.save(report::path(dir, "fea_temperature.svg"));

    // ---- convergence ----
    ConvergenceResult c = convergence(base, Ns);
    {
        auto f = report::open(dir, "convergence.csv");
        f << "N,h_m,max_mid_err_C,max_nodal_err_C\n";
        for (const auto& pt : c.points)
            f << pt.N << "," << report::g12(pt.h) << "," << report::g12(pt.max_mid_err) << ","
              << report::g12(pt.max_nodal_err) << "\n";
    }
    svgplot::Plot conv;
    conv.title = "Convergence of the midpoint error";
    conv.xlabel = "Element length h [m]";
    conv.ylabel = "Max midpoint error [degC]";
    conv.logx = conv.logy = true;
    svgplot::Series meas, ref;
    meas.label = "FE max midpoint error (slope = " + report::fmt("%.3f", c.slope) + ")";
    meas.color = svgplot::palette(0);
    meas.marker = svgplot::Marker::Circle;
    ref.label = "reference slope 2";
    ref.color = svgplot::palette(1);
    ref.dashed = true;
    for (const auto& pt : c.points) {
        meas.x.push_back(pt.h);
        meas.y.push_back(pt.max_mid_err);
        // Reference line anchored at the coarsest mesh: err0 * (h/h0)^2.
        ref.x.push_back(pt.h);
        ref.y.push_back(c.points[0].max_mid_err * std::pow(pt.h / c.points[0].h, 2.0));
    }
    conv.add(meas);
    conv.add(ref);
    conv.save(report::path(dir, "fea_convergence.svg"));

    std::printf("convergence slope = %.4f (expected 2)\nwrote results to %s\n", c.slope, dir.c_str());
    return 0;
}
