// test_main.cpp - runs every WB_TEST. Optional argv[1] = substring filter.
#include <chrono>
#include <cstring>
#include "wb_test.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0;
    for (auto& c : wbt::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        auto t0 = std::chrono::steady_clock::now();
        try {
            c.fn();
            ++passed;
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::cout << "[PASS] " << c.name << " (" << ms << " ms)\n";
        } catch (const wbt::Failure& f) {
            ++failed; std::cout << "[FAIL] " << c.name << ": " << f.msg << "\n";
        } catch (const std::exception& e) {
            ++failed; std::cout << "[FAIL] " << c.name << ": unexpected exception: " << e.what() << "\n";
        }
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
