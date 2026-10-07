/* tests/svgpath-test.c — the SVG path reader of the logos (src/hde-svgpath.c), without GTK:
 *   svgpath-test                 unit checks (PASS:/FAIL: lines, exit code = failures)
 *   svgpath-test --logos DIR     also: every data/logos/NAME.svg parses and stays inside its 24x24 box
 *   svgpath-test --flat FILE     print the path of FILE as absolute M/L/C/Z (to compare drawings by hand) */
#include "hde-svgpath.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

typedef struct {
    double minx, miny, maxx, maxy, lx, ly;
    int moves, lines, curves, closes;
    FILE *out;
} Acc;

static void grow(Acc *a, double x, double y)
{
    if (x < a->minx) a->minx = x;
    if (y < a->miny) a->miny = y;
    if (x > a->maxx) a->maxx = x;
    if (y > a->maxy) a->maxy = y;
    a->lx = x;
    a->ly = y;
}
static void s_move(void *u, double x, double y) { Acc *a = u; a->moves++; grow(a, x, y); if (a->out) fprintf(a->out, "M%.4f %.4f", x, y); }
static void s_line(void *u, double x, double y) { Acc *a = u; a->lines++; grow(a, x, y); if (a->out) fprintf(a->out, "L%.4f %.4f", x, y); }
static void s_curve(void *u, double x1, double y1, double x2, double y2, double x3, double y3)
{
    Acc *a = u;
    a->curves++;
    grow(a, x3, y3);                 /* control points may lie outside; the curve itself rarely does much */
    if (a->out) fprintf(a->out, "C%.4f %.4f %.4f %.4f %.4f %.4f", x1, y1, x2, y2, x3, y3);
}
static void s_close(void *u) { Acc *a = u; a->closes++; if (a->out) fprintf(a->out, "Z"); }
static const HdeSvgPathSink sink = { s_move, s_line, s_curve, s_close };

static int run(const char *d, Acc *a, FILE *out)
{
    memset(a, 0, sizeof *a);
    a->minx = a->miny = 1e9;
    a->maxx = a->maxy = -1e9;
    a->out = out;
    return hde_svg_path_parse(d, &sink, a);
}

static char *path_of(const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f) return NULL;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *p = strstr(buf, " d=\"");
    if (!p) return NULL;
    p += 4;
    char *e = strchr(p, '"');
    if (!e) return NULL;
    *e = '\0';
    return p;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--flat")) {
        char *d = path_of(argv[2]);
        Acc a;
        int n = d ? run(d, &a, stdout) : -1;
        printf("\n");
        return n < 0;
    }
    Acc a;
    check(run("M1 2L3 4Z", &a, NULL) == 3 && a.moves == 1 && a.lines == 1 && a.closes == 1, "M L Z");
    check(run("m1 2 3 4 5 6", &a, NULL) == 3 && a.lines == 2 && fabs(a.lx - 9) < 1e-9 && fabs(a.ly - 12) < 1e-9,
          "numbers after a relative moveto are relative linetos");
    check(run("M0 0h5v5h-5z", &a, NULL) == 5 && a.maxx == 5 && a.maxy == 5, "H V and relative h v");
    check(run("M3.41.455l.5-.5", &a, NULL) == 2 && fabs(a.lx - 3.91) < 1e-9 && fabs(a.ly + 0.045) < 1e-9,
          "compact numbers: 3.41.455 is 3.41 then .455, .5-.5 is .5 then -.5");
    check(run("M0 0L1e1 2E-1", &a, NULL) == 2 && fabs(a.lx - 10) < 1e-9 && fabs(a.ly - 0.2) < 1e-9, "exponents");
    check(run("M10 0A10 10 0 0 1 -10 0", &a, NULL) == 2 && a.curves == 2 && fabs(a.lx + 10) < 1e-9 && fabs(a.ly) < 1e-9,
          "half circle arc: two curves, ends on the given point");
    check(run("M10 0a10 10 0 1110 10", &a, NULL) == 2 && a.curves == 3 && fabs(a.lx - 20) < 1e-9 && fabs(a.ly - 10) < 1e-9,
          "arc flags written without separators (a10 10 0 1110 10), large arc: three curves");
    check(run("M0 0A0 5 0 0 1 5 5", &a, NULL) == 2 && a.lines == 1, "arc with a zero radius is a line");
    check(run("M0 0C1 1 2 1 3 0S5 -1 6 0", &a, NULL) == 3 && a.curves == 2 && a.lx == 6, "C then S (reflected control point)");
    check(run("M0 0Q1 1 2 0T4 0", &a, NULL) == 3 && a.curves == 2 && a.lx == 4, "Q then T");
    check(run("M0 0L1 1Zl1 0", &a, NULL) == 4 && a.moves == 2 && fabs(a.lx - 1) < 1e-9 && fabs(a.ly) < 1e-9,
          "after Z the next subpath starts where the closed one started");
    check(run("M0 0L1 x", &a, NULL) < 0, "invalid data is rejected");
    check(run("1 2", &a, NULL) < 0, "data must start with a command");

    if (argc == 3 && !strcmp(argv[1], "--logos")) {
        DIR *dp = opendir(argv[2]);
        int nlogos = 0;
        struct dirent *e;
        while (dp && (e = readdir(dp))) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".svg")) continue;
            char file[4096], what[512];
            snprintf(file, sizeof file, "%s/%s", argv[2], e->d_name);
            char *d = path_of(file);
            int n = d ? run(d, &a, NULL) : -1;
            snprintf(what, sizeof what, "logo %s: %d commands, inside 0..24 (%.1f,%.1f)-(%.1f,%.1f)", e->d_name, n,
                     a.minx, a.miny, a.maxx, a.maxy);
            check(n > 2 && a.minx > -0.6 && a.miny > -0.6 && a.maxx < 24.6 && a.maxy < 24.6, what);
            nlogos++;
        }
        if (dp) closedir(dp);
        check(nlogos >= 20, "data/logos has the logos of at least 20 distributions");
    }
    return fails;
}
