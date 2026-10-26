#ifndef SPL_TEST_FRAMEWORK_H
#define SPL_TEST_FRAMEWORK_H

#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace spltest {

class Failure : public std::exception {
public:
    explicit Failure(const std::string& text) : message_(text) {}
    ~Failure() noexcept {}

    const char* what() const noexcept {
        return message_.c_str();
    }

private:
    std::string message_;
};

struct Case {
    const char* name;
    void (*function)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> registered;
    return registered;
}

inline long& checks() {
    static long count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, void (*function)()) {
        Case test_case;
        test_case.name = name;
        test_case.function = function;
        cases().push_back(test_case);
    }
};

template <typename T>
std::string value_to_string(const T& value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

inline std::string value_to_string(const std::string& value) {
    return "\"" + value + "\"";
}

inline std::string value_to_string(const char* value) {
    return std::string("\"") + (value ? value : "null") + "\"";
}

template <typename A, typename B>
bool equal_checks(const A& left, const B& right) {
    typedef typename std::common_type<A, B>::type Common;
    return static_cast<Common>(left) == static_cast<Common>(right);
}

inline int run_all() {
    int failed = 0;
    for (std::size_t i = 0; i < cases().size(); ++i) {
        const Case& test_case = cases()[i];
        std::cout << "[ RUN      ] " << test_case.name << std::endl;
        try {
            test_case.function();
            std::cout << "[       OK ] " << test_case.name << std::endl;
        } catch (const Failure& failure) {
            ++failed;
            std::cout << "[  FAILED  ] " << test_case.name << "\n             "
                      << failure.what() << std::endl;
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[  FAILED  ] " << test_case.name
                      << " (unexpected exception: " << error.what() << ")" << std::endl;
        } catch (...) {
            ++failed;
            std::cout << "[  FAILED  ] " << test_case.name
                      << " (unexpected non-standard exception)" << std::endl;
        }
    }
    std::cout << "[==========] " << cases().size() << " tests, " << checks()
              << " checks, " << failed << " failed" << std::endl;
    return failed == 0 ? 0 : 1;
}

}

#define SPL_TEST(name)                                                \
    static void name();                                               \
    static ::spltest::Registrar name##_registrar(#name, &name);       \
    static void name()

#define SPL_CHECK(expression)                                         \
    do {                                                              \
        ++::spltest::checks();                                        \
        if (!(expression)) {                                          \
            std::ostringstream stream;                                \
            stream << __FILE__ << ":" << __LINE__                     \
                   << " check failed: " #expression;                  \
            throw ::spltest::Failure(stream.str());                   \
        }                                                             \
    } while (0)

#define SPL_CHECK_EQ(left, right)                                     \
    do {                                                              \
        ++::spltest::checks();                                        \
        const auto& check_left = (left);                              \
        const auto& check_right = (right);                            \
        if (!::spltest::equal_checks(check_left, check_right)) {      \
            std::ostringstream stream;                                \
            stream << __FILE__ << ":" << __LINE__                     \
                   << " expected " #left " == " #right "\n"           \
                   << "             actual: "                         \
                   << ::spltest::value_to_string(check_left)          \
                   << " vs "                                          \
                   << ::spltest::value_to_string(check_right);        \
            throw ::spltest::Failure(stream.str());                   \
        }                                                             \
    } while (0)

#define SPL_CHECK_THROW(expression, exception_type)                   \
    do {                                                              \
        ++::spltest::checks();                                        \
        bool caught_expected = false;                                 \
        try {                                                         \
            expression;                                               \
        } catch (const exception_type&) {                             \
            caught_expected = true;                                   \
        } catch (...) {                                               \
            std::ostringstream stream;                                \
            stream << __FILE__ << ":" << __LINE__                     \
                   << " threw the wrong exception type, expected "    \
                   << #exception_type;                                \
            throw ::spltest::Failure(stream.str());                   \
        }                                                             \
        if (!caught_expected) {                                       \
            std::ostringstream stream;                                \
            stream << __FILE__ << ":" << __LINE__                     \
                   << " expected exception " << #exception_type;      \
            throw ::spltest::Failure(stream.str());                   \
        }                                                             \
    } while (0)

#endif
