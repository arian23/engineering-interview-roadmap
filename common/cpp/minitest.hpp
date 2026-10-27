#pragma once
// minitest: a tiny, dependency-free unit-test helper shared by every C++ exercise.
//
//   TEST(adds_numbers) {
//       CHECK(1 + 1 == 2);
//       CHECK_EQ(add(2, 3), 5);
//       CHECK_THROWS(parse("x"), std::invalid_argument);
//   }
//   int main() { return minitest::run_all(); }
//
// Notes
// - A failed CHECK throws minitest::failure, which ends the current test and is reported by run_all().
// - Never use CHECK inside a std::thread: an exception escaping a thread calls std::terminate.
//   Record results from threads (atomics, vectors) and CHECK them on the main thread.

#include <chrono>
#include <concepts>
#include <exception>
#include <format>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace minitest {

class failure : public std::exception {
public:
    explicit failure(std::string message) : message_(std::move(message)) {}
    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

struct test_case {
    std::string_view name;
    void (*fn)();
};

inline std::vector<test_case>& registry() {
    static std::vector<test_case> tests;
    return tests;
}

struct registrar {
    registrar(std::string_view name, void (*fn)()) { registry().push_back({name, fn}); }
};

// True when std::format can print a value of type T.
template <typename T>
concept printable = requires { std::formatter<std::remove_cvref_t<T>, char>{}; };

inline void check(bool ok, std::string_view expression,
                  std::source_location where = std::source_location::current()) {
    if (!ok) {
        throw failure(std::format("{}:{}: CHECK({}) failed", where.file_name(), where.line(), expression));
    }
}

template <typename A, typename B>
void check_eq(const A& left, const B& right, std::string_view left_expr, std::string_view right_expr,
              std::source_location where = std::source_location::current()) {
    bool equal = false;
    // std::cmp_equal gives safe signed/unsigned comparison, but it rejects bool and character types.
    if constexpr (std::integral<A> && std::integral<B> && !std::same_as<A, bool> && !std::same_as<B, bool>) {
        equal = std::cmp_equal(left, right);
    } else {
        equal = (left == right);
    }
    if (equal) return;

    std::string message = std::format("{}:{}: CHECK_EQ({}, {}) failed", where.file_name(), where.line(),
                                      left_expr, right_expr);
    if constexpr (printable<A> && printable<B>) {
        message += std::format("  [left = {}, right = {}]", left, right);
    }
    throw failure(std::move(message));
}

inline int run_all() {
    int failed = 0;
    for (const auto& test : registry()) {
        const auto start = std::chrono::steady_clock::now();
        try {
            test.fn();
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
            std::println("[PASS] {} ({:.1f} ms)", test.name, ms.count());
        } catch (const std::exception& error) {
            ++failed;
            std::println("[FAIL] {}\n       {}", test.name, error.what());
        } catch (...) {
            ++failed;
            std::println("[FAIL] {}\n       unknown exception", test.name);
        }
    }
    std::println("\n{} passed, {} failed", registry().size() - static_cast<std::size_t>(failed), failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace minitest

#define MINITEST_CONCAT_(a, b) a##b
#define MINITEST_CONCAT(a, b) MINITEST_CONCAT_(a, b)

#define TEST(name)                                                                     \
    static void name();                                                                \
    static const ::minitest::registrar MINITEST_CONCAT(minitest_registrar_, name){#name, &name}; \
    static void name()

#define CHECK(expr) ::minitest::check(static_cast<bool>(expr), #expr)
#define CHECK_EQ(a, b) ::minitest::check_eq((a), (b), #a, #b)
#define CHECK_THROWS(expr, exception_type)                                                         \
    do {                                                                                           \
        bool minitest_caught_ = false;                                                             \
        try {                                                                                      \
            (void)(expr);                                                                          \
        } catch (const exception_type&) {                                                          \
            minitest_caught_ = true;                                                               \
        }                                                                                          \
        ::minitest::check(minitest_caught_, #expr " throws " #exception_type);                     \
    } while (false)
