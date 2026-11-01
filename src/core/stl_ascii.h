// stl_ascii.h - strict ASCII STL parser.
#pragma once
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include "parse_error.h"

namespace wb {

struct StlMesh {
    std::string name;                 // from "solid <name>"
    std::vector<float> positions;     // 9 floats per triangle (v0,v1,v2 xyz)
    std::vector<float> normals;       // 3 floats per triangle (as stored in file)
    std::size_t triangle_count = 0;
    std::array<double, 3> bbox_min{}, bbox_max{};
};

// Limits (documented in docs/FORMATS.md, verified in tests).
constexpr std::size_t kMaxStlBytes = 200u * 1024u * 1024u;   // 200 MB
constexpr std::size_t kMaxStlTriangles = 2'000'000;

// Throws ParseError with 1-based line on: empty/whitespace-only file, binary STL,
// missing "solid", bad keyword order, wrong vertex count per facet, unparsable or
// non-finite numbers (nan/inf), missing "endsolid", zero facets, size limits.
StlMesh parse_stl_ascii(std::string_view text);

}  // namespace wb
