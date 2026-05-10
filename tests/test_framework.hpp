#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <future>
#include <chrono>

namespace test {

// Default test timeout in seconds (30 seconds per test)
constexpr int DEFAULT_TEST_TIMEOUT_SEC = 30;

// Simple test framework without external dependencies

struct TestCase {
    const char* name;
    std::function<bool()> func;
};

inline std::vector<TestCase>& get_tests() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& passed_count() {
    static int count = 0;
    return count;
}

inline int& failed_count() {
    static int count = 0;
    return count;
}

#define TEST(name) \
    static bool test_##name(); \
    static bool test_##name##_registered = []() { \
        test::get_tests().push_back({#name, test_##name}); \
        return true; \
    }(); \
    static bool test_##name()

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::fprintf(stderr, "  FAILED: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::fprintf(stderr, "  FAILED: %s != %s at %s:%d\n", #a, #b, __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

#define ASSERT_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            std::fprintf(stderr, "  FAILED: %s == %s at %s:%d\n", #a, #b, __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

#define ASSERT_LT(a, b) \
    do { \
        if (!((a) < (b))) { \
            std::fprintf(stderr, "  FAILED: %s >= %s at %s:%d\n", #a, #b, __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

#define ASSERT_GT(a, b) \
    do { \
        if (!((a) > (b))) { \
            std::fprintf(stderr, "  FAILED: %s <= %s at %s:%d\n", #a, #b, __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

#define ASSERT_NEAR(a, b, epsilon) \
    do { \
        if (std::abs((a) - (b)) > (epsilon)) { \
            std::fprintf(stderr, "  FAILED: |%s - %s| > %s at %s:%d\n", #a, #b, #epsilon, __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        if (std::strcmp((a), (b)) != 0) { \
            std::fprintf(stderr, "  FAILED: \"%s\" != \"%s\" at %s:%d\n", (a), (b), __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

// Run a single test with timeout protection
inline bool run_test_with_timeout(const TestCase& test, int timeout_sec) {
    // Use std::async to run test in separate thread with timeout
    auto future = std::async(std::launch::async, test.func);

    auto status = future.wait_for(std::chrono::seconds(timeout_sec));

    if (status == std::future_status::timeout) {
        std::fprintf(stderr, "  TIMEOUT: Test exceeded %d second limit\n", timeout_sec);
        // Note: We cannot forcefully kill the thread in C++. The test will
        // continue running in the background. This is a limitation, but at
        // least we can report the timeout and continue with other tests.
        return false;
    }

    // Note: With -fno-exceptions, future.get() won't throw, so no try/catch needed
    return future.get();
}

inline int run_all_tests() {
    std::printf("Running %zu tests...\n\n", get_tests().size());

    for (const auto& test : get_tests()) {
        std::printf("[ RUN  ] %s\n", test.name);
        std::fflush(stdout);

        // Run test directly (timeout protection via run_test_with_timeout can cause issues
        // with fork/exec in integration tests)
        const bool passed = test.func();

        if (passed) {
            std::printf("[ PASS ] %s\n", test.name);
            ++passed_count();
        } else {
            std::printf("[ FAIL ] %s\n", test.name);
            ++failed_count();
        }
        std::fflush(stdout);
    }

    std::printf("\n========================================\n");
    std::printf("Passed: %d, Failed: %d, Total: %zu\n",
                passed_count(), failed_count(), get_tests().size());
    std::printf("========================================\n");

    return failed_count() > 0 ? 1 : 0;
}

} // namespace test
