#ifndef MO_TEST_FRAMEWORK_H
#define MO_TEST_FRAMEWORK_H

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*function)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> value;
    return value;
}

inline int& checks() {
    static int value = 0;
    return value;
}

inline int& failures() {
    static int value = 0;
    return value;
}

inline void fail(const char* file, int line, const std::string& message) {
    ++failures();
    std::printf("  FAIL %s:%d %s\n", file, line, message.c_str());
}

inline void check(bool condition, const char* file, int line, const char* text) {
    ++checks();
    if (!condition) {
        fail(file, line, text);
    }
}

template <typename A, typename B>
void check_eq(const A& actual, const B& expected, const char* file, int line,
              const char* text) {
    ++checks();
    if (!(actual == expected)) {
        fail(file, line, std::string(text) + " (values differ)");
    }
}

template <typename F>
void check_throw(F function, const char* file, int line, const char* text) {
    ++checks();
    try {
        function();
    } catch (...) {
        return;
    }
    fail(file, line, std::string(text) + " (no exception)");
}

struct Registrar {
    Registrar(const char* name, void (*function)()) {
        registry().push_back(Case{name, function});
    }
};

inline int run() {
    for (std::size_t i = 0; i < registry().size(); ++i) {
        const Case& item = registry()[i];
        std::printf("[ RUN      ] %s\n", item.name);
        try {
            item.function();
        } catch (const std::exception& error) {
            ++failures();
            std::printf("  FAIL uncaught exception: %s\n", error.what());
        } catch (...) {
            ++failures();
            std::printf("  FAIL uncaught unknown exception\n");
        }
    }
    std::printf("[==========] %zu tests, %d checks, %d failed\n",
                registry().size(), checks(), failures());
    return failures() == 0 ? 0 : 1;
}

}

#define MO_TEST(name)                                                       \
    static void name();                                                     \
    static test::Registrar registrar_##name(#name, &name);                  \
    static void name()

#define MO_CHECK(condition)                                                 \
    test::check((condition), __FILE__, __LINE__, #condition)

#define MO_CHECK_EQ(actual, expected)                                       \
    test::check_eq((actual), (expected), __FILE__, __LINE__, #actual)

#define MO_CHECK_THROW(expression)                                          \
    test::check_throw([&]() { (void)(expression); }, __FILE__, __LINE__,   \
                      #expression)

#endif
