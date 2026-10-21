/* hde-calc-core.c — Pure-C expression evaluator, programmer bitwise engine, and unit converter. */
#define _POSIX_C_SOURCE 200809L

#include "hde-calc-core.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

void hde_calc_env_init(HdeCalcEnv *env)
{
    if (!env) return;
    env->deg_mode = 1;
    env->last_ans = 0.0;
    env->memory = 0.0;
}

typedef struct {
    const char *p;
    HdeCalcEnv *env;
    char *err;
    size_t err_n;
    int failed;
} Parser;

static void set_err(Parser *ps, const char *msg)
{
    if (ps->failed) return;
    ps->failed = 1;
    if (ps->err && ps->err_n > 0)
        snprintf(ps->err, ps->err_n, "%s", msg);
}

static void skip_ws(Parser *ps)
{
    while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++;
}

/* Match UTF-8 operator sequences */
static int match_utf8(Parser *ps, const char *seq)
{
    size_t len = strlen(seq);
    if (strncmp(ps->p, seq, len) == 0) {
        ps->p += len;
        return 1;
    }
    return 0;
}

static double parse_expr(Parser *ps);
static double parse_additive(Parser *ps);

static double to_rad(Parser *ps, double v)
{
    return (ps->env && ps->env->deg_mode) ? (v * M_PI / 180.0) : v;
}

static double from_rad(Parser *ps, double v)
{
    return (ps->env && ps->env->deg_mode) ? (v * 180.0 / M_PI) : v;
}

static double calc_factorial(Parser *ps, double x)
{
    if (x < 0.0 || fabs(x - floor(x)) > 1e-9) {
        set_err(ps, "Factorial requires a non-negative integer");
        return 0.0;
    }
    if (x > 170.0) {
        set_err(ps, "Overflow in factorial (n > 170)");
        return 0.0;
    }
    double r = 1.0;
    int n = (int)floor(x + 0.5);
    for (int i = 2; i <= n; i++) r *= (double)i;
    return r;
}

