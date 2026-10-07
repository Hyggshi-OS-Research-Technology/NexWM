/* hde-svgpath.c — see hde-svgpath.h. Follows the SVG 1.1 path grammar ("paths" chapter, implementation notes F.6
 * for elliptical arcs). Quadratic curves and arcs are turned into cubic curves. */
#include "hde-svgpath.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    const char *p;
} Lexer;

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static void skip_sep(Lexer *lx)
{
    while (is_ws(*lx->p)) lx->p++;
    if (*lx->p == ',') {
        lx->p++;
        while (is_ws(*lx->p)) lx->p++;
    }
}

/* A number as SVG writes it: "-1.5", ".5", "1e-3", and "1.5.5" is 1.5 followed by .5 */
static int read_number(Lexer *lx, double *out)
{
    skip_sep(lx);
    const char *s = lx->p, *q = s;
    if (*q == '+' || *q == '-') q++;
    int digits = 0;
    while (*q >= '0' && *q <= '9') { q++; digits++; }
    if (*q == '.') {
        q++;
        while (*q >= '0' && *q <= '9') { q++; digits++; }
    }
    if (!digits) return 0;
    if (*q == 'e' || *q == 'E') {
        const char *e = q + 1;
        if (*e == '+' || *e == '-') e++;
        if (*e >= '0' && *e <= '9') {
            while (*e >= '0' && *e <= '9') e++;
            q = e;
        }
    }
    char buf[64];
    size_t n = (size_t)(q - s);
    if (n >= sizeof buf) return 0;
    memcpy(buf, s, n);
    buf[n] = '\0';
    *out = strtod(buf, NULL);
    lx->p = q;
    return 1;
}

/* arc flags are single characters and may be written without separators ("a1 1 0 011 1") */
static int read_flag(Lexer *lx, int *out)
{
    skip_sep(lx);
    if (*lx->p != '0' && *lx->p != '1') return 0;
    *out = *lx->p == '1';
    lx->p++;
    return 1;
}

static int next_is_number(Lexer *lx)
{
    Lexer t = *lx;
    skip_sep(&t);
    char c = *t.p;
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
}

/* SVG implementation notes F.6.5: endpoint to center parameterization, then cubic curves of at most 90 degrees */
static void arc_to(const HdeSvgPathSink *k, void *u, double x1, double y1, double rx, double ry, double phi_deg,
                   int large, int sweep, double x2, double y2)
{
    if (x1 == x2 && y1 == y2) return;
    rx = fabs(rx);
    ry = fabs(ry);
    if (rx == 0 || ry == 0) { k->line_to(u, x2, y2); return; }
    double phi = phi_deg * M_PI / 180.0, cp = cos(phi), sp = sin(phi);
    double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    double lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1) { double s = sqrt(lam); rx *= s; ry *= s; }
    double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double co = den == 0 ? 0 : sqrt(num / den > 0 ? num / den : 0);
    if (large == sweep) co = -co;
    double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    double cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    double th1 = atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double th2 = atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    double dth = th2 - th1;
    if (sweep && dth < 0) dth += 2 * M_PI;
    else if (!sweep && dth > 0) dth -= 2 * M_PI;
    int segs = (int)ceil(fabs(dth) / (M_PI / 2) - 1e-9);
    if (segs < 1) segs = 1;
    double step = dth / segs, t = 4.0 / 3.0 * tan(step / 4);
    double a = th1;
    for (int i = 0; i < segs; i++) {
        double b = a + step;
        double ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b);
        /* on the unit circle, then scaled by rx/ry, rotated by phi and moved to the centre */
        double px[3] = { ca - t * sa, cb + t * sb, cb }, py[3] = { sa + t * ca, sb - t * cb, sb };
        double X[3], Y[3];
        for (int j = 0; j < 3; j++) {
            double ex = px[j] * rx, ey = py[j] * ry;
            X[j] = cp * ex - sp * ey + cx;
            Y[j] = sp * ex + cp * ey + cy;
        }
        if (i == segs - 1) { X[2] = x2; Y[2] = y2; }     /* end exactly on the given point */
        k->curve_to(u, X[0], Y[0], X[1], Y[1], X[2], Y[2]);
        a = b;
    }
}

