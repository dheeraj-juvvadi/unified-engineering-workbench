// parse_util.h - small helpers shared by the STL and CSV parsers (internal, header only).
#pragma once
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace wb::detail {

inline bool is_blank_char(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && is_blank_char(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_blank_char(s.back())) s.remove_suffix(1);
    return s;
}

// Shortens a token for use inside an error message.
inline std::string clip(std::string_view s, std::size_t max_len = 24) {
    if (s.size() <= max_len) return std::string(s);
    return std::string(s.substr(0, max_len)) + "...";
}

// Walks over '\n'-terminated lines and counts them (1-based). A trailing '\r' is dropped
// so LF and CRLF files behave the same. Lone '\r' (classic Mac) is not a line break.
struct LineReader {
    std::string_view text;
    std::size_t pos = 0;
    std::size_t line_no = 0;   // number of the line returned by the last next()

    bool next(std::string_view& line) {
        if (pos >= text.size()) return false;
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = end + 1;
        ++line_no;
        return true;
    }
};

// Strips a UTF-8 byte order mark if present.
inline std::string_view strip_bom(std::string_view s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
        s.remove_prefix(3);
    return s;
}

enum class Num { Ok, Invalid, Overflow, NotFinite };

// Parses one whole token as a finite double. "nan"/"inf" are recognised and reported as
// NotFinite, "1e999" as Overflow; values that underflow to zero are accepted as 0.
inline Num parse_double(std::string_view tok, double& out) {
    if (tok.empty()) return Num::Invalid;
    const char* b = tok.data();
    const char* e = b + tok.size();
    if (*b == '+' && tok.size() > 1 && b[1] != '-' && b[1] != '+') ++b;   // from_chars rejects '+'
#if defined(__cpp_lib_to_chars)
    double v = 0;
    auto r = std::from_chars(b, e, v);
    if (r.ec == std::errc::result_out_of_range) {
        // from_chars reports both overflow and underflow the same way; look at the exponent sign.
        std::size_t epos = tok.find_first_of("eE");
        if (epos != std::string_view::npos && epos + 1 < tok.size() && tok[epos + 1] == '-') {
            out = 0.0;
            return Num::Ok;
        }
        return Num::Overflow;
    }
    if (r.ec != std::errc() || r.ptr != e) return Num::Invalid;
#else
    // Fallback for standard libraries without floating-point from_chars.
    char buf[64];
    std::size_t n = static_cast<std::size_t>(e - b);
    if (n >= sizeof buf) return Num::Invalid;
    for (std::size_t i = 0; i < n; ++i) buf[i] = b[i];
    buf[n] = '\0';
    char* endp = nullptr;
    double v = std::strtod(buf, &endp);
    if (endp != buf + n) return Num::Invalid;
#endif
    if (!std::isfinite(v)) {
        // strtod (and some from_chars versions) turn "1e999" into inf instead of reporting
        // an error. Only a literal "inf"/"nan" token counts as NotFinite; anything else
        // that became infinite overflowed. Keeps messages identical on every platform.
        bool literal = tok.find_first_of("iInN") != std::string_view::npos;
        return (std::isinf(v) && !literal) ? Num::Overflow : Num::NotFinite;
    }
    out = v;
    return Num::Ok;
}

}  // namespace wb::detail
