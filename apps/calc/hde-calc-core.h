/* hde-calc-core.h — Pure-C expression evaluator, programmer bitwise engine, and unit converter for Hyggshi Calculator. */
#ifndef HDE_CALC_CORE_H
#define HDE_CALC_CORE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int deg_mode;       /* 1 = Degrees (default), 0 = Radians */
    double last_ans;    /* Ans register */
    double memory;      /* M register */
} HdeCalcEnv;

void hde_calc_env_init(HdeCalcEnv *env);

/* Evaluate a mathematical expression string.
 * Returns 0 on success and writes the result to *out_val;
 * returns -1 on syntax/domain error and writes a short English message to err (up to err_n bytes). */
int hde_calc_eval(const char *expr, HdeCalcEnv *env, double *out_val, char *err, size_t err_n);

/* Format a double result cleanly (e.g. "42", "3.14159265359", "-1.2e+18"). */
void hde_calc_format(double val, char *buf, size_t n);

/* Format a 64-bit integer in base 16, 10 (signed), 8, or 2 (grouped by 4 bits), masked to bit_width (8, 16, 32, 64). */
uint64_t hde_calc_mask_bits(uint64_t v, int bit_width);
int64_t  hde_calc_sign_extend(uint64_t v, int bit_width);
void     hde_calc_format_base(uint64_t v, int base, int bit_width, char *buf, size_t n);

/* Unit converter categories and conversion helper */
typedef enum {
    HDE_UNIT_LENGTH = 0,
    HDE_UNIT_MASS,
    HDE_UNIT_TEMP,
    HDE_UNIT_DATA,
    HDE_UNIT_SPEED,
    HDE_UNIT_AREA,
    HDE_UNIT_CAT_COUNT
} HdeUnitCategory;

typedef struct {
    const char *name;
    const char *symbol;
    double factor_to_base; /* for linear units: 1 unit = factor_to_base base units */
} HdeUnitDef;

const char       *hde_unit_category_name(HdeUnitCategory cat);
const HdeUnitDef *hde_unit_list(HdeUnitCategory cat, int *count);
double            hde_unit_convert(HdeUnitCategory cat, int from_idx, int to_idx, double val);

#endif /* HDE_CALC_CORE_H */
