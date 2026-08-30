// Minimal test harness.
//
// Deliberately not a framework: the whole suite is a few hundred assertions over
// pure functions, and a dependency would cost more to wire up than it saves.
#include "test.hpp"

#include <cstdio>

namespace hnx::test {

Registry& Registry::instance() {
    static Registry r;
    return r;
}

void Registry::add(const char* name, void (*fn)()) { cases_.push_back({name, fn}); }

int Registry::runAll() {
    int failedCases = 0;

    for (const auto& c : cases_) {
        failures_.clear();
        current_ = c.name;
        c.fn();

        if (failures_.empty()) {
            std::printf("  PASS  %s\n", c.name);
        } else {
            ++failedCases;
            std::printf("  FAIL  %s\n", c.name);
            for (const auto& f : failures_) std::printf("          %s\n", f.c_str());
        }
    }

    std::printf("\n%zu cases, %d failed, %d assertions\n", cases_.size(), failedCases,
                assertions_);
    return failedCases == 0 ? 0 : 1;
}

void Registry::recordFailure(std::string message) { failures_.push_back(std::move(message)); }
void Registry::countAssertion() { ++assertions_; }

}  // namespace hnx::test

int main() {
    std::printf("hnx tests\n");
    return hnx::test::Registry::instance().runAll();
}