static double parse_primary(Parser *ps)
{
    skip_ws(ps);
    if (ps->failed) return 0.0;

    if (*ps->p == '(') {
        ps->p++;
        double v = parse_expr(ps);
        skip_ws(ps);
        if (*ps->p == ')') ps->p++;
        else set_err(ps, "Missing closing parenthesis ')'");
        return v;
    }

    if (*ps->p == '|') {
        ps->p++;
        double v = fabs(parse_additive(ps));
        skip_ws(ps);
        if (*ps->p == '|') ps->p++;
        else set_err(ps, "Missing closing '|'");
        return v;
    }

    /* Prefix square root / cube root symbols: √, ∛ */
    if (match_utf8(ps, "√")) {
        double v = parse_primary(ps);
        if (v < 0.0) { set_err(ps, "Square root of negative number"); return 0.0; }
        return sqrt(v);
    }
    if (match_utf8(ps, "∛")) {
        return cbrt(parse_primary(ps));
    }
    if (match_utf8(ps, "π")) {
        return M_PI;
    }

    /* Hex / Binary literals: 0x... or 0b... */
    if (ps->p[0] == '0' && (ps->p[1] == 'x' || ps->p[1] == 'X')) {
        char *end = NULL;
        unsigned long long hv = strtoull(ps->p + 2, &end, 16);
        if (end == ps->p + 2) { set_err(ps, "Invalid hexadecimal number"); return 0.0; }
        ps->p = end;
        return (double)hv;
    }
    if (ps->p[0] == '0' && (ps->p[1] == 'b' || ps->p[1] == 'B')) {
        ps->p += 2;
        if (*ps->p != '0' && *ps->p != '1') { set_err(ps, "Invalid binary number"); return 0.0; }
        unsigned long long bv = 0;
        while (*ps->p == '0' || *ps->p == '1') {
            bv = (bv << 1) | (unsigned long long)(*ps->p - '0');
            ps->p++;
        }
        return (double)bv;
    }

    /* Floating-point number */
    if (isdigit((unsigned char)*ps->p) || *ps->p == '.') {
        char *end = NULL;
        double v = strtod(ps->p, &end);
        if (end == ps->p) {
            set_err(ps, "Invalid number");
            return 0.0;
        }
        ps->p = end;
        return v;
    }

    /* Identifier (constant or function call) */
    if (isalpha((unsigned char)*ps->p)) {
        char id[32];
        size_t n = 0;
        while ((isalnum((unsigned char)*ps->p) || *ps->p == '_') && n + 1 < sizeof id) {
            id[n++] = (char)tolower((unsigned char)*ps->p++);
        }
        id[n] = '\0';
        skip_ws(ps);

        if (!strcmp(id, "pi")) return M_PI;
        if (!strcmp(id, "e") && *ps->p != '(') return M_E;
        if (!strcmp(id, "ans")) return ps->env ? ps->env->last_ans : 0.0;
        if (!strcmp(id, "mem") || !strcmp(id, "mr")) return ps->env ? ps->env->memory : 0.0;

        /* Function argument: either parenthesized or immediate unary primary */
        double arg = 0.0;
        if (*ps->p == '(') {
            ps->p++;
            arg = parse_expr(ps);
            skip_ws(ps);
            if (*ps->p == ')') ps->p++;
            else set_err(ps, "Missing ')' after function argument");
        } else {
            arg = parse_primary(ps);
        }
        if (ps->failed) return 0.0;

        if (!strcmp(id, "sin")) return sin(to_rad(ps, arg));
        if (!strcmp(id, "cos")) return cos(to_rad(ps, arg));
        if (!strcmp(id, "tan")) return tan(to_rad(ps, arg));
        if (!strcmp(id, "asin")) {
            if (arg < -1.0 || arg > 1.0) { set_err(ps, "asin domain error (|x| <= 1)"); return 0.0; }
            return from_rad(ps, asin(arg));
        }
        if (!strcmp(id, "acos")) {
            if (arg < -1.0 || arg > 1.0) { set_err(ps, "acos domain error (|x| <= 1)"); return 0.0; }
            return from_rad(ps, acos(arg));
        }
        if (!strcmp(id, "atan")) return from_rad(ps, atan(arg));
        if (!strcmp(id, "sinh")) return sinh(arg);
        if (!strcmp(id, "cosh")) return cosh(arg);
        if (!strcmp(id, "tanh")) return tanh(arg);
        if (!strcmp(id, "sqrt")) {
            if (arg < 0.0) { set_err(ps, "Square root of negative number"); return 0.0; }
            return sqrt(arg);
        }
        if (!strcmp(id, "cbrt")) return cbrt(arg);
        if (!strcmp(id, "abs")) return fabs(arg);
        if (!strcmp(id, "ln")) {
            if (arg <= 0.0) { set_err(ps, "ln domain error (x > 0)"); return 0.0; }
            return log(arg);
        }
        if (!strcmp(id, "log") || !strcmp(id, "log10")) {
            if (arg <= 0.0) { set_err(ps, "log10 domain error (x > 0)"); return 0.0; }
            return log10(arg);
        }
        if (!strcmp(id, "log2")) {
            if (arg <= 0.0) { set_err(ps, "log2 domain error (x > 0)"); return 0.0; }
            return log2(arg);
        }
        if (!strcmp(id, "exp")) return exp(arg);
        if (!strcmp(id, "floor")) return floor(arg);
        if (!strcmp(id, "ceil")) return ceil(arg);
        if (!strcmp(id, "round")) return round(arg);
        if (!strcmp(id, "fact")) return calc_factorial(ps, arg);

        set_err(ps, "Unknown function or identifier");
        return 0.0;
    }

    set_err(ps, "Unexpected token in expression");
    return 0.0;
}

