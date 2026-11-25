/* test_util.h - mini framework de pruebas (sin dependencias externas). */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int t_checks, t_failed, t_tests, t_tests_failed;
static int t_cur_failed;

#define CHECK(cond) do { t_checks++; if (!(cond)) { t_failed++; t_cur_failed++; \
    printf("    FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_NEAR(a, b, tol) do { t_checks++; double _a = (a), _b = (b); \
    if (fabs(_a - _b) > (tol)) { t_failed++; t_cur_failed++; \
    printf("    FALLO %s:%d: %s=%.6f esperado %.6f\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

#define RUN(fn) do { t_cur_failed = 0; t_tests++; fn(); \
    if (t_cur_failed) t_tests_failed++; \
    printf("  [%s] %s\n", t_cur_failed ? "FALLA" : " OK  ", #fn); } while (0)

#define TEST_SUMMARY() do { \
    printf("\n%d pruebas, %d fallidas, %d verificaciones, %d fallos\n", \
           t_tests, t_tests_failed, t_checks, t_failed); \
    return t_failed ? 1 : 0; } while (0)

#endif /* TEST_UTIL_H */
