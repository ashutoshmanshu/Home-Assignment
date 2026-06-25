#ifndef ME_TEST_FRAMEWORK_H
#define ME_TEST_FRAMEWORK_H

// A deliberately tiny, dependency-free unit-test harness.
//
// The assignment allows third-party test frameworks, but a ~60-line header keeps
// the project buildable anywhere with just a compiler and no fetch step. Tests
// self-register via TEST_CASE; CHECK/CHECK_EQ record non-fatal failures so one
// broken assertion doesn't hide the rest. RUN_ALL_TESTS() returns a process exit
// code (0 = all passed).

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace metest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline int& totalFailures() { static int n = 0; return n; }
inline std::string& currentTest() { static std::string s; return s; }
inline int& currentFailures() { static int n = 0; return n; }

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void reportFailure(const char* file, int line, const std::string& expr,
                          const std::string& detail) {
    ++totalFailures();
    ++currentFailures();
    std::cerr << "  FAIL [" << currentTest() << "] " << file << ':' << line << "  " << expr;
    if (!detail.empty()) std::cerr << "  (" << detail << ')';
    std::cerr << '\n';
}

inline int runAll() {
    std::size_t passed = 0;
    for (auto& tc : registry()) {
        currentTest() = tc.name;
        currentFailures() = 0;
        try {
            tc.fn();
        } catch (const std::exception& e) {
            reportFailure("<exception>", 0, tc.name, e.what());
        } catch (...) {
            reportFailure("<exception>", 0, tc.name, "unknown exception");
        }
        if (currentFailures() == 0) {
            std::cout << "  ok   " << tc.name << '\n';
            ++passed;
        }
    }
    std::cout << '\n' << passed << '/' << registry().size() << " tests passed";
    if (totalFailures() > 0) std::cout << ", " << totalFailures() << " checks FAILED";
    std::cout << '\n';
    return totalFailures() == 0 ? 0 : 1;
}

} // namespace metest

#define TEST_CASE(testname)                                                       \
    static void testname();                                                       \
    static metest::Registrar registrar_##testname(#testname, testname);           \
    static void testname()

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) metest::reportFailure(__FILE__, __LINE__, #cond, "");        \
    } while (0)

#define CHECK_FALSE(cond) CHECK(!(cond))

#define CHECK_EQ(actual, expected)                                                \
    do {                                                                          \
        auto _a = (actual);                                                       \
        auto _e = (expected);                                                     \
        if (!(_a == _e)) {                                                        \
            std::ostringstream _os;                                               \
            _os << "expected " << _e << ", got " << _a;                           \
            metest::reportFailure(__FILE__, __LINE__, #actual " == " #expected,   \
                                  _os.str());                                     \
        }                                                                         \
    } while (0)

#endif // ME_TEST_FRAMEWORK_H
