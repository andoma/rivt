#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

// Minimal test framework — no dependencies
struct TestCase {
    const char *name;
    std::function<void()> fn;
};

static std::vector<TestCase> &test_registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistrar {
    TestRegistrar(const char *name, std::function<void()> fn) {
        test_registry().push_back({name, std::move(fn)});
    }
};

#define TEST(name) \
    static void test_##name(); \
    static TestRegistrar reg_##name(#name, test_##name); \
    static void test_##name()

static int g_failures = 0;

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
        g_failures++; return; \
    } \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_failures++; return; \
    } \
} while(0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_STR_EQ(a, b) do { \
    std::string _a = (a); std::string _b = (b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); \
        g_failures++; return; \
    } \
} while(0)

inline int run_tests() {
    int passed = 0, failed = 0;
    for (auto &tc : test_registry()) {
        int before = g_failures;
        tc.fn();
        if (g_failures == before) {
            passed++;
        } else {
            fprintf(stderr, "FAILED: %s\n", tc.name);
            failed++;
        }
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
