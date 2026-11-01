// ansys_csv.h - parser for ANSYS-style nodal temperature export.
// Required header (exact, case-sensitive, whitespace around names tolerated):
//   node_id,x_m,y_m,z_m,temperature_C
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include "parse_error.h"

namespace wb {

struct ThermalNode { std::int64_t id; double x, y, z, t; };   // m, m, m, degC

struct ThermalField {
    std::vector<ThermalNode> nodes;
    double t_min = 0, t_max = 0;                  // degC
    std::array<double, 3> bbox_min{}, bbox_max{}; // m
};

constexpr std::size_t kMaxCsvBytes = 200u * 1024u * 1024u;   // 200 MB
constexpr std::size_t kMaxCsvRows = 5'000'000;

// Throws ParseError (1-based line) on: empty file, header-only file (no data),
// wrong/missing header, wrong column count, unparsable or non-finite numbers,
// duplicate node_id, negative node_id, temperature below absolute zero (-273.15),
// size limits. Accepts LF or CRLF, an optional UTF-8 BOM, trailing blank lines.
ThermalField parse_ansys_csv(std::string_view text);

}  // namespace wb
