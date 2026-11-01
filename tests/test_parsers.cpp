// test_parsers.cpp - STL and CSV parser tests: good files, every documented rejection, big inputs.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>

#include "ansys_csv.h"
#include "stl_ascii.h"
#include "wb_test.h"

#ifndef WB_DATA_DIR
#define WB_DATA_DIR "data"
#endif

namespace {

std::string read_file(const std::string& name) {
    std::ifstream f(std::string(WB_DATA_DIR) + "/" + name, std::ios::binary);
    if (!f) throw wbt::Failure{"cannot open sample file " + name + " (looked in " WB_DATA_DIR ")"};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string to_crlf(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// Runs fn, expects a ParseError on `line` whose message contains `msg_part`.
template <class F>
wb::ParseError expect_error(F fn, std::size_t line, const std::string& msg_part) {
    try {
        fn();
    } catch (const wb::ParseError& e) {
        if (e.line != line) {
            std::ostringstream os;
            os << "wrong line: got " << e.line << ", expected " << line << " (message: " << e.what() << ")";
            throw wbt::Failure{os.str()};
        }
        if (!contains(e.what(), msg_part))
            throw wbt::Failure{std::string("message '") + e.what() + "' does not contain '" + msg_part + "'"};
        if (e.hint.empty()) throw wbt::Failure{std::string("no hint for: ") + e.what()};
        return e;
    }
    throw wbt::Failure{"expected a ParseError (" + msg_part + ") but nothing was thrown"};
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------- STL helpers
// A minimal valid file; line numbers: solid=1 facet=2 outer=3 vertex=4,5,6 endloop=7 endfacet=8 endsolid=9
const char* kTri =
    "solid t\n"
    "facet normal 0 0 1\n"
    "outer loop\n"
    "vertex 0 0 0\n"
    "vertex 1 0 0\n"
    "vertex 0 1 0\n"
    "endloop\n"
    "endfacet\n"
    "endsolid t\n";

std::string replace_line(const std::string& text, int line_no, const std::string& with) {
    std::istringstream in(text);
    std::string out, l;
    for (int n = 1; std::getline(in, l); ++n) out += (n == line_no ? with : l) + "\n";
    return out;
}

std::string drop_line(const std::string& text, int line_no) {
    std::istringstream in(text);
    std::string out, l;
    for (int n = 1; std::getline(in, l); ++n)
        if (n != line_no) out += l + "\n";
    return out;
}

// Signed volume of the mesh: positive means the stored winding gives outward normals.
double volume(const wb::StlMesh& m) {
    double v = 0;
    for (std::size_t t = 0; t < m.triangle_count; ++t) {
        const float* p = &m.positions[9 * t];
        v += (double(p[0]) * (double(p[4]) * p[8] - double(p[5]) * p[7]) -
              double(p[1]) * (double(p[3]) * p[8] - double(p[5]) * p[6]) +
              double(p[2]) * (double(p[3]) * p[7] - double(p[4]) * p[6])) / 6.0;
    }
    return v;
}

// Does the stored normal agree with the winding of each triangle?
bool normals_match_winding(const wb::StlMesh& m) {
    for (std::size_t t = 0; t < m.triangle_count; ++t) {
        const float* p = &m.positions[9 * t];
        double ux = p[3] - p[0], uy = p[4] - p[1], uz = p[5] - p[2];
        double vx = p[6] - p[0], vy = p[7] - p[1], vz = p[8] - p[2];
        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float* n = &m.normals[3 * t];
        if (nx * n[0] + ny * n[1] + nz * n[2] <= 0) return false;
    }
    return true;
}

// Synthetic STL with `n` triangles laid out on a grid (not a closed solid, just valid syntax).
std::string make_big_stl(std::size_t n) {
    std::string s;
    s.reserve(n * 200 + 64);
    s += "solid big\n";
    char buf[512];
    for (std::size_t i = 0; i < n; ++i) {
        double x = double(i % 500) * 0.37, y = double((i / 500) % 500) * 0.21, z = double(i / 250000) * 1.3;
        int len = std::snprintf(buf, sizeof buf,
                                "  facet normal 0.000000e+00 0.000000e+00 1.000000e+00\n"
                                "    outer loop\n"
                                "      vertex %.6e %.6e %.6e\n"
                                "      vertex %.6e %.6e %.6e\n"
                                "      vertex %.6e %.6e %.6e\n"
                                "    endloop\n"
                                "  endfacet\n",
                                x, y, z, x + 0.3, y, z, x, y + 0.2, z);
        s.append(buf, static_cast<std::size_t>(len));
    }
    s += "endsolid big\n";
    return s;
}

// ---------------------------------------------------------------- CSV helpers
const char* kHdr = "node_id,x_m,y_m,z_m,temperature_C\n";

std::string make_big_csv(std::size_t rows) {
    std::string s = kHdr;
    s.reserve(rows * 48 + 64);
    char buf[160];
    for (std::size_t i = 0; i < rows; ++i) {
        double x = double(i % 1000) * 1e-3, y = double((i / 1000) % 500) * 2e-3, z = double(i / 500000) * 1e-2;
        double t = 20.0 + 80.0 * std::sin(double(i) * 1e-3);
        int len = std::snprintf(buf, sizeof buf, "%zu,%.6g,%.6g,%.6g,%.6g\n", i + 1, x, y, z, t);
        s.append(buf, static_cast<std::size_t>(len));
    }
    return s;
}

}  // namespace

// ============================================================ STL: accepted input

WB_TEST(stl_cube_sample) {
    wb::StlMesh m = wb::parse_stl_ascii(read_file("cube.stl"));
    CHECK(m.triangle_count == 12);
    CHECK(m.positions.size() == 12 * 9);
    CHECK(m.normals.size() == 12 * 3);
    CHECK(m.name == "cube_10mm");
    for (int i = 0; i < 3; ++i) {
        CHECK_NEAR(m.bbox_min[i], 0.0, 1e-12);
        CHECK_NEAR(m.bbox_max[i], 10.0, 1e-12);
    }
    CHECK_NEAR(volume(m), 1000.0, 1e-3);
    CHECK(normals_match_winding(m));
}

WB_TEST(stl_heatsink_sample) {
    wb::StlMesh m = wb::parse_stl_ascii(read_file("heatsink.stl"));
    CHECK(m.triangle_count == 204);
    CHECK_NEAR(m.bbox_min[0], 0, 1e-9);  CHECK_NEAR(m.bbox_max[0], 48, 1e-9);
    CHECK_NEAR(m.bbox_min[1], 0, 1e-9);  CHECK_NEAR(m.bbox_max[1], 30, 1e-9);
    CHECK_NEAR(m.bbox_min[2], 0, 1e-9);  CHECK_NEAR(m.bbox_max[2], 20, 1e-9);
    CHECK_NEAR(volume(m), 13500.0, 0.5);        // 48x3x30 base + 12 fins of 1.5x17x30 (float coordinates)
    CHECK(normals_match_winding(m));

    // Closed surface: every directed edge must be matched by exactly one reverse edge.
    std::map<std::array<float, 6>, int> edges;
    for (std::size_t t = 0; t < m.triangle_count; ++t) {
        const float* p = &m.positions[9 * t];
        for (int k = 0; k < 3; ++k) {
            const float* a = p + 3 * k;
            const float* b = p + 3 * ((k + 1) % 3);
            edges[{a[0], a[1], a[2], b[0], b[1], b[2]}] += 1;
            edges[{b[0], b[1], b[2], a[0], a[1], a[2]}] -= 1;
        }
    }
    for (auto& kv : edges) CHECK(kv.second == 0);
}

WB_TEST(stl_crlf_bom_and_odd_formatting) {
    std::string cube = read_file("cube.stl");
    CHECK(wb::parse_stl_ascii(to_crlf(cube)).triangle_count == 12);
    CHECK(wb::parse_stl_ascii("\xEF\xBB\xBF" + cube).triangle_count == 12);
    CHECK(wb::parse_stl_ascii("\xEF\xBB\xBF" + to_crlf(cube)).triangle_count == 12);

    // upper-case keywords, tabs, '+' signs, exponents, blank lines, no final newline
    std::string odd =
        "\n  SOLID  odd name \n\n"
        "\tFACET NORMAL +0 0 1e0\n  OUTER LOOP\n"
        "VERTEX\t-1.5E+1  2  3\nvertex 4 5.5e-1 6\nvertex 7 8 9\n"
        "ENDLOOP\nENDFACET\nENDSOLID odd name";
    wb::StlMesh m = wb::parse_stl_ascii(odd);
    CHECK(m.triangle_count == 1);
    CHECK(m.name == "odd name");
    CHECK_NEAR(m.bbox_min[0], -15.0, 1e-6);
    CHECK_NEAR(m.positions[4], 0.55, 1e-6);
}

// ============================================================ STL: rejected input

WB_TEST(stl_empty_and_whitespace) {
    auto e = expect_error([] { wb::parse_stl_ascii(""); }, 0, "File is empty (0 bytes)");
    CHECK(e.hint.find("ASCII") != std::string::npos);
    expect_error([] { wb::parse_stl_ascii("  \n\t\r\n   \n"); }, 0, "only whitespace");
    expect_error([] { wb::parse_stl_ascii("\xEF\xBB\xBF\n"); }, 0, "only whitespace");
}

WB_TEST(stl_binary_rejected) {
    // 80 byte header that even starts with "solid", then a count and one 50-byte facet.
    std::string bin = "solid exported by some CAD tool";
    bin.resize(80, ' ');
    bin += std::string("\x01\x00\x00\x00", 4);
    for (int i = 0; i < 12; ++i) bin += std::string("\x00\x00\x80\x3f", 4);
    bin += std::string("\x00\x00", 2);
    auto e = expect_error([&] { wb::parse_stl_ascii(bin); }, 0, "binary STL");
    CHECK(e.hint.find("ASCII STL") != std::string::npos);

    // NUL byte somewhere in the first 512 bytes of otherwise ASCII-looking text
    std::string nul = kTri;
    nul[20] = '\0';
    expect_error([&] { wb::parse_stl_ascii(nul); }, 0, "binary STL");
}

WB_TEST(stl_structure_errors) {
    std::string tri = kTri;
    CHECK(wb::parse_stl_ascii(tri).triangle_count == 1);   // the baseline really is valid

    expect_error([] { wb::parse_stl_ascii("hello world\n"); }, 1, "expected 'solid");
    expect_error([] { wb::parse_stl_ascii("\n\nfacet normal 0 0 1\n"); }, 3, "expected 'solid");

    // missing endsolid: reported on the last non-blank line
    expect_error([&] { wb::parse_stl_ascii(drop_line(tri, 9)); }, 8, "endsolid");
    expect_error([&] { wb::parse_stl_ascii(drop_line(drop_line(tri, 9), 8) + "\n\n"); }, 7, "middle of a facet");

    // facet with only 2 vertices
    expect_error([&] { wb::parse_stl_ascii(drop_line(tri, 6)); }, 6, "only 2 vertices");
    // facet with 4 vertices
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 7, "vertex 1 1 0\nendloop")); }, 7, "more than 3 vertices");
    // keywords out of order / missing
    expect_error([&] { wb::parse_stl_ascii(drop_line(tri, 3)); }, 3, "outer loop");
    expect_error([&] { wb::parse_stl_ascii(drop_line(tri, 8)); }, 8, "endfacet");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 2, "facet 0 0 1")); }, 2, "facet normal");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 2, "triangle 0 0 1")); }, 2, "found 'triangle'");

    // no facets at all / junk after the end
    expect_error([] { wb::parse_stl_ascii("solid x\nendsolid x\n"); }, 2, "no facets");
    expect_error([&] { wb::parse_stl_ascii(tri + "\n  \nsolid second\n"); }, 12, "after 'endsolid'");
}

