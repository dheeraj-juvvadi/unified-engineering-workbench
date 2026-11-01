// wb_test.h - tiny test framework (no external deps). Register with WB_TEST(name).
#pragma once
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace wbt {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
struct Failure { std::string msg; };
}  // namespace wbt

#define WB_CAT2(a, b) a##b
#define WB_CAT(a, b) WB_CAT2(a, b)
#define WB_TEST(name) \
    static void name(); \
    static wbt::Reg WB_CAT(reg_, name)(#name, name); \
    static void name()

#define WB_FAIL_(expr_text) do { std::ostringstream os_; \
    os_ << __FILE__ << ":" << __LINE__ << ": " << expr_text; \
    throw wbt::Failure{os_.str()}; } while (0)
#define CHECK(cond) do { if (!(cond)) WB_FAIL_("CHECK(" #cond ")"); } while (0)
#define CHECK_NEAR(a, b, tol) do { double a_ = (a), b_ = (b); \
    if (!(std::fabs(a_ - b_) <= (tol))) { std::ostringstream m_; \
    m_ << "CHECK_NEAR(" #a ", " #b ") " << a_ << " vs " << b_ << " tol " << (tol); \
    WB_FAIL_(m_.str()); } } while (0)
#define CHECK_THROWS(stmt) do { bool t_ = false; try { stmt; } catch (...) { t_ = true; } \
    if (!t_) WB_FAIL_("CHECK_THROWS(" #stmt ")"); } while (0)
#define CHECK_THROWS_AS(stmt, Ex) do { bool t_ = false; try { stmt; } catch (const Ex&) { t_ = true; } catch (...) {} \
    if (!t_) WB_FAIL_("CHECK_THROWS_AS(" #stmt ", " #Ex ")"); } while (0)
