#pragma once

#include <string>
#include <vector>

namespace hnx::test {

class Registry {
public:
    static Registry& instance();

    void add(const char* name, void (*fn)());
    int runAll();
    void recordFailure(std::string message);
    void countAssertion();

private:
    struct Case {
        const char* name;
        void (*fn)();
    };
    std::vector<Case> cases_;
    std::vector<std::string> failures_;
    const char* current_ = nullptr;
    int assertions_ = 0;
};

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry::instance().add(name, fn); }
};

}  // namespace hnx::test

#define HNX_TEST(name)                                                        \
    static void name();                                                       \
    static ::hnx::test::Registrar registrar_##name{#name, &name};             \
    static void name()

#define HNX_CHECK(expr)                                                       \
    do {                                                                      \
        ::hnx::test::Registry::instance().countAssertion();                   \
        if (!(expr))                                                          \
            ::hnx::test::Registry::instance().recordFailure(                  \
                std::string(__FILE__ ":") + std::to_string(__LINE__) +        \
                "  expected: " #expr);                                        \
    } while (0)

#define HNX_CHECK_EQ(a, b)                                                    \
    do {                                                                      \
        ::hnx::test::Registry::instance().countAssertion();                   \
        if (!((a) == (b)))                                                    \
            ::hnx::test::Registry::instance().recordFailure(                  \
                std::string(__FILE__ ":") + std::to_string(__LINE__) +        \
                "  expected " #a " == " #b);                                  \
    } while (0)