WB_TEST(stl_number_errors) {
    std::string tri = kTri;
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 5, "vertex nan 0 0")); }, 5, "not finite");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 5, "vertex 1 -inf 0")); }, 5, "not finite");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 6, "vertex 1 0 1e999")); }, 6, "out of range");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 4, "vertex 1e300 0 0")); }, 4, "32-bit float");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 4, "vertex 1 abc 0")); }, 4, "not a number");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 4, "vertex 1 2")); }, 4, "needs 3 numbers");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 4, "vertex 1 2 3 4")); }, 4, "unexpected text");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 2, "facet normal 0 NaN 1")); }, 2, "not finite");
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 4, "vertex 1,5 2 3")); }, 4, "not a number");
    // a bad value that is not "nan" but looks similar must not be accepted by a prefix match
    expect_error([&] { wb::parse_stl_ascii(replace_line(tri, 4, "vertex 1.0x 2 3")); }, 4, "not a number");
}

WB_TEST(stl_line_numbers_with_crlf_and_blank_lines) {
    // Same defect, three layouts: the reported line must follow the physical line.
    std::string tri = kTri;
    std::string bad = replace_line(tri, 5, "vertex nan 0 0");
    expect_error([&] { wb::parse_stl_ascii(to_crlf(bad)); }, 5, "not finite");
    expect_error([&] { wb::parse_stl_ascii("\n\n" + bad); }, 7, "not finite");
    expect_error([&] { wb::parse_stl_ascii("\xEF\xBB\xBF" + to_crlf(bad)); }, 5, "not finite");
}