int hde_svg_path_parse(const char *d, const HdeSvgPathSink *k, void *u)
{
    if (!d || !k) return -1;
    Lexer lx = { d };
    double cx = 0, cy = 0, sx = 0, sy = 0;       /* current point, start of the subpath */
    double lcx = 0, lcy = 0;                      /* last control point (S, T) */
    char cmd = 0, last = 0;
    int count = 0, open = 0;
    for (;;) {
        skip_sep(&lx);
        if (!*lx.p) break;
        char c = *lx.p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            cmd = c;
            lx.p++;
        } else if (!cmd || !next_is_number(&lx)) {
            return -1;
        } else if (cmd == 'M') cmd = 'L';            /* numbers after a moveto are linetos */
        else if (cmd == 'm') cmd = 'l';
        else if (cmd == 'Z' || cmd == 'z') return -1;
        int rel = cmd >= 'a';
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        double v[7];
        switch (cmd) {
        case 'M': case 'm':
            if (!read_number(&lx, &v[0]) || !read_number(&lx, &v[1])) return -1;
            cx = sx = ox + v[0];
            cy = sy = oy + v[1];
            k->move_to(u, cx, cy);
            open = 1;
            break;
        case 'L': case 'l':
            if (!read_number(&lx, &v[0]) || !read_number(&lx, &v[1])) return -1;
            if (!open) { k->move_to(u, cx, cy); open = 1; }
            cx = ox + v[0];
            cy = oy + v[1];
            k->line_to(u, cx, cy);
            break;
        case 'H': case 'h':
            if (!read_number(&lx, &v[0])) return -1;
            if (!open) { k->move_to(u, cx, cy); open = 1; }
            cx = ox + v[0];
            k->line_to(u, cx, cy);
            break;
        case 'V': case 'v':
            if (!read_number(&lx, &v[0])) return -1;
            if (!open) { k->move_to(u, cx, cy); open = 1; }
            cy = oy + v[0];
            k->line_to(u, cx, cy);
            break;
        case 'C': case 'c':
            for (int i = 0; i < 6; i++) if (!read_number(&lx, &v[i])) return -1;
            if (!open) { k->move_to(u, cx, cy); open = 1; }
            k->curve_to(u, ox + v[0], oy + v[1], ox + v[2], oy + v[3], ox + v[4], oy + v[5]);
            lcx = ox + v[2]; lcy = oy + v[3];
            cx = ox + v[4]; cy = oy + v[5];
            break;
        case 'S': case 's': {
            for (int i = 0; i < 4; i++) if (!read_number(&lx, &v[i])) return -1;
            if (!open) { k->move_to(u, cx, cy); open = 1; }
            double x1 = cx, y1 = cy;
            if (last == 'C' || last == 'c' || last == 'S' || last == 's') { x1 = 2 * cx - lcx; y1 = 2 * cy - lcy; }
            k->curve_to(u, x1, y1, ox + v[0], oy + v[1], ox + v[2], oy + v[3]);
            lcx = ox + v[0]; lcy = oy + v[1];
            cx = ox + v[2]; cy = oy + v[3];
            break;
        }
        case 'Q': case 'q': {
            for (int i = 0; i < 4; i++) if (!read_number(&lx, &v[i])) return -1;
            if (!open) { k->move_to(u, cx, cy); open = 1; }
            double qx = ox + v[0], qy = oy + v[1], ex = ox + v[2], ey = oy + v[3];
            k->curve_to(u, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), ex + 2.0 / 3 * (qx - ex),
                        ey + 2.0 / 3 * (qy - ey), ex, ey);
            lcx = qx; lcy = qy;
            cx = ex; cy = ey;
            break;
        }
        case 'T': case 't': {
            for (int i = 0; i < 2; i++) if (!read_number(&lx, &v[i])) return -1;
            if (!open) { k->move_to(u, cx, cy); open = 1; }
            double qx = cx, qy = cy;
            if (last == 'Q' || last == 'q' || last == 'T' || last == 't') { qx = 2 * cx - lcx; qy = 2 * cy - lcy; }
            double ex = ox + v[0], ey = oy + v[1];
            k->curve_to(u, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), ex + 2.0 / 3 * (qx - ex),
                        ey + 2.0 / 3 * (qy - ey), ex, ey);
            lcx = qx; lcy = qy;
            cx = ex; cy = ey;
            break;
        }
        case 'A': case 'a': {
            int large, sweep;
            if (!read_number(&lx, &v[0]) || !read_number(&lx, &v[1]) || !read_number(&lx, &v[2]) ||
                !read_flag(&lx, &large) || !read_flag(&lx, &sweep) || !read_number(&lx, &v[3]) ||
                !read_number(&lx, &v[4]))
                return -1;
            if (!open) { k->move_to(u, cx, cy); open = 1; }
            double ex = ox + v[3], ey = oy + v[4];
            arc_to(k, u, cx, cy, v[0], v[1], v[2], large, sweep, ex, ey);
            cx = ex; cy = ey;
            break;
        }
        case 'Z': case 'z':
            k->close_path(u);
            cx = sx; cy = sy;
            open = 0;
            break;
        default:
            return -1;
        }
        last = cmd;
        count++;
    }
    return count;
}
