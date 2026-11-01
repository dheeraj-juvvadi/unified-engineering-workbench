// ansys_csv.cpp - nodal temperature CSV parser (see ansys_csv.h for the contract).
#include "ansys_csv.h"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>

#include "parse_util.h"

namespace wb {
namespace {

using detail::clip;
using detail::Num;

constexpr int kCols = 5;
const char* kNames[kCols] = {"node_id", "x_m", "y_m", "z_m", "temperature_C"};
const char* kHeader = "node_id,x_m,y_m,z_m,temperature_C";
constexpr double kAbsZeroC = -273.15;

[[noreturn]] void fail(std::size_t line, const std::string& msg, const std::string& hint) {
    throw ParseError(line ? "Line " + std::to_string(line) + ": " + msg : msg, line, hint);
}

std::string header_hint() {
    return std::string("Expected header (case-sensitive): ") + kHeader + ". Units: metres and degrees Celsius.";
}

// Splits `row` on commas into at most kCols fields; returns the real field count.
int split_row(std::string_view row, std::string_view out[kCols]) {
    int n = 0;
    std::size_t start = 0;
    while (true) {
        std::size_t comma = row.find(',', start);
        std::string_view f = row.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (n < kCols) out[n] = detail::trim(f);
        ++n;
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return n;
}

double read_number(std::string_view tok, std::size_t line, int col) {
    std::string where = "column " + std::to_string(col + 1) + " (" + kNames[col] + ")";
    if (tok.empty()) fail(line, where + " is empty", "Every row needs a value in all " + std::to_string(kCols) + " columns.");
    double v = 0;
    switch (detail::parse_double(tok, v)) {
        case Num::Ok: return v;
        case Num::Invalid:
            fail(line, where + " value '" + clip(tok) + "' is not a number",
                 "Use plain decimal or scientific notation (0.125 or 1.25e-1), without units, quotes or thousands separators.");
        case Num::Overflow:
            fail(line, where + " value '" + clip(tok) + "' is out of range (overflows to infinity)",
                 "Values must be finite numbers; check the solver export.");
        case Num::NotFinite:
            fail(line, where + " value '" + clip(tok) + "' is not finite (nan/inf are rejected)",
                 "The export contains NaN/Inf, usually from unconverged nodes; fix or remove those rows.");
    }
    return 0;
}

}  // namespace

ThermalField parse_ansys_csv(std::string_view text) {
    if (text.empty())
        fail(0, "File is empty (0 bytes). Nothing to load.", header_hint());
    if (text.size() > kMaxCsvBytes)
        fail(0, "File is too large (" + std::to_string(text.size() / (1024 * 1024)) + " MB, limit " +
                    std::to_string(kMaxCsvBytes / (1024 * 1024)) + " MB).",
             "Export a subset of the nodes (or a coarser mesh).");
    text = detail::strip_bom(text);

    detail::LineReader lines{text};
    std::string_view line;

    // --- header: first non-blank line ---
    bool have_header = false;
    while (lines.next(line)) {
        if (detail::trim(line).empty()) continue;
        have_header = true;
        break;
    }
    if (!have_header) fail(0, "File contains only whitespace (no header and no data).", header_hint());

    const std::size_t header_line = lines.line_no;
    std::string_view f[kCols];
    int nf = split_row(line, f);
    if (nf != kCols)
        fail(header_line, "header has " + std::to_string(nf) + " columns, expected " + std::to_string(kCols) + " (" + kHeader + ")",
             header_hint());
    for (int i = 0; i < kCols; ++i)
        if (f[i] != kNames[i])
            fail(header_line, "header column " + std::to_string(i + 1) + " is '" + clip(f[i]) + "', expected '" + kNames[i] + "'",
                 header_hint());

    // --- data rows ---
    ThermalField out;
    out.nodes.reserve(std::min<std::size_t>(std::count(text.begin(), text.end(), '\n') + 1, kMaxCsvRows));
    constexpr double inf = std::numeric_limits<double>::infinity();
    out.bbox_min = {inf, inf, inf};
    out.bbox_max = {-inf, -inf, -inf};
    out.t_min = inf;
    out.t_max = -inf;

    // Duplicate check: while ids arrive strictly increasing (the normal case) no duplicate is
    // possible, so no set is needed. On the first out-of-order id we build a hash set once.
    bool sorted = true;
    std::unordered_set<std::int64_t> seen;

    while (lines.next(line)) {
        if (detail::trim(line).empty()) continue;
        const std::size_t ln = lines.line_no;
        int n = split_row(line, f);
        if (n != kCols)
            fail(ln, "expected " + std::to_string(kCols) + " columns (" + kHeader + "), found " + std::to_string(n),
                 n > kCols ? "Remove the extra column or trailing comma; the format has exactly five columns."
                           : "Every row needs node_id,x_m,y_m,z_m,temperature_C. Is a decimal comma or a missing value splitting the row?");
        if (out.nodes.size() >= kMaxCsvRows)
            fail(ln, "too many rows (limit " + std::to_string(kMaxCsvRows) + ")", "Export a subset of the nodes.");

        // node_id must be a non-negative integer
        std::int64_t id = 0;
        {
            const char* b = f[0].data();
            const char* e = b + f[0].size();
            auto r = std::from_chars(b, e, id);
            if (f[0].empty() || r.ec != std::errc() || r.ptr != e)
                fail(ln, "column 1 (node_id) value '" + clip(f[0]) + "' is not an integer",
                     "node_id must be a whole number such as 17 (no decimals).");
            if (id < 0) fail(ln, "node_id " + std::to_string(id) + " is negative", "Node ids must be >= 0.");
        }

        double x = read_number(f[1], ln, 1), y = read_number(f[2], ln, 2), z = read_number(f[3], ln, 3);
        double t = read_number(f[4], ln, 4);
        if (t < kAbsZeroC)
            fail(ln, "temperature " + std::string(f[4]) + " degC is below absolute zero (-273.15 degC)",
                 "Temperatures must be in degrees Celsius. Values in kelvin are not accepted; subtract 273.15.");

        // duplicate id?
        if (sorted && !out.nodes.empty() && id <= out.nodes.back().id) {
            sorted = false;
            seen.reserve(out.nodes.capacity());
            for (const auto& nd : out.nodes) seen.insert(nd.id);
        }
        if (!sorted && !seen.insert(id).second)
            fail(ln, "duplicate node_id " + std::to_string(id) + " (already used earlier in the file)",
                 "Each node must appear once; remove or renumber the repeated row.");

        out.nodes.push_back({id, x, y, z, t});
        out.bbox_min = {std::min(out.bbox_min[0], x), std::min(out.bbox_min[1], y), std::min(out.bbox_min[2], z)};
        out.bbox_max = {std::max(out.bbox_max[0], x), std::max(out.bbox_max[1], y), std::max(out.bbox_max[2], z)};
        out.t_min = std::min(out.t_min, t);
        out.t_max = std::max(out.t_max, t);
    }

    if (out.nodes.empty())
        fail(header_line, "file contains a header but no data rows",
             "Add one row per node: " + std::string(kHeader) + " (e.g. 1,0.0,0.0,0.0,20.5).");
    return out;
}

}  // namespace wb