// ============================================================ CSV: accepted input

WB_TEST(csv_plate_sample) {
    wb::ThermalField f = wb::parse_ansys_csv(read_file("synthetic_plate.csv"));
    CHECK(f.nodes.size() == 21 * 21 * 2);
    CHECK_NEAR(f.bbox_min[0], 0.0, 1e-12);  CHECK_NEAR(f.bbox_max[0], 0.2, 1e-9);
    CHECK_NEAR(f.bbox_min[1], 0.0, 1e-12);  CHECK_NEAR(f.bbox_max[1], 0.1, 1e-9);
    CHECK_NEAR(f.bbox_min[2], 0.0, 1e-12);  CHECK_NEAR(f.bbox_max[2], 0.01, 1e-9);
    CHECK(f.t_min > -273.15 && f.t_min < f.t_max);
    CHECK(f.t_max > 150.0 && f.t_max < 190.0);   // hot spot
    CHECK(f.nodes.front().id == 1 && f.nodes.back().id == 882);
    double lo = 1e9, hi = -1e9;
    for (auto& n : f.nodes) { lo = std::min(lo, n.t); hi = std::max(hi, n.t); }
    CHECK(lo == f.t_min && hi == f.t_max);
}

WB_TEST(csv_formatting_variants) {
    std::string body = "1,0,0,0,20\n2,0.5,0,0,-273.15\n3,1e-1,2.5E+0,-1,300\n";
    std::string plain = kHdr + body;
    wb::ThermalField f = wb::parse_ansys_csv(plain);
    CHECK(f.nodes.size() == 3);
    CHECK_NEAR(f.t_min, -273.15, 1e-12);                    // exactly absolute zero is allowed
    CHECK_NEAR(f.nodes[2].y, 2.5, 1e-12);
    CHECK_NEAR(f.bbox_min[2], -1.0, 1e-12);

    CHECK(wb::parse_ansys_csv(to_crlf(plain)).nodes.size() == 3);
    CHECK(wb::parse_ansys_csv("\xEF\xBB\xBF" + plain).nodes.size() == 3);
    CHECK(wb::parse_ansys_csv(plain + "\n\n  \n").nodes.size() == 3);                 // trailing blank lines
    CHECK(wb::parse_ansys_csv(kHdr + body.substr(0, body.size() - 1)).nodes.size() == 3);   // no final newline
    CHECK(wb::parse_ansys_csv(" node_id , x_m,y_m ,z_m,temperature_C \r\n 7 , 1 , 2 , 3 , 4 \r\n").nodes[0].id == 7);
    // ids need not be sorted, only unique
    CHECK(wb::parse_ansys_csv(std::string(kHdr) + "5,0,0,0,1\n2,0,0,0,1\n9,0,0,0,1\n").nodes.size() == 3);
}

