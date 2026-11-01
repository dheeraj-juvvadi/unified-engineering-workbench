// svg_plot.h - tiny header-only SVG plot writer for the report tools.
// Line / scatter series, titled, with axis labels (give the units in the label,
// e.g. "Position x [m]"), tick marks and grid, a legend, markers, and optional
// log-log axes. Output is a self-contained SVG with a white background so it
// can be dropped straight into a report.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace svgplot {

enum class Marker { None, Circle, Square, Triangle, Diamond };

struct Series {
    std::string label;
    std::vector<double> x, y;
    std::string color = "#0072B2";
    bool line = true;            // connect points with a polyline
    bool dashed = false;         // dashed line (e.g. a reference slope)
    Marker marker = Marker::None;
    double width = 2.0;          // line width in px
};

// Colour-blind-safe palette (Okabe-Ito) for cycling through series.
inline const char* palette(size_t i) {
    static const char* c[] = {"#0072B2", "#D55E00", "#009E73", "#CC79A7",
                              "#E69F00", "#56B4E9", "#000000"};
    return c[i % 7];
}

struct Plot {
    std::string title, xlabel, ylabel;
    bool logx = false, logy = false;
    int width = 820, height = 520;
    std::vector<Series> series;

    void add(Series s) { series.push_back(std::move(s)); }

    // Render to an SVG string.
    std::string render() const;
    // Write to a file; returns false if the file could not be opened.
    bool save(const std::string& path) const {
        std::ofstream f(path);
        if (!f) return false;
        f << render();
        return static_cast<bool>(f);
    }
};

