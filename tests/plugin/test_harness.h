//
// Cxyml plugin - Test Harness
//
// Minimal self-contained test harness.  No external dependencies.
//
// Usage
// -----
// 1. Include this header in every test file.
// 2. Define test functions with signature:  void test_something(void)
// 3. Use ASSERT_* macros to check conditions.
// 4. In main.c call SUITE("name") then RUN_TEST(fn) for each test.
//
// Build
// -----
// Compile ONLY main.c - it includes test_lexer.c and test_parser.c
// via #include so they must NOT be passed as separate compilation units.
//
//   clang -D_DARWIN_C_SOURCE -I $CXY_ROOT/include -L $CXY_ROOT/lib \
//         tests/plugin/main.c -lcxy-plugin -o run_tests
//

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// ============================================================================
// Global counters  (defined once in main.c via TEST_HARNESS_IMPL)
// ============================================================================

extern int g_passed;
extern int g_failed;
extern int g_errors;   // diagnostic errors captured by the test handler

#ifdef TEST_HARNESS_IMPL
int g_passed  = 0;
int g_failed  = 0;
int g_errors  = 0;
#endif

// ============================================================================
// Log setup
// Uses the real CXY Log with a custom DiagnosticHandler that records errors
// into g_errors.  No stubs needed - avoids symbol conflicts with libcxy-plugin.
// ============================================================================

#include <cxy/core/log.h>
#include <cxy/core/mempool.h>

#ifdef TEST_HARNESS_IMPL

static void testDiagnosticHandler(const Diagnostic *diag, void *ctx)
{
    (void)ctx;
    if (diag->kind == dkError) {
        g_errors++;
        fprintf(stderr, "    [error] %s\n", diag->fmt);
    }
}

// Call this once in main() before any tests run.
// Returns an initialised Log backed by a pool the caller owns.
static Log makeTestLog(MemPool *pool)
{
    return newLog(pool, testDiagnosticHandler, NULL);
}

#endif // TEST_HARNESS_IMPL

// ============================================================================
// Helpers
// ============================================================================

// Reset the error counter before a test that expects no errors
#define RESET_ERRORS() do { g_errors = 0; } while(0)

// ============================================================================
// Assert macros
// ============================================================================

#define _FAIL(msg)                                                             \
    do {                                                                       \
        fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, (msg));    \
        g_failed++;                                                            \
        return;                                                                \
    } while(0)

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) _FAIL("ASSERT(" #cond ")");                               \
    } while(0)

#define ASSERT_MSG(cond, msg)                                                  \
    do {                                                                       \
        if (!(cond)) _FAIL(msg);                                               \
    } while(0)

#define ASSERT_TRUE(cond)   ASSERT((cond))
#define ASSERT_FALSE(cond)  ASSERT(!(cond))
#define ASSERT_NULL(ptr)    ASSERT_MSG((ptr) == NULL,  #ptr " should be NULL")
#define ASSERT_NOT_NULL(ptr) ASSERT_MSG((ptr) != NULL, #ptr " should not be NULL")

#define ASSERT_EQ(a, b)                                                        \
    do {                                                                       \
        if ((a) != (b)) {                                                      \
            fprintf(stderr, "  FAIL  %s:%d  %s == %s\n",                      \
                    __FILE__, __LINE__, #a, #b);                               \
            g_failed++;                                                        \
            return;                                                            \
        }                                                                      \
    } while(0)

#define ASSERT_STR_EQ(a, b)                                                    \
    do {                                                                       \
        if (strcmp((a), (b)) != 0) {                                           \
            fprintf(stderr, "  FAIL  %s:%d  expected \"%s\"  got \"%s\"\n",   \
                    __FILE__, __LINE__, (b), (a));                             \
            g_failed++;                                                        \
            return;                                                            \
        }                                                                      \
    } while(0)

#define ASSERT_STRN_EQ(a, alen, b)                                             \
    do {                                                                       \
        if ((alen) != strlen(b) || strncmp((a), (b), (alen)) != 0) {          \
            fprintf(stderr, "  FAIL  %s:%d  expected \"%s\"  got \"%.*s\"\n", \
                    __FILE__, __LINE__, (b), (int)(alen), (a));                \
            g_failed++;                                                        \
            return;                                                            \
        }                                                                      \
    } while(0)

#define ASSERT_NO_ERRORS()                                                     \
    ASSERT_MSG(g_errors == 0, "unexpected logError() call(s)")

#define ASSERT_HAS_ERRORS()                                                    \
    ASSERT_MSG(g_errors > 0, "expected logError() to be called")

// ============================================================================
// Test runner
// ============================================================================

#define SUITE(name)                                                            \
    do { printf("\n%s\n", (name)); } while(0)

#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        int _f_before = g_failed;                                              \
        RESET_ERRORS();                                                        \
        printf("  %-50s", #fn);                                                \
        fn();                                                                  \
        if (g_failed == _f_before) {                                           \
            g_passed++;                                                        \
            printf("ok\n");                                                    \
        }                                                                      \
    } while(0)

// ============================================================================
// Summary + exit
// ============================================================================

#define TEST_SUMMARY()                                                         \
    do {                                                                       \
        printf("\n----------------------------------------\n");                \
        printf("  passed: %d\n", g_passed);                                    \
        printf("  failed: %d\n", g_failed);                                    \
        printf("----------------------------------------\n");                  \
        return g_failed > 0 ? 1 : 0;                                          \
    } while(0)