// ============================================================ CSV: rejected input

WB_TEST(csv_empty_header_only) {
    auto e = expect_error([] { wb::parse_ansys_csv(""); }, 0, "File is empty (0 bytes)");
    CHECK(e.hint.find("node_id,x_m,y_m,z_m,temperature_C") != std::string::npos);
    expect_error([] { wb::parse_ansys_csv(" \n\r\n\t\n"); }, 0, "only whitespace");
    expect_error([] { wb::parse_ansys_csv(kHdr); }, 1, "contains a header but no data rows");
    expect_error([] { wb::parse_ansys_csv(std::string(kHdr) + "\n\n  \n"); }, 1, "contains a header but no data rows");
    expect_error([] { wb::parse_ansys_csv(std::string("\xEF\xBB\xBF") + kHdr + "\r\n"); }, 1, "no data rows");
}

WB_TEST(csv_header_errors) {
    expect_error([] { wb::parse_ansys_csv("id,x,y,z,T\n1,0,0,0,20\n"); }, 1, "header column 1 is 'id'");
    expect_error([] { wb::parse_ansys_csv("node_id,x_m,y_m,z_m,Temperature_C\n1,0,0,0,20\n"); }, 1, "expected 'temperature_C'");
    expect_error([] { wb::parse_ansys_csv("node_id,x_m,y_m,z_m\n1,0,0,0\n"); }, 1, "header has 4 columns");
    expect_error([] { wb::parse_ansys_csv("node_id,x_m,y_m,z_m,temperature_C,extra\n1,0,0,0,20,1\n"); }, 1, "header has 6 columns");
    expect_error([] { wb::parse_ansys_csv("1,0,0,0,20\n2,0,0,0,21\n"); }, 1, "header");   // data without a header
    expect_error([] { wb::parse_ansys_csv("\n\nid,x,y,z,T\n"); }, 3, "header column 1");    // header on line 3
}

