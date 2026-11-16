#pragma once
#include <iostream>
#include <string>

namespace test {
inline int failures = 0;
inline void Check(bool condition, const std::string& name) {
    if (condition) std::cout << "[PASS] " << name << '\n';
    else { std::cerr << "[FAIL] " << name << '\n'; ++failures; }
}
} // namespace test
