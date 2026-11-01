// gen_samples.cpp - writes the SYNTHETIC sample files used by the demo and the tests.
//   usage: gen_samples [output_dir]      (default: "data")
// Files: cube.stl, heatsink.stl, synthetic_plate.csv and data/bad/* (malformed examples).
// Standard library only. STL units are not stored in the format; we use millimetres.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using Vec = std::array<double, 3>;
struct Tri { Vec a, b, c; };

static Vec sub(const Vec& p, const Vec& q) { return {p[0] - q[0], p[1] - q[1], p[2] - q[2]}; }
static Vec cross(const Vec& p, const Vec& q) {
    return {p[1] * q[2] - p[2] * q[1], p[2] * q[0] - p[0] * q[2], p[0] * q[1] - p[1] * q[0]};
}
static double dot(const Vec& p, const Vec& q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; }

// Signed volume of a closed triangle mesh: positive when normals point outward (CCW winding).
static double signed_volume(const std::vector<Tri>& m) {
    double v = 0;
    for (const Tri& t : m) v += dot(t.a, cross(t.b, t.c)) / 6.0;
    return v;
}

// Every directed edge must appear exactly once and be matched by its reverse -> watertight,
// consistently oriented surface. Vertices are compared after rounding (all ours are exact).
static bool watertight(const std::vector<Tri>& m) {
    auto key = [](const Vec& p) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.4f,%.4f,%.4f", p[0], p[1], p[2]);
        return std::string(buf);
    };
    std::vector<std::pair<std::string, std::string>> edges;
    for (const Tri& t : m) {
        std::string a = key(t.a), b = key(t.b), c = key(t.c);
        edges.push_back({a, b});
        edges.push_back({b, c});
        edges.push_back({c, a});
    }
    std::vector<std::pair<std::string, std::string>> rev;
    for (auto& e : edges) rev.push_back({e.second, e.first});
    std::sort(edges.begin(), edges.end());
    std::sort(rev.begin(), rev.end());
    return edges == rev && std::adjacent_find(edges.begin(), edges.end()) == edges.end();
}

static void write_stl(const fs::path& path, const std::string& name, const std::vector<Tri>& mesh) {
    if (!(signed_volume(mesh) > 0) || !watertight(mesh)) {
        std::cerr << "gen_samples: internal error, mesh '" << name << "' is not a closed outward-oriented surface\n";
        std::exit(2);
    }
    std::ofstream f(path, std::ios::binary);   // binary mode: always LF, also on Windows
    f << "solid " << name << "\n";
    char buf[256];
    for (const Tri& t : mesh) {
        Vec n = cross(sub(t.b, t.a), sub(t.c, t.a));
        double len = std::sqrt(dot(n, n));
        for (double& x : n) x /= len;
        std::snprintf(buf, sizeof buf, "  facet normal %.6g %.6g %.6g\n    outer loop\n", n[0] + 0.0, n[1] + 0.0, n[2] + 0.0);
        f << buf;
        for (const Vec* p : {&t.a, &t.b, &t.c}) {
            std::snprintf(buf, sizeof buf, "      vertex %.6g %.6g %.6g\n", (*p)[0] + 0.0, (*p)[1] + 0.0, (*p)[2] + 0.0);
            f << buf;
        }
        f << "    endloop\n  endfacet\n";
    }
    f << "endsolid " << name << "\n";
    std::cout << "wrote " << path.string() << " (" << mesh.size() << " triangles, volume "
              << signed_volume(mesh) << " mm^3)\n";
}

// ---- cube: 10 mm edge, corner at the origin --------------------------------
static std::vector<Tri> make_cube(double s) {
    Vec p[8] = {{0, 0, 0}, {s, 0, 0}, {s, s, 0}, {0, s, 0}, {0, 0, s}, {s, 0, s}, {s, s, s}, {0, s, s}};
    // Each face as a quad listed counter-clockwise when seen from outside.
    int q[6][4] = {{0, 3, 2, 1},   // z = 0 (bottom)
                   {4, 5, 6, 7},   // z = s (top)
                   {0, 1, 5, 4},   // y = 0
                   {2, 3, 7, 6},   // y = s
                   {1, 2, 6, 5},   // x = s
                   {3, 0, 4, 7}};  // x = 0
    std::vector<Tri> m;
    for (auto& f : q) {
        m.push_back({p[f[0]], p[f[1]], p[f[2]]});
        m.push_back({p[f[0]], p[f[2]], p[f[3]]});
    }
    return m;
}

// ---- finned heat sink: comb profile in the x-z plane, extruded along y -----
struct P2 { double x, z; };

static double area2(P2 a, P2 b, P2 c) { return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x); }

static bool inside_tri(P2 p, P2 a, P2 b, P2 c) {
    return area2(a, b, p) >= 0 && area2(b, c, p) >= 0 && area2(c, a, p) >= 0;
}