WB_TEST(csv_row_errors) {
    std::string h = kHdr;
    auto e = expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,20\n2,0,0,0,21\n3,0,0,21\n"); }, 4,
                          "Line 4: expected 5 columns (node_id,x_m,y_m,z_m,temperature_C), found 4");
    CHECK(!e.hint.empty());
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,20,99\n"); }, 2, "found 6");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,20,\n"); }, 2, "found 6");           // trailing comma
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0\n"); }, 2, "found 4");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,20\n\n2,0,0,0,21\n3,0\n"); }, 5, "found 2");   // blank line counted
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,abc,0,20\n"); }, 2, "column 3 (y_m) value 'abc' is not a number");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,20 degC\n"); }, 2, "column 5 (temperature_C)");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,,0,20\n"); }, 2, "column 3 (y_m) is empty");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,20\n2,nan,0,0,20\n"); }, 3, "not finite");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,NaN\n"); }, 2, "not finite");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,Inf,20\n"); }, 2, "not finite");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,1e999,20\n"); }, 2, "out of range");
    expect_error([&] { wb::parse_ansys_csv(h + "1.5,0,0,0,20\n"); }, 2, "not an integer");
    expect_error([&] { wb::parse_ansys_csv(h + "abc,0,0,0,20\n"); }, 2, "node_id");
    expect_error([&] { wb::parse_ansys_csv(h + "-3,0,0,0,20\n"); }, 2, "negative");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,20\n2,0,0,0,-273.16\n"); }, 3, "below absolute zero");
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,-300\n"); }, 2, "below absolute zero");
    // CRLF and BOM must not shift the line numbers
    expect_error([&] { wb::parse_ansys_csv("\xEF\xBB\xBF" + to_crlf(h + "1,0,0,0,20\n2,0,0,0\n")); }, 3, "found 4");
}

WB_TEST(csv_duplicate_ids) {
    std::string h = kHdr;
    // adjacent duplicate, duplicate after out-of-order ids, and a late duplicate of an early id
    expect_error([&] { wb::parse_ansys_csv(h + "1,0,0,0,20\n2,0,0,0,20\n2,1,1,1,30\n"); }, 4, "duplicate node_id 2");
    expect_error([&] { wb::parse_ansys_csv(h + "5,0,0,0,20\n3,0,0,0,20\n9,0,0,0,20\n3,0,0,0,21\n"); }, 5, "duplicate node_id 3");
    expect_error([&] { wb::parse_ansys_csv(h + "10,0,0,0,20\n11,0,0,0,20\n12,0,0,0,20\n10,0,0,0,22\n"); }, 5, "duplicate node_id 10");
}

