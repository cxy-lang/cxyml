//
// Cxyml plugin - Test runner
//
// Single compilation entry point.  Includes all test suites and runs them.
//
// Build:
//   clang -D_DARWIN_C_SOURCE -I $CXY_ROOT/include -L $CXY_ROOT/lib \
//         tests/plugin/main.c -lcxy-plugin -o run_tests
//
// Run:
//   ./run_tests
//
// NOTE: compile ONLY this file - test_lexer.c and test_parser.c are
//       pulled in via #include below and must NOT be separate TUs.
//

#define TEST_HARNESS_IMPL
#include "test_harness.h"

#include <cxy/core/mempool.h>

// Test suites - sources are provided by libcxy-plugin at link time
#include "test_lexer.c"
#include "test_parser.c"
#include "test_codegen.c"

// Forward declarations of suite entry points
void run_lexer_tests(Log *log);
void run_parser_tests(Log *log);
void run_codegen_tests(Log *log);

int main(void)
{
    printf("Cxyml Plugin Tests\n");
    printf("==================\n");

    // One pool for the whole test run - backs the Log and all parser tests
    MemPool pool = newMemPool();
    Log     log  = makeTestLog(&pool);

    run_lexer_tests(&log);
    run_parser_tests(&log);
    run_codegen_tests(&log);

    freeMemPool(&pool);

    TEST_SUMMARY();
}
