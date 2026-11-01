// stl_ascii.cpp - strict ASCII STL parser (see stl_ascii.h for the contract).
#include "stl_ascii.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <limits>

#include "parse_util.h"

namespace wb {
namespace {

using detail::clip;
using detail::Num;

[[noreturn]] void fail(std::size_t line, const std::string& msg, const std::string& hint) {
    throw ParseError(line ? "Line " + std::to_string(line) + ": " + msg : msg, line, hint);
}

const char* kHintAscii = "Export the model as ASCII STL (not binary) from your CAD tool.";

// Case-insensitive comparison against a lowercase keyword.
bool ieq(std::string_view a, std::string_view lower) {
    if (a.size() != lower.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != lower[i]) return false;
    return true;
}

// Splits a line into whitespace separated tokens, one at a time.
struct Cursor {
    std::string_view s;
    std::string_view next() {
        std::size_t i = 0;
        while (i < s.size() && detail::is_blank_char(s[i])) ++i;
        std::size_t j = i;
        while (j < s.size() && !detail::is_blank_char(s[j])) ++j;
        std::string_view tok = s.substr(i, j - i);
        s.remove_prefix(j);
        return tok;
    }
    std::string_view rest() const { return detail::trim(s); }
};

// Binary STL has a free-form 80 byte header, so "solid" at the start proves nothing.
// Control characters (NUL in particular, which float data is full of) give it away.
bool looks_binary(std::string_view text) {
    std::size_t n = std::min<std::size_t>(text.size(), 512);
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        bool ok_ws = (c == '\t' || c == '\n' || c == '\r');
        if ((c < 0x20 && !ok_ws) || c == 0x7F) return true;
    }
    // Exact size match for 80 + 4 + 50 * n also means binary, even with a printable header.
    if (text.size() >= 84) {
        std::uint32_t count = 0;
        for (int k = 3; k >= 0; --k) count = (count << 8) | static_cast<unsigned char>(text[80 + k]);
        if (count > 0 && text.size() == 84 + static_cast<std::size_t>(count) * 50)
            return true;
    }
    return false;
}

// Reads three numbers from the rest of a line (used for "facet normal" and "vertex").
void read_vec3(Cursor& c, std::size_t line, const char* what, double out[3]) {
    static const char* axis[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        std::string_view tok = c.next();
        if (tok.empty())
            fail(line, std::string(what) + " needs 3 numbers (x y z), found " + std::to_string(i),
                 "Each '" + std::string(what) + "' line must look like: " + what + " 1.0 2.0 3.0");
        double v = 0;
        switch (detail::parse_double(tok, v)) {
            case Num::Ok: break;
            case Num::Invalid:
                fail(line, std::string(what) + " " + axis[i] + " value '" + clip(tok) + "' is not a number",
                     "Check the file for stray characters; ASCII STL numbers look like 1.5 or -2.5e-3.");
            case Num::Overflow:
                fail(line, std::string(what) + " " + axis[i] + " value '" + clip(tok) + "' is out of range (overflows to infinity)",
                     "Coordinates must be finite numbers; re-export the mesh.");
            case Num::NotFinite:
                fail(line, std::string(what) + " " + axis[i] + " value '" + clip(tok) + "' is not finite (nan/inf are rejected)",
                     "The mesh contains invalid coordinates; repair or re-export it in your CAD tool.");
        }
        if (std::fabs(v) > FLT_MAX)
            fail(line, std::string(what) + " " + axis[i] + " value '" + clip(tok) + "' does not fit in a 32-bit float",
                 "Rescale the model so coordinates stay below 3e38.");
        out[i] = v;
    }
    if (!c.rest().empty())
        fail(line, std::string("unexpected text after ") + what + " values: '" + clip(c.rest()) + "'",
             "Each line may hold exactly three numbers.");
}

enum class St { Facet, Outer, Vertex, EndLoop, EndFacet };

const char* expected_text(St s) {
    switch (s) {
        case St::Facet:    return "'facet normal ...' or 'endsolid'";
        case St::Outer:    return "'outer loop'";
        case St::Vertex:   return "'vertex x y z'";
        case St::EndLoop:  return "'endloop'";
        case St::EndFacet: return "'endfacet'";
    }
    return "";
}

}  // namespace