static double parse_postfix(Parser *ps)
{
    double v = parse_primary(ps);
    while (!ps->failed) {
        skip_ws(ps);
        if (*ps->p == '!') {
            ps->p++;
            v = calc_factorial(ps, v);
        } else if (match_utf8(ps, "²")) {
            v = v * v;
        } else if (match_utf8(ps, "³")) {
            v = v * v * v;
        } else if (*ps->p == '%') {
            /* If followed by a number/identifier/(, treat % as modulo in parse_term;
             * otherwise treat as postfix percentage (/ 100.0). */
            const char *look = ps->p + 1;
            while (*look && isspace((unsigned char)*look)) look++;
            if (*look == '\0' || *look == ')' || *look == '+' || *look == '-' || *look == '*' || *look == '/') {
                ps->p++;
                v *= 0.01;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return v;
}

static double parse_power(Parser *ps)
{
    double base = parse_postfix(ps);
    skip_ws(ps);
    if (!ps->failed && (*ps->p == '^' || match_utf8(ps, "**"))) {
        if (*ps->p == '^') ps->p++;
        double exp_val = parse_power(ps); /* right-associative */
        return pow(base, exp_val);
    }
    return base;
}

static double parse_unary(Parser *ps)
{
    skip_ws(ps);
    if (*ps->p == '+') { ps->p++; return parse_unary(ps); }
    if (*ps->p == '-' || match_utf8(ps, "−")) {
        if (*ps->p == '-') ps->p++;
        return -parse_unary(ps);
    }
    if (*ps->p == '~') {
        ps->p++;
        int64_t iv = (int64_t)parse_unary(ps);
        return (double)(~iv);
    }
    return parse_power(ps);
}

static int starts_implicit_mul(const char *p)
{
    if (*p == '(') return 1;
    if (isalpha((unsigned char)*p)) {
        if (strncmp(p, "mod", 3) == 0 && !isalnum((unsigned char)p[3])) return 0;
        if (strncmp(p, "and", 3) == 0 && !isalnum((unsigned char)p[3])) return 0;
        if (strncmp(p, "xor", 3) == 0 && !isalnum((unsigned char)p[3])) return 0;
        if (strncmp(p, "or", 2) == 0 && !isalnum((unsigned char)p[2])) return 0;
        return 1;
    }
    if (strncmp(p, "π", strlen("π")) == 0 || strncmp(p, "√", strlen("√")) == 0)
        return 1;
    return 0;
}

static double parse_term(Parser *ps)
{
    double v = parse_unary(ps);
    while (!ps->failed) {
        skip_ws(ps);
        if (*ps->p == '*' || match_utf8(ps, "×") || match_utf8(ps, "·")) {
            if (*ps->p == '*') ps->p++;
            v *= parse_unary(ps);
        } else if (*ps->p == '/' || match_utf8(ps, "÷")) {
            if (*ps->p == '/') ps->p++;
            double rhs = parse_unary(ps);
            if (fabs(rhs) < 1e-18) { set_err(ps, "Division by zero"); return 0.0; }
            v /= rhs;
        } else if (*ps->p == '%') {
            ps->p++;
            double rhs = parse_unary(ps);
            if (fabs(rhs) < 1e-18) { set_err(ps, "Modulo by zero"); return 0.0; }
            v = fmod(v, rhs);
        } else if (strncmp(ps->p, "mod", 3) == 0 && !isalnum((unsigned char)ps->p[3])) {
            ps->p += 3;
            double rhs = parse_unary(ps);
            if (fabs(rhs) < 1e-18) { set_err(ps, "Modulo by zero"); return 0.0; }
            v = fmod(v, rhs);
        } else if (starts_implicit_mul(ps->p)) {
            v *= parse_unary(ps);
        } else {
            break;
        }
    }
    return v;
}

static double parse_additive(Parser *ps)
{
    double v = parse_term(ps);
    while (!ps->failed) {
        skip_ws(ps);
        if (*ps->p == '+') {
            ps->p++;
            v += parse_term(ps);
        } else if (*ps->p == '-' || match_utf8(ps, "−")) {
            if (*ps->p == '-') ps->p++;
            v -= parse_term(ps);
        } else {
            break;
        }
    }
    return v;
}

static double parse_bitwise(Parser *ps)
{
    double v = parse_additive(ps);
    while (!ps->failed) {
        skip_ws(ps);
        if (ps->p[0] == '<' && ps->p[1] == '<') {
            ps->p += 2;
            int sh = (int)parse_additive(ps);
            v = (double)((int64_t)v << (sh & 63));
        } else if (ps->p[0] == '>' && ps->p[1] == '>') {
            ps->p += 2;
            int sh = (int)parse_additive(ps);
            v = (double)((int64_t)v >> (sh & 63));
        } else if (*ps->p == '&' || (strncmp(ps->p, "and", 3) == 0 && !isalnum((unsigned char)ps->p[3]))) {
            ps->p += (*ps->p == '&') ? 1 : 3;
            v = (double)((int64_t)v & (int64_t)parse_additive(ps));
        } else if (strncmp(ps->p, "xor", 3) == 0 && !isalnum((unsigned char)ps->p[3])) {
            ps->p += 3;
            v = (double)((int64_t)v ^ (int64_t)parse_additive(ps));
        } else if (*ps->p == '|' || (strncmp(ps->p, "or", 2) == 0 && !isalnum((unsigned char)ps->p[2]))) {
            ps->p += (*ps->p == '|') ? 1 : 2;
            v = (double)((int64_t)v | (int64_t)parse_additive(ps));
        } else {
            break;
        }
    }
    return v;
}

static double parse_expr(Parser *ps)
{
    return parse_bitwise(ps);
}

int hde_calc_eval(const char *expr, HdeCalcEnv *env, double *out_val, char *err, size_t err_n)
{
    if (err && err_n > 0) err[0] = '\0';
    if (!expr) {
        if (err && err_n > 0) snprintf(err, err_n, "Empty expression");
        return -1;
    }
    Parser ps = { expr, env, err, err_n, 0 };
    skip_ws(&ps);
    if (!*ps.p) {
        if (out_val) *out_val = 0.0;
        return 0;
    }
    double val = parse_expr(&ps);
    skip_ws(&ps);
    if (!ps.failed && *ps.p != '\0') {
        set_err(&ps, "Unexpected trailing characters");
    }
    if (ps.failed) return -1;
    if (isnan(val) || isinf(val)) {
        if (err && err_n > 0) snprintf(err, err_n, isinf(val) ? "Numeric overflow (Infinity)" : "Undefined result (NaN)");
        return -1;
    }
    if (fabs(val) < 1e-15) val = 0.0;
    if (env) env->last_ans = val;
    if (out_val) *out_val = val;
    return 0;
}

void hde_calc_format(double val, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    if (isnan(val)) { snprintf(buf, n, "NaN"); return; }
    if (isinf(val)) { snprintf(buf, n, val < 0 ? "-Infinity" : "Infinity"); return; }
    if (fabs(val) < 1e-15) val = 0.0;
    if (fabs(val) >= 1e15 || (val != 0.0 && fabs(val) < 1e-6)) {
        snprintf(buf, n, "%.10g", val);
        return;
    }
    snprintf(buf, n, "%.12f", val);
    /* Strip trailing zeros after decimal point */
    char *dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *dot = '\0';
    }
}

uint64_t hde_calc_mask_bits(uint64_t v, int bit_width)
{
    if (bit_width == 8) return v & 0xFFULL;
    if (bit_width == 16) return v & 0xFFFFULL;
    if (bit_width == 32) return v & 0xFFFFFFFFULL;
    return v;
}

int64_t hde_calc_sign_extend(uint64_t v, int bit_width)
{
    v = hde_calc_mask_bits(v, bit_width);
    if (bit_width == 8) return (int8_t)v;
    if (bit_width == 16) return (int16_t)v;
    if (bit_width == 32) return (int32_t)v;
    return (int64_t)v;
}

void hde_calc_format_base(uint64_t v, int base, int bit_width, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    v = hde_calc_mask_bits(v, bit_width);
    if (base == 16) {
        snprintf(buf, n, "%llX", (unsigned long long)v);
    } else if (base == 8) {
        snprintf(buf, n, "%llo", (unsigned long long)v);
    } else if (base == 2) {
        if (v == 0) { snprintf(buf, n, "0000"); return; }
        int top = bit_width - 1;
        while (top > 3 && ((v >> top) & 1ULL) == 0) top--;
        top = ((top / 4) + 1) * 4 - 1;
        size_t pos = 0;
        for (int b = top; b >= 0 && pos + 2 < n; b--) {
            buf[pos++] = ((v >> b) & 1ULL) ? '1' : '0';
            if (b > 0 && (b % 4) == 0 && pos + 2 < n) buf[pos++] = ' ';
        }
        buf[pos] = '\0';
    } else {
        int64_t sv = hde_calc_sign_extend(v, bit_width);
        snprintf(buf, n, "%lld", (long long)sv);
    }
}

/* ---------------------------------------------------------------- Unit Converter */
static const HdeUnitDef units_length[] = {
    { "Millimeter", "mm", 0.001 },
    { "Centimeter", "cm", 0.01 },
    { "Meter", "m", 1.0 },
    { "Kilometer", "km", 1000.0 },
    { "Inch", "in", 0.0254 },
    { "Foot", "ft", 0.3048 },
    { "Yard", "yd", 0.9144 },
    { "Mile", "mi", 1609.344 }
};

static const HdeUnitDef units_mass[] = {
    { "Milligram", "mg", 0.001 },
    { "Gram", "g", 1.0 },
    { "Kilogram", "kg", 1000.0 },
    { "Metric Ton", "t", 1000000.0 },
    { "Ounce", "oz", 28.349523125 },
    { "Pound", "lb", 453.59237 }
};

static const HdeUnitDef units_temp[] = {
    { "Celsius", "°C", 1.0 },
    { "Fahrenheit", "°F", 1.0 },
    { "Kelvin", "K", 1.0 }
};

static const HdeUnitDef units_data[] = {
    { "Byte", "B", 1.0 },
    { "Kilobyte (1024 B)", "KiB", 1024.0 },
    { "Megabyte (1024 KiB)", "MiB", 1048576.0 },
    { "Gigabyte (1024 MiB)", "GiB", 1073741824.0 },
    { "Terabyte (1024 GiB)", "TiB", 1099511627776.0 }
};

static const HdeUnitDef units_speed[] = {
    { "Meter / second", "m/s", 1.0 },
    { "Kilometer / hour", "km/h", 1.0 / 3.6 },
    { "Miles / hour", "mph", 0.44704 },
    { "Knot", "kn", 0.514444 }
};

static const HdeUnitDef units_area[] = {
    { "Square Centimeter", "cm²", 0.0001 },
    { "Square Meter", "m²", 1.0 },
    { "Hectare", "ha", 10000.0 },
    { "Square Kilometer", "km²", 1000000.0 },
    { "Square Foot", "ft²", 0.09290304 },
    { "Acre", "ac", 4046.8564224 }
};

const char *hde_unit_category_name(HdeUnitCategory cat)
{
    switch (cat) {
    case HDE_UNIT_LENGTH: return "Length";
    case HDE_UNIT_MASS:   return "Weight & Mass";
    case HDE_UNIT_TEMP:   return "Temperature";
    case HDE_UNIT_DATA:   return "Digital Storage";
    case HDE_UNIT_SPEED:  return "Speed";
    case HDE_UNIT_AREA:   return "Area";
    default:              return "Unknown";
    }
}

const HdeUnitDef *hde_unit_list(HdeUnitCategory cat, int *count)
{
    switch (cat) {
    case HDE_UNIT_LENGTH: if (count) *count = (int)(sizeof units_length / sizeof units_length[0]); return units_length;
    case HDE_UNIT_MASS:   if (count) *count = (int)(sizeof units_mass / sizeof units_mass[0]); return units_mass;
    case HDE_UNIT_TEMP:   if (count) *count = (int)(sizeof units_temp / sizeof units_temp[0]); return units_temp;
    case HDE_UNIT_DATA:   if (count) *count = (int)(sizeof units_data / sizeof units_data[0]); return units_data;
    case HDE_UNIT_SPEED:  if (count) *count = (int)(sizeof units_speed / sizeof units_speed[0]); return units_speed;
    case HDE_UNIT_AREA:   if (count) *count = (int)(sizeof units_area / sizeof units_area[0]); return units_area;
    default:              if (count) *count = 0; return NULL;
    }
}

double hde_unit_convert(HdeUnitCategory cat, int from_idx, int to_idx, double val)
{
    int count = 0;
    const HdeUnitDef *list = hde_unit_list(cat, &count);
    if (!list || from_idx < 0 || from_idx >= count || to_idx < 0 || to_idx >= count) return val;
    if (from_idx == to_idx) return val;

    if (cat == HDE_UNIT_TEMP) {
        double c = val;
        if (from_idx == 1) c = (val - 32.0) * (5.0 / 9.0);
        else if (from_idx == 2) c = val - 273.15;

        if (to_idx == 0) return c;
        if (to_idx == 1) return c * (9.0 / 5.0) + 32.0;
        return c + 273.15;
    }
    double base = val * list[from_idx].factor_to_base;
    return base / list[to_idx].factor_to_base;
}