// ---------------------------------------------------------------- internals
namespace detail {

inline std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

inline std::string num(double v) {   // coordinates / sizes in the SVG
    char b[40];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

inline std::string tick_label(double v) {
    if (std::fabs(v) < 1e-12) v = 0;   // avoid "-0" and 1e-17 noise
    char b[40];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

struct Axis {
    bool log = false;
    double lo = 0, hi = 1;           // plotted range (ticks lie on or inside it)
    std::vector<double> ticks;       // positions with labels

    // Map a data value to 0..1 along the axis.
    double frac(double v) const {
        if (log) return (std::log10(v) - std::log10(lo)) / (std::log10(hi) - std::log10(lo));
        return (v - lo) / (hi - lo);
    }
};

// Linear axis with "nice" steps (1, 2, 5 x 10^n) covering [mn, mx].
inline Axis make_linear(double mn, double mx) {
    Axis a;
    if (!(mx > mn)) { double pad = std::fabs(mn) > 0 ? std::fabs(mn) * 0.1 : 1.0; mn -= pad; mx += pad; }
    double raw = (mx - mn) / 6.0;
    double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double step = mag;
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        step = m * mag;
        if (step >= raw) break;
    }
    a.lo = std::floor(mn / step + 1e-9) * step;
    a.hi = std::ceil(mx / step - 1e-9) * step;
    for (double t = a.lo; t <= a.hi + step * 1e-6; t += step) a.ticks.push_back(t);
    return a;
}

// Log axis: ticks at 1, 2, 5 x 10^n (just powers of ten when the range is wide).
inline Axis make_log(double mn, double mx) {
    Axis a;
    a.log = true;
    if (!(mx > mn)) { mn /= 2; mx *= 2; }
    int e0 = static_cast<int>(std::floor(std::log10(mn)));
    int e1 = static_cast<int>(std::ceil(std::log10(mx)));
    bool wide = (e1 - e0) >= 4;
    std::vector<double> all;
    for (int e = e0 - 1; e <= e1 + 1; ++e)
        for (double m : {1.0, 2.0, 5.0}) {
            if (wide && m != 1.0) continue;
            all.push_back(m * std::pow(10.0, e));
        }
    // lo = largest tick <= mn, hi = smallest tick >= mx.
    a.lo = all.front();
    for (double t : all) if (t <= mn * (1 + 1e-9)) a.lo = t;
    a.hi = all.back();
    for (auto it = all.rbegin(); it != all.rend(); ++it) if (*it >= mx * (1 - 1e-9)) a.hi = *it;
    for (double t : all) if (t >= a.lo * (1 - 1e-9) && t <= a.hi * (1 + 1e-9)) a.ticks.push_back(t);
    return a;
}

inline void marker_svg(std::ostringstream& o, Marker m, double cx, double cy, const std::string& col) {
    const double r = 4.0;
    switch (m) {
        case Marker::Circle:
            o << "<circle cx='" << num(cx) << "' cy='" << num(cy) << "' r='" << r << "' fill='" << col << "'/>";
            break;
        case Marker::Square:
            o << "<rect x='" << num(cx - r) << "' y='" << num(cy - r) << "' width='" << 2 * r
              << "' height='" << 2 * r << "' fill='" << col << "'/>";
            break;
        case Marker::Triangle:
            o << "<polygon points='" << num(cx) << "," << num(cy - r - 1) << " " << num(cx - r - 1) << ","
              << num(cy + r) << " " << num(cx + r + 1) << "," << num(cy + r) << "' fill='" << col << "'/>";
            break;
        case Marker::Diamond:
            o << "<polygon points='" << num(cx) << "," << num(cy - r - 1) << " " << num(cx + r + 1) << ","
              << num(cy) << " " << num(cx) << "," << num(cy + r + 1) << " " << num(cx - r - 1) << ","
              << num(cy) << "' fill='" << col << "'/>";
            break;
        case Marker::None: break;
    }
}

}  // namespace detail

inline std::string Plot::render() const {
    using namespace detail;
    // ---- data range (log axes ignore non-positive values) ----
    double xmin = std::numeric_limits<double>::infinity(), xmax = -xmin, ymin = xmin, ymax = -xmin;
    for (const auto& s : series) {
        size_t n = std::min(s.x.size(), s.y.size());
        for (size_t i = 0; i < n; ++i) {
            if (!std::isfinite(s.x[i]) || !std::isfinite(s.y[i])) continue;
            if ((logx && s.x[i] <= 0) || (logy && s.y[i] <= 0)) continue;
            xmin = std::min(xmin, s.x[i]); xmax = std::max(xmax, s.x[i]);
            ymin = std::min(ymin, s.y[i]); ymax = std::max(ymax, s.y[i]);
        }
    }
    if (xmin > xmax) { xmin = ymin = logx || logy ? 1 : 0; xmax = ymax = 2; }   // no data at all
    Axis ax = logx ? make_log(xmin, xmax) : make_linear(xmin, xmax);
    Axis ay = logy ? make_log(ymin, ymax) : make_linear(ymin, ymax);

    // ---- layout: room on the right for the legend ----
    size_t longest = 0;
    for (const auto& s : series) longest = std::max(longest, s.label.size());
    const int legend_w = series.empty() ? 20 : std::min(300, 52 + static_cast<int>(longest) * 7);
    const double ml = 78, mr = legend_w + 16, mt = 46, mb = 62;
    const double pw = width - ml - mr, ph = height - mt - mb;
    auto X = [&](double v) { return ml + ax.frac(v) * pw; };
    auto Y = [&](double v) { return mt + (1.0 - ay.frac(v)) * ph; };

    std::ostringstream o;
    o << "<svg xmlns='http://www.w3.org/2000/svg' width='" << width << "' height='" << height
      << "' viewBox='0 0 " << width << " " << height << "' font-family='Helvetica, Arial, sans-serif'>\n";
    o << "<rect width='100%' height='100%' fill='#ffffff'/>\n";
    o << "<text x='" << num(ml + pw / 2) << "' y='26' text-anchor='middle' font-size='17' font-weight='bold' fill='#111'>"
      << esc(title) << "</text>\n";

    // ---- grid, ticks, tick labels ----
    for (double t : ax.ticks) {
        double px = X(t);
        o << "<line x1='" << num(px) << "' y1='" << num(mt) << "' x2='" << num(px) << "' y2='" << num(mt + ph)
          << "' stroke='#e3e3e3'/>\n";
        o << "<line x1='" << num(px) << "' y1='" << num(mt + ph) << "' x2='" << num(px) << "' y2='" << num(mt + ph + 5)
          << "' stroke='#333'/>\n";
        o << "<text x='" << num(px) << "' y='" << num(mt + ph + 20) << "' text-anchor='middle' font-size='12' fill='#333'>"
          << tick_label(t) << "</text>\n";
    }
    for (double t : ay.ticks) {
        double py = Y(t);
        o << "<line x1='" << num(ml) << "' y1='" << num(py) << "' x2='" << num(ml + pw) << "' y2='" << num(py)
          << "' stroke='#e3e3e3'/>\n";
        o << "<line x1='" << num(ml - 5) << "' y1='" << num(py) << "' x2='" << num(ml) << "' y2='" << num(py)
          << "' stroke='#333'/>\n";
        o << "<text x='" << num(ml - 9) << "' y='" << num(py + 4) << "' text-anchor='end' font-size='12' fill='#333'>"
          << tick_label(t) << "</text>\n";
    }
    o << "<rect x='" << num(ml) << "' y='" << num(mt) << "' width='" << num(pw) << "' height='" << num(ph)
      << "' fill='none' stroke='#333'/>\n";

    // ---- axis labels ----
    o << "<text x='" << num(ml + pw / 2) << "' y='" << num(height - 16) << "' text-anchor='middle' font-size='14' fill='#111'>"
      << esc(xlabel) << "</text>\n";
    o << "<text transform='translate(18," << num(mt + ph / 2) << ") rotate(-90)' text-anchor='middle' font-size='14' fill='#111'>"
      << esc(ylabel) << "</text>\n";

    // ---- series (clipped to the plot area) ----
    o << "<clipPath id='clip'><rect x='" << num(ml) << "' y='" << num(mt) << "' width='" << num(pw)
      << "' height='" << num(ph) << "'/></clipPath>\n<g clip-path='url(#clip)'>\n";
    for (const auto& s : series) {
        std::vector<std::pair<double, double>> pts;
        size_t n = std::min(s.x.size(), s.y.size());
        for (size_t i = 0; i < n; ++i) {
            if (!std::isfinite(s.x[i]) || !std::isfinite(s.y[i])) continue;
            if ((logx && s.x[i] <= 0) || (logy && s.y[i] <= 0)) continue;
            pts.push_back({X(s.x[i]), Y(s.y[i])});
        }
        if (s.line && pts.size() > 1) {
            o << "<polyline fill='none' stroke='" << s.color << "' stroke-width='" << s.width << "'"
              << (s.dashed ? " stroke-dasharray='7 5'" : "") << " points='";
            for (auto& p : pts) o << num(p.first) << "," << num(p.second) << " ";
            o << "'/>\n";
        }
        for (auto& p : pts) marker_svg(o, s.marker, p.first, p.second, s.color);
        o << "\n";
    }
    o << "</g>\n";

    // ---- legend (right of the plot) ----
    double ly = mt + 6;
    for (const auto& s : series) {
        double lx = ml + pw + 14;
        if (s.line)
            o << "<line x1='" << num(lx) << "' y1='" << num(ly + 6) << "' x2='" << num(lx + 26) << "' y2='" << num(ly + 6)
              << "' stroke='" << s.color << "' stroke-width='" << s.width << "'"
              << (s.dashed ? " stroke-dasharray='7 5'" : "") << "/>";
        marker_svg(o, s.marker, lx + 13, ly + 6, s.color);
        o << "<text x='" << num(lx + 33) << "' y='" << num(ly + 10) << "' font-size='12' fill='#111'>" << esc(s.label)
          << "</text>\n";
        ly += 20;
    }
    o << "</svg>\n";
    return o.str();
}

}  // namespace svgplot