StlMesh parse_stl_ascii(std::string_view text) {
    if (text.empty())
        fail(0, "File is empty (0 bytes). Nothing to load.", "Choose a non-empty ASCII STL file (it starts with 'solid').");
    if (text.size() > kMaxStlBytes)
        fail(0, "File is too large (" + std::to_string(text.size() / (1024 * 1024)) + " MB, limit " +
                    std::to_string(kMaxStlBytes / (1024 * 1024)) + " MB).",
             "Decimate the mesh or export a coarser tessellation.");

    text = detail::strip_bom(text);
    if (text.find_first_not_of(" \t\r\n\v\f") == std::string_view::npos)
        fail(0, "File contains only whitespace (no 'solid' header).", kHintAscii);
    if (looks_binary(text))
        fail(0, "This looks like a binary STL file; only ASCII STL is supported.", "Export as ASCII STL.");

    StlMesh mesh;
    mesh.positions.reserve(std::min<std::size_t>(text.size() / 200, kMaxStlTriangles) * 9);
    mesh.normals.reserve(std::min<std::size_t>(text.size() / 200, kMaxStlTriangles) * 3);
    constexpr double inf = std::numeric_limits<double>::infinity();
    mesh.bbox_min = {inf, inf, inf};
    mesh.bbox_max = {-inf, -inf, -inf};

    detail::LineReader lines{text};
    std::string_view line;
    bool seen_solid = false, seen_end = false;
    St st = St::Facet;
    int nverts = 0;
    double nrm[3] = {0, 0, 0}, v[3][3] = {};
    std::size_t last_content_line = 0;

    while (lines.next(line)) {
        Cursor c{line};
        std::string_view kw = c.next();
        if (kw.empty()) continue;               // blank line
        std::size_t ln = lines.line_no;
        last_content_line = ln;

        if (seen_end)
            fail(ln, "unexpected content after 'endsolid': '" + clip(line) + "'",
                 "Only one solid per file is supported; remove the extra data.");

        if (!seen_solid) {
            if (!ieq(kw, "solid"))
                fail(ln, "expected 'solid <name>' at the start of the file, found '" + clip(kw) + "'",
                     "This does not look like an ASCII STL file. " + std::string(kHintAscii));
            mesh.name = std::string(c.rest());
            seen_solid = true;
            continue;
        }

        switch (st) {
            case St::Facet:
                if (ieq(kw, "endsolid")) {
                    if (mesh.triangle_count == 0)
                        fail(ln, "the solid contains no facets", "The file has a header but no triangles; re-export the model.");
                    seen_end = true;
                } else if (ieq(kw, "facet")) {
                    if (!ieq(c.next(), "normal"))
                        fail(ln, "expected 'facet normal nx ny nz'", "Each facet starts with: facet normal 0 0 1");
                    if (mesh.triangle_count >= kMaxStlTriangles)
                        fail(ln, "too many triangles (limit " + std::to_string(kMaxStlTriangles) + ")",
                             "Decimate the mesh before loading it.");
                    read_vec3(c, ln, "normal", nrm);
                    st = St::Outer;
                } else {
                    fail(ln, "expected " + std::string(expected_text(st)) + ", found '" + clip(kw) + "'",
                         "The facet structure is broken; check the file around this line.");
                }
                break;
            case St::Outer:
                if (!ieq(kw, "outer") || !ieq(c.next(), "loop"))
                    fail(ln, "expected 'outer loop', found '" + clip(kw) + "'", "Every facet needs an 'outer loop' line.");
                nverts = 0;
                st = St::Vertex;
                break;
            case St::Vertex:
                if (ieq(kw, "vertex")) {
                    if (nverts == 3)
                        fail(ln, "facet has more than 3 vertices; only triangles are supported",
                             "Triangulate the mesh before exporting.");
                    read_vec3(c, ln, "vertex", v[nverts]);
                    if (++nverts == 3) st = St::EndLoop;
                } else {
                    fail(ln, "facet has only " + std::to_string(nverts) + " vertices (expected 3), found '" + clip(kw) + "'",
                         "Each facet needs exactly three 'vertex x y z' lines.");
                }
                break;
            case St::EndLoop:
                if (ieq(kw, "vertex"))
                    fail(ln, "facet has more than 3 vertices; only triangles are supported", "Triangulate the mesh before exporting.");
                if (!ieq(kw, "endloop"))
                    fail(ln, "expected 'endloop', found '" + clip(kw) + "'", "Each facet needs exactly three vertices then 'endloop'.");
                st = St::EndFacet;
                break;
            case St::EndFacet:
                if (!ieq(kw, "endfacet"))
                    fail(ln, "expected 'endfacet', found '" + clip(kw) + "'", "Close every facet with 'endfacet'.");
                // Facet complete: store it and grow the bounding box.
                for (int i = 0; i < 3; ++i) mesh.normals.push_back(static_cast<float>(nrm[i]));
                for (int k = 0; k < 3; ++k)
                    for (int i = 0; i < 3; ++i) {
                        mesh.positions.push_back(static_cast<float>(v[k][i]));
                        mesh.bbox_min[i] = std::min(mesh.bbox_min[i], v[k][i]);
                        mesh.bbox_max[i] = std::max(mesh.bbox_max[i], v[k][i]);
                    }
                ++mesh.triangle_count;
                st = St::Facet;
                break;
        }
    }

    if (!seen_end) {
        if (st == St::Facet)
            fail(last_content_line, "end of file reached without 'endsolid'",
                 "The file looks truncated; re-export it or add an 'endsolid' line at the end.");
        fail(last_content_line, "file ends in the middle of a facet (expected " + std::string(expected_text(st)) + ")",
             "The file looks truncated; re-export it.");
    }
    return mesh;
}

}  // namespace wb