// Ear clipping for a simple counter-clockwise polygon. Returns index triples.
static std::vector<std::array<int, 3>> triangulate(const std::vector<P2>& poly) {
    std::vector<int> idx(poly.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);
    std::vector<std::array<int, 3>> out;
    while (idx.size() > 3) {
        bool clipped = false;
        for (std::size_t i = 0; i < idx.size(); ++i) {
            int ia = idx[(i + idx.size() - 1) % idx.size()], ib = idx[i], ic = idx[(i + 1) % idx.size()];
            if (area2(poly[ia], poly[ib], poly[ic]) <= 0) continue;   // reflex corner, not an ear
            bool empty = true;
            for (int j : idx)
                if (j != ia && j != ib && j != ic && inside_tri(poly[j], poly[ia], poly[ib], poly[ic])) { empty = false; break; }
            if (!empty) continue;
            out.push_back({ia, ib, ic});
            idx.erase(idx.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        if (!clipped) { std::cerr << "gen_samples: triangulation failed\n"; std::exit(2); }
    }
    out.push_back({idx[0], idx[1], idx[2]});
    return out;
}

static std::vector<Tri> make_heatsink() {
    const int fins = 12;
    const double base_w = 48.0, base_h = 3.0, fin_t = 1.5, fin_h = 17.0, depth = 30.0;
    const double gap = (base_w - fins * fin_t) / (fins + 1);   // equal gaps incl. both ends
    // Profile, counter-clockwise in (x,z): along the bottom, up the right side, back over the fins.
    std::vector<P2> prof = {{0, 0}, {base_w, 0}, {base_w, base_h}};
    for (int i = fins - 1; i >= 0; --i) {
        double x0 = gap + i * (fin_t + gap), x1 = x0 + fin_t;
        prof.push_back({x1 + 0.0, base_h});
        prof.push_back({x1, base_h + fin_h});
        prof.push_back({x0, base_h + fin_h});
        prof.push_back({x0, base_h});
    }
    prof.push_back({0, base_h});

    auto pt = [&](const P2& p, double y) { return Vec{p.x, y, p.z}; };
    std::vector<Tri> m;
    const std::size_t n = prof.size();
    for (std::size_t i = 0; i < n; ++i) {                     // side walls (outward = right of each CCW edge)
        P2 a = prof[i], b = prof[(i + 1) % n];
        m.push_back({pt(a, 0), pt(b, depth), pt(b, 0)});
        m.push_back({pt(a, 0), pt(a, depth), pt(b, depth)});
    }
    for (auto& t : triangulate(prof)) {                       // end caps
        // A profile triangle that is CCW in (x,z) has its normal along -y, which is outward
        // for the front cap (y = 0); the back cap (y = depth) needs the reverse order.
        m.push_back({pt(prof[t[0]], 0), pt(prof[t[1]], 0), pt(prof[t[2]], 0)});
        m.push_back({pt(prof[t[0]], depth), pt(prof[t[2]], depth), pt(prof[t[1]], depth)});
    }
    return m;
}

// ---- synthetic nodal temperatures on a thin plate ---------------------------
static void write_plate_csv(const fs::path& path) {
    const int nx = 21, ny = 21, nz = 2;
    const double Lx = 0.2, Ly = 0.1, Lz = 0.01;
    std::ofstream f(path, std::ios::binary);
    f << "node_id,x_m,y_m,z_m,temperature_C\n";
    char buf[160];
    int id = 1;
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i, ++id) {
                double x = Lx * i / (nx - 1), y = Ly * j / (ny - 1), z = Lz * k / (nz - 1);
                // 40 degC ambient + Gaussian hot spot at (60 mm, 50 mm) on the bottom face,
                // a gentle drop towards the far end and a small drop through the thickness.
                double r2 = (x - 0.06) * (x - 0.06) + (y - 0.05) * (y - 0.05);
                double T = 40.0 + 140.0 * std::exp(-r2 / (2 * 0.035 * 0.035)) - 30.0 * x / Lx - 4.0 * z / Lz;
                std::snprintf(buf, sizeof buf, "%d,%.6g,%.6g,%.6g,%.6g\n", id, x, y, z, T);
                f << buf;
            }
    std::cout << "wrote " << path.string() << " (" << id - 1 << " nodes)\n";
}

// ---- small malformed files for the error-handling demo ---------------------
static void write_text(const fs::path& path, const std::string& s) {
    std::ofstream f(path, std::ios::binary);
    f << s;
    std::cout << "wrote " << path.string() << " (" << s.size() << " bytes)\n";
}

int main(int argc, char** argv) {
    fs::path out = argc > 1 ? argv[1] : "data";
    fs::create_directories(out / "bad");

    write_stl(out / "cube.stl", "cube_10mm", make_cube(10.0));
    write_stl(out / "heatsink.stl", "heatsink_12_fins", make_heatsink());
    write_plate_csv(out / "synthetic_plate.csv");

    write_text(out / "bad" / "empty.csv", "");
    write_text(out / "bad" / "bad_header.csv",
               "id,x,y,z,T\n1,0.0,0.0,0.0,20.0\n2,0.1,0.0,0.0,25.5\n");
    write_text(out / "bad" / "nan_value.csv",
               "node_id,x_m,y_m,z_m,temperature_C\n1,0.0,0.0,0.0,20.0\n2,0.1,0.0,0.0,25.5\n"
               "3,0.2,0.0,0.0,nan\n4,0.3,0.0,0.0,31.0\n");
    write_text(out / "bad" / "missing_endsolid.stl",
               "solid broken_triangle\n"
               "  facet normal 0 0 1\n    outer loop\n"
               "      vertex 0 0 0\n      vertex 1 0 0\n      vertex 0 1 0\n"
               "    endloop\n  endfacet\n");
    return 0;
}
