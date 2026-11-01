// parse_error.h - error type shared by the STL and CSV parsers.
#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>

namespace wb {

// Thrown by parsers. `line` is 1-based (0 = whole file, e.g. empty input).
// `hint` is a short, actionable suggestion shown to the user.
struct ParseError : std::runtime_error {
    std::size_t line;
    std::string hint;
    ParseError(const std::string& msg, std::size_t line_no, std::string hint_text = {})
        : std::runtime_error(msg), line(line_no), hint(std::move(hint_text)) {}
};

}  // namespace wb
