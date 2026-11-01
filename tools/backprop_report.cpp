// backprop_report.cpp - one-neuron backprop worked example: CSV of the loss
// curve, an SVG plot and a text summary of every intermediate number.
// Usage: backprop_report [output_dir]      (default: results)
#include <cstdio>
#include <string>
#include "backprop.h"
#include "report_util.h"
#include "svg_plot.h"

using namespace wb;

int main(int argc, char** argv) {
    const std::string dir = report::output_dir(argc, argv);
    const BpReport r = run_backprop(100);

    // step 0 = loss at the initial parameters, step 1 = after the first update, ...
    std::vector<double> steps, J;
    steps.push_back(0); J.push_back(r.f0.J);
    for (size_t i = 0; i < r.loss.size(); ++i) { steps.push_back(static_cast<double>(i + 1)); J.push_back(r.loss[i]); }
    {
        auto f = report::open(dir, "backprop.csv");
        f << "step,J\n";
        for (size_t i = 0; i < J.size(); ++i) f << static_cast<int>(steps[i]) << "," << report::g12(J[i]) << "\n";
    }

    svgplot::Plot plot;
    plot.title = "One-neuron network: training loss";
    plot.xlabel = "Gradient-descent step [-]";
    plot.ylabel = "Loss J = 0.5 (yhat - y)^2 [-]";
    svgplot::Series s;
    s.label = "J (lr = " + report::fmt("%.2g", r.lr) + ")";
    s.color = svgplot::palette(0);
    s.marker = svgplot::Marker::None;
    s.x = steps;
    s.y = J;
    plot.add(s);
    plot.save(report::path(dir, "backprop_loss.svg"));

    auto f = report::open(dir, "backprop_summary.txt");
    auto line = [&](const char* name, double v) { f << "  " << name << " = " << report::fmt("%.8f", v) << "\n"; };
    f << "One-neuron backprop: h = tanh(w x + b), yhat = v h + c, J = 0.5 (yhat - y)^2\n";
    f << "Input x = " << r.x << ", target y = " << r.y << ", learning rate = " << r.lr << ", FD eps = " << r.eps << "\n\n";
    f << "Initial parameters:\n";
    line("w", r.p0.w); line("b", r.p0.b); line("v", r.p0.v); line("c", r.p0.c);
    f << "\nForward pass:\n";
    line("z", r.f0.z); line("h", r.f0.h); line("yhat", r.f0.yhat); line("e = yhat - y", r.f0.e); line("J", r.f0.J);
    f << "\nAnalytic gradients (chain rule):\n";
    line("dJ/dw", r.g.dw); line("dJ/db", r.g.db); line("dJ/dv", r.g.dv); line("dJ/dc", r.g.dc);
    f << "\nFinite-difference gradients (central, eps = 1e-6):\n";
    line("dJ/dw", r.g_fd.dw); line("dJ/db", r.g_fd.db); line("dJ/dv", r.g_fd.dv); line("dJ/dc", r.g_fd.dc);
    f << "\nAbsolute error |analytic - FD|:\n";
    f << "  dw = " << report::fmt("%.3e", std::fabs(r.g.dw - r.g_fd.dw)) << "\n";
    f << "  db = " << report::fmt("%.3e", std::fabs(r.g.db - r.g_fd.db)) << "\n";
    f << "  dv = " << report::fmt("%.3e", std::fabs(r.g.dv - r.g_fd.dv)) << "\n";
    f << "  dc = " << report::fmt("%.3e", std::fabs(r.g.dc - r.g_fd.dc)) << "\n";
    f << "  max = " << report::fmt("%.3e", r.max_fd_err) << "\n";
    f << "\nAfter one update (p <- p - lr * grad):\n";
    line("w", r.p1.w); line("b", r.p1.b); line("v", r.p1.v); line("c", r.p1.c); line("J1", r.J1);
    f << "\nAfter " << r.loss.size() - 1 << " further updates: J = " << report::fmt("%.8f", r.loss.back()) << "\n";

    std::printf("J0 = %.8f, J1 = %.8f, J(final) = %.8f, max FD error = %.3e\nwrote results to %s\n", r.f0.J, r.J1,
                r.loss.back(), r.max_fd_err, dir.c_str());
    return 0;
}
