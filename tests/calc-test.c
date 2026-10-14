/* tests/calc-test.c — Unit tests for Hyggshi Calculator's pure-C math, programmer, and unit converter engine. */
#include "hde-calc-core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int passes, fails;

#define CHECK(cond, ...) do { \
    if (cond) { passes++; printf("PASS: calc: "); } \
    else { fails++; printf("FAIL: calc: "); } \
    printf(__VA_ARGS__); printf("\n"); \
} while (0)

static void check_expr(HdeCalcEnv *env, const char *expr, double expected, double tol)
{
    double got = 0.0;
    char err[128] = "";
    int rc = hde_calc_eval(expr, env, &got, err, sizeof err);
    CHECK(rc == 0 && fabs(got - expected) <= tol,
          "'%s' == %.6g (got %.6g, rc=%d, err='%s')", expr, expected, got, rc, err);
}

int main(void)
{
    HdeCalcEnv env;
    hde_calc_env_init(&env);

    /* Arithmetic & precedence */
    check_expr(&env, "2 + 3 * 4", 14.0, 1e-9);
    check_expr(&env, "(2 + 3) * 4", 20.0, 1e-9);
    check_expr(&env, "2 ^ 3 ^ 2", 512.0, 1e-9);
    check_expr(&env, "100 - 25%", 99.75, 1e-9);
    check_expr(&env, "17 mod 5", 2.0, 1e-9);
    check_expr(&env, "5!", 120.0, 1e-9);
    check_expr(&env, "3(4 + 5)", 27.0, 1e-9);
    check_expr(&env, "|-42.5|", 42.5, 1e-9);

    /* Scientific functions (Degrees) */
    check_expr(&env, "sin(30)", 0.5, 1e-9);
    check_expr(&env, "cos(60)", 0.5, 1e-9);
    check_expr(&env, "tan(45)", 1.0, 1e-9);
    check_expr(&env, "asin(0.5)", 30.0, 1e-9);
    check_expr(&env, "sqrt(144) + cbrt(27)", 15.0, 1e-9);
    check_expr(&env, "log(1000) + log2(8)", 6.0, 1e-9);
    check_expr(&env, "ln(e^3)", 3.0, 1e-9);

    /* Radians mode */
    env.deg_mode = 0;
    check_expr(&env, "sin(pi / 2)", 1.0, 1e-9);
    check_expr(&env, "cos(pi)", -1.0, 1e-9);
    env.deg_mode = 1;

    /* Programmer bitwise expressions */
    check_expr(&env, "0xFF & 0x0F", 15.0, 1e-9);
    check_expr(&env, "0b1010 | 0b0101", 15.0, 1e-9);
    check_expr(&env, "1 << 8", 256.0, 1e-9);
    check_expr(&env, "256 >> 4", 16.0, 1e-9);

    /* Base formatting */
    char buf[64];
    hde_calc_format_base(255, 16, 8, buf, sizeof buf);
    CHECK(!strcmp(buf, "FF"), "255 in 8-bit HEX is FF (got %s)", buf);
    hde_calc_format_base(255, 10, 8, buf, sizeof buf);
    CHECK(!strcmp(buf, "-1"), "255 in signed 8-bit DEC is -1 (got %s)", buf);
    hde_calc_format_base(10, 2, 8, buf, sizeof buf);
    CHECK(!strcmp(buf, "1010"), "10 in BIN is 1010 (got %s)", buf);

    /* Unit conversions */
    double f = hde_unit_convert(HDE_UNIT_TEMP, 0, 1, 100.0);
    CHECK(fabs(f - 212.0) < 1e-6, "100 °C is 212 °F (got %.2f)", f);
    double c = hde_unit_convert(HDE_UNIT_TEMP, 1, 0, 32.0);
    CHECK(fabs(c - 0.0) < 1e-6, "32 °F is 0 °C (got %.2f)", c);
    double m = hde_unit_convert(HDE_UNIT_LENGTH, 3, 2, 2.5);
    CHECK(fabs(m - 2500.0) < 1e-6, "2.5 km is 2500 m (got %.2f)", m);

    /* Error handling */
    double dummy = 0;
    char err[128] = "";
    CHECK(hde_calc_eval("1 / 0", &env, &dummy, err, sizeof err) != 0, "division by zero is rejected (%s)", err);
    CHECK(hde_calc_eval("sqrt(-4)", &env, &dummy, err, sizeof err) != 0, "sqrt(-4) is rejected (%s)", err);

    printf("\ncalc-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