// ============================================================ big inputs

WB_TEST(stl_size_200k_triangles) {
    const std::size_t n = 200000;
    std::string text = make_big_stl(n);
    auto t0 = std::chrono::steady_clock::now();
    wb::StlMesh m = wb::parse_stl_ascii(text);
    double ms = ms_since(t0);
    CHECK(m.triangle_count == n);
    CHECK(m.positions.size() == n * 9 && m.normals.size() == n * 3);
    std::cout << "    STL " << n << " triangles, " << text.size() / 1048576.0 << " MB: parsed in " << ms << " ms\n";
    CHECK(ms < 3000);   // generous: typically a few hundred ms at -O2
}

WB_TEST(csv_size_500k_rows) {
    const std::size_t n = 500000;
    std::string text = make_big_csv(n);
    auto t0 = std::chrono::steady_clock::now();
    wb::ThermalField f = wb::parse_ansys_csv(text);
    double ms = ms_since(t0);
    CHECK(f.nodes.size() == n);
    CHECK(f.nodes.back().id == static_cast<std::int64_t>(n));
    std::cout << "    CSV " << n << " rows, " << text.size() / 1048576.0 << " MB: parsed in " << ms << " ms\n";
    CHECK(ms < 3000);
}

WB_TEST(csv_size_500k_rows_unsorted_ids) {
    // Descending ids force the hash-set duplicate check, the slower path.
    const std::size_t n = 500000;
    std::string text = kHdr;
    char buf[96];
    for (std::size_t i = 0; i < n; ++i) {
        int len = std::snprintf(buf, sizeof buf, "%zu,%.3f,0,0,25.5\n", n - i, double(i) * 1e-3);
        text.append(buf, static_cast<std::size_t>(len));
    }
    auto t0 = std::chrono::steady_clock::now();
    wb::ThermalField f = wb::parse_ansys_csv(text);
    double ms = ms_since(t0);
    CHECK(f.nodes.size() == n);
    std::cout << "    CSV " << n << " rows with unsorted ids: parsed in " << ms << " ms\n";
    CHECK(ms < 5000);
    // and a duplicate hidden at the very end is still found on the right line
    text += "250000,0,0,0,25.5\n";
    expect_error([&] { wb::parse_ansys_csv(text); }, n + 2, "duplicate node_id 250000");
}

WB_TEST(size_limits_rejected) {
    std::string big(wb::kMaxStlBytes + 1, ' ');
    expect_error([&] { wb::parse_stl_ascii(big); }, 0, "too large");
    big.assign(wb::kMaxCsvBytes + 1, ' ');
    expect_error([&] { wb::parse_ansys_csv(big); }, 0, "too large");
}

// Near-limit run for the numbers in docs/FORMATS.md. Slow and memory hungry, so it is
// opt-in:  WB_BIG_TESTS=1 ./tests
WB_TEST(big_inputs_near_limit) {
    if (!std::getenv("WB_BIG_TESTS")) { std::cout << "    (skipped; set WB_BIG_TESTS=1 to run)\n"; return; }
    {
        std::size_t n = 800000;   // roughly 190 MB of text, just under kMaxStlBytes
        std::string text = make_big_stl(n);
        auto t0 = std::chrono::steady_clock::now();
        wb::StlMesh m = wb::parse_stl_ascii(text);
        std::cout << "    STL " << n << " triangles, " << text.size() / 1048576.0 << " MB: " << ms_since(t0) << " ms\n";
        CHECK(m.triangle_count == n);
    }
    {
        std::size_t n = wb::kMaxCsvRows;   // the row limit is reached long before the byte limit
        std::string text = make_big_csv(n);
        auto t0 = std::chrono::steady_clock::now();
        wb::ThermalField f = wb::parse_ansys_csv(text);
        std::cout << "    CSV " << n << " rows, " << text.size() / 1048576.0 << " MB: " << ms_since(t0) << " ms\n";
        CHECK(f.nodes.size() == n);
        text += "5000001,0,0,0,20\n";
        expect_error([&] { wb::parse_ansys_csv(text); }, n + 2, "too many rows");
    }
}
