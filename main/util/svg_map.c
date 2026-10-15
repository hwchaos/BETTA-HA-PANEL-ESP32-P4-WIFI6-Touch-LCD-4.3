/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#include "util/svg_map.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#define SVGM_STACK 16
#define SVGM_CURVE_STEPS 8
#define SVGM_MAP_MARGIN 4
#define SVGM_MAX_ZOOM 8.0f
#define SVGM_CELL 4

/* x' = a*x + c*y + e, y' = b*x + d*y + f */
typedef struct {
    float a, b, c, d, e, f;
} mat_t;

typedef struct {
    uint32_t rgb;
    bool none;
} paint_t;

typedef struct {
    mat_t m;
    paint_t fill;
    paint_t stroke;
    float stroke_w;
    float opacity;
    float fill_op;
    float stroke_op;
} style_t;

typedef struct {
    float x, y;
} pt_t;

typedef struct {
    int start; /* first point */
    bool closed;
} sub_t;

typedef struct {
    uint8_t *buf;
    int w, h;
    float s, ox, oy, vbx, vby; /* device = o + s * (user - vb) */
    uint8_t min_value;
    /* current path, device coordinates */
    pt_t *pts;
    int npts, cap;
    sub_t *sub;
    int nsub, subcap;
    /* scan line crossings */
    float *xs;
    int xcap;
    int8_t *dirs;
    int dcap;
} rc_t;

static const mat_t MAT_ID = {1, 0, 0, 1, 0, 0};

/* ------------------------------------------------------------ helpers */

static bool svgm_grow(void **p, int *cap, int need, size_t elem)
{
    if (need <= *cap) {
        return true;
    }
    int n = *cap > 0 ? *cap : 256;
    while (n < need) {
        n *= 2;
    }
    void *q = heap_caps_realloc(*p, (size_t)n * elem, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (q == NULL) {
        return false;
    }
    *p = q;
    *cap = n;
    return true;
}

static mat_t mat_mul(mat_t p, mat_t l) /* p * l: l applied first */
{
    mat_t r;
    r.a = p.a * l.a + p.c * l.b;
    r.b = p.b * l.a + p.d * l.b;
    r.c = p.a * l.c + p.c * l.d;
    r.d = p.b * l.c + p.d * l.d;
    r.e = p.a * l.e + p.c * l.f + p.e;
    r.f = p.b * l.e + p.d * l.f + p.f;
    return r;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') {
        p++;
    }
    return p;
}

static bool parse_num(const char **pp, float *out)
{
    const char *p = skip_ws(*pp);
    char *end = NULL;
    const float v = strtof(p, &end);
    if (end == p) {
        return false;
    }
    *out = v;
    *pp = end;
    return true;
}

/* Value of attribute `name` within [tag, end): copied to out. */
static bool attr(const char *tag, const char *end, const char *name, char *out, size_t n)
{
    const size_t len = strlen(name);
    for (const char *p = tag; p < end; p++) {
        p = strstr(p, name);
        if (p == NULL || p >= end) {
            return false;
        }
        if (!isspace((unsigned char)p[-1]) || p[len] != '=' || (p[len + 1] != '"' && p[len + 1] != '\'')) {
            continue;
        }
        const char q = p[len + 1];
        const char *v = p + len + 2;
        const char *ve = memchr(v, q, (size_t)(end - v));
        if (ve == NULL) {
            return false;
        }
        size_t vl = (size_t)(ve - v);
        if (vl >= n) {
            vl = n - 1;
        }
        memcpy(out, v, vl);
        out[vl] = '\0';
        return true;
    }
    return false;
}

/* Same for a large value (path data, points): pointer into the text. */
static const char *attr_ref(const char *tag, const char *end, const char *name, const char **vend)
{
    const size_t len = strlen(name);
    for (const char *p = tag; p < end; p++) {
        p = strstr(p, name);
        if (p == NULL || p >= end) {
            return NULL;
        }
        if (!isspace((unsigned char)p[-1]) || p[len] != '=' || (p[len + 1] != '"' && p[len + 1] != '\'')) {
            continue;
        }
        const char q = p[len + 1];
        const char *v = p + len + 2;
        const char *ve = memchr(v, q, (size_t)(end - v));
        if (ve == NULL) {
            return NULL;
        }
        *vend = ve;
        return v;
    }
    return NULL;
}

/* ------------------------------------------------------------ style */

static bool parse_color(const char *v, paint_t *out, float *alpha)
{
    while (isspace((unsigned char)*v)) {
        v++;
    }
    out->none = false;
    if (strncmp(v, "none", 4) == 0 || strncmp(v, "transparent", 11) == 0 || strncmp(v, "url(", 4) == 0) {
        out->none = true;
        return true;
    }
    if (*v == '#') {
        char hex[9] = {0};
        size_t n = 0;
        while (n < 8 && isxdigit((unsigned char)v[1 + n])) {
            hex[n] = v[1 + n];
            n++;
        }
        const unsigned long x = strtoul(hex, NULL, 16);
        if (n == 3) {
            const uint32_t r = (x >> 8) & 0xF, g = (x >> 4) & 0xF, b = x & 0xF;
            out->rgb = (r * 17) << 16 | (g * 17) << 8 | (b * 17);
        } else if (n == 6) {
            out->rgb = (uint32_t)x;
        } else if (n == 8) {
            out->rgb = (uint32_t)(x >> 8);
            *alpha *= (float)(x & 0xFF) / 255.0f;
        } else {
            return false;
        }
        return true;
    }
    if (strncmp(v, "rgb", 3) == 0) {
        const char *p = strchr(v, '(');
        float c[3] = {0};
        for (int i = 0; p != NULL && i < 3; i++) {
            p++;
            if (!parse_num(&p, &c[i])) {
                return false;
            }
            if (*p == '%') {
                c[i] *= 2.55f;
                p++;
            }
        }
        out->rgb = ((uint32_t)fminf(c[0], 255) << 16) | ((uint32_t)fminf(c[1], 255) << 8) | (uint32_t)fminf(c[2], 255);
        return true;
    }
    static const struct {
        const char *name;
        uint32_t rgb;
    } names[] = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000}, {"lime", 0x00FF00},
        {"blue", 0x0000FF}, {"yellow", 0xFFFF00}, {"orange", 0xFFA500}, {"gray", 0x808080}, {"grey", 0x808080},
        {"silver", 0xC0C0C0}, {"darkgreen", 0x006400}, {"currentColor", 0x000000},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strncmp(v, names[i].name, strlen(names[i].name)) == 0) {
            out->rgb = names[i].rgb;
            return true;
        }
    }
    return false;
}

static void parse_transform(const char *v, mat_t *m)
{
    const char *p = v;
    while (*p) {
        p = skip_ws(p);
        const char *name = p;
        while (isalpha((unsigned char)*p)) {
            p++;
        }
        const size_t nl = (size_t)(p - name);
        p = skip_ws(p);
        if (*p != '(') {
            return;
        }
        p++;
        float a[6] = {0};
        int n = 0;
        while (n < 6 && parse_num(&p, &a[n])) {
            n++;
        }
        p = strchr(p, ')');
        if (p == NULL) {
            return;
        }
        p++;
        mat_t l = MAT_ID;
        if (nl == 9 && strncmp(name, "translate", 9) == 0) {
            l.e = a[0];
            l.f = n > 1 ? a[1] : 0;
        } else if (nl == 5 && strncmp(name, "scale", 5) == 0) {
            l.a = a[0];
            l.d = n > 1 ? a[1] : a[0];
        } else if (nl == 6 && strncmp(name, "rotate", 6) == 0) {
            const float r = a[0] * (float)M_PI / 180.0f;
            const float cs = cosf(r), sn = sinf(r);
            l.a = cs;
            l.b = sn;
            l.c = -sn;
            l.d = cs;
            if (n >= 3) { /* about (cx, cy) */
                l.e = a[1] - cs * a[1] + sn * a[2];
                l.f = a[2] - sn * a[1] - cs * a[2];
            }
        } else if (nl == 6 && strncmp(name, "matrix", 6) == 0 && n == 6) {
            l = (mat_t){a[0], a[1], a[2], a[3], a[4], a[5]};
        } else if (nl == 5 && strncmp(name, "skewX", 5) == 0) {
            l.c = tanf(a[0] * (float)M_PI / 180.0f);
        } else if (nl == 5 && strncmp(name, "skewY", 5) == 0) {
            l.b = tanf(a[0] * (float)M_PI / 180.0f);
        }
        *m = mat_mul(*m, l);
    }
}

static void apply_prop(style_t *st, const char *name, size_t nl, const char *v)
{
#define IS(s) (nl == strlen(s) && strncmp(name, s, nl) == 0)
    float tmp = 1.0f;
    if (IS("fill")) {
        float a = 1.0f;
        if (parse_color(v, &st->fill, &a)) {
            st->fill_op = a;
        }
    } else if (IS("stroke")) {
        float a = 1.0f;
        if (parse_color(v, &st->stroke, &a)) {
            st->stroke_op = a;
        }
    } else if (IS("stroke-width")) {
        if (parse_num(&v, &tmp)) {
            st->stroke_w = tmp;
        }
    } else if (IS("opacity")) {
        if (parse_num(&v, &tmp)) {
            st->opacity *= tmp;
        }
    } else if (IS("fill-opacity")) {
        if (parse_num(&v, &tmp)) {
            st->fill_op *= tmp;
        }
    } else if (IS("stroke-opacity")) {
        if (parse_num(&v, &tmp)) {
            st->stroke_op *= tmp;
        }
    }
#undef IS
}

static void apply_style(style_t *st, const char *tag, const char *end)
{
    static const char *const props[] = {"fill", "stroke", "stroke-width", "opacity", "fill-opacity", "stroke-opacity"};
    char v[64];
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        if (attr(tag, end, props[i], v, sizeof(v))) {
            apply_prop(st, props[i], strlen(props[i]), v);
        }
    }
    char css[256];
    if (attr(tag, end, "style", css, sizeof(css))) {
        char *p = css;
        while (*p) {
            char *colon = strchr(p, ':');
            if (colon == NULL) {
                break;
            }
            char *semi = strchr(colon, ';');
            if (semi != NULL) {
                *semi = '\0';
            }
            while (isspace((unsigned char)*p)) {
                p++;
            }
            size_t nl = (size_t)(colon - p);
            while (nl > 0 && isspace((unsigned char)p[nl - 1])) {
                nl--;
            }
            apply_prop(st, p, nl, colon + 1);
            if (semi == NULL) {
                break;
            }
            p = semi + 1;
        }
    }
    char tr[160];
    if (attr(tag, end, "transform", tr, sizeof(tr))) {
        parse_transform(tr, &st->m);
    }
}

/* ------------------------------------------------------------ raster */

static uint32_t lift(uint32_t rgb, uint8_t min_value)
{
    const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    const int v = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (min_value == 0 || v >= min_value) {
        return rgb;
    }
    if (v == 0) {
        return (uint32_t)min_value << 16 | (uint32_t)min_value << 8 | min_value;
    }
    return (uint32_t)(r * min_value / v) << 16 | (uint32_t)(g * min_value / v) << 8 | (uint32_t)(b * min_value / v);
}

static void span(rc_t *rc, int y, int x0, int x1, uint32_t rgb, uint8_t a)
{
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 > rc->w) {
        x1 = rc->w;
    }
    uint8_t *p = rc->buf + ((size_t)y * rc->w + x0) * 4;
    const uint8_t sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    for (int x = x0; x < x1; x++, p += 4) {
        const uint8_t da = p[3];
        if (a == 255 || da == 0) {
            p[0] = sb;
            p[1] = sg;
            p[2] = sr;
            p[3] = a;
            continue;
        }
        const uint32_t wd = (uint32_t)da * (255 - a) / 255; /* weight of what is there */
        const uint32_t oa = a + wd;
        p[0] = (uint8_t)((sb * a + p[0] * wd) / oa);
        p[1] = (uint8_t)((sg * a + p[1] * wd) / oa);
        p[2] = (uint8_t)((sr * a + p[2] * wd) / oa);
        p[3] = (uint8_t)(oa > 255 ? 255 : oa);
    }
}

/* Non-zero fill of the closed polygons sub[0..nsub) of pts. */
static void fill_poly(rc_t *rc, const pt_t *pts, const sub_t *sub, int nsub, int npts, uint32_t rgb, uint8_t a)
{
    if (a == 0 || npts < 3) {
        return;
    }
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < npts; i++) {
        ymin = fminf(ymin, pts[i].y);
        ymax = fmaxf(ymax, pts[i].y);
    }
    int y0 = (int)floorf(ymin);
    int y1 = (int)ceilf(ymax);
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > rc->h) {
        y1 = rc->h;
    }
    if (!svgm_grow((void **)&rc->xs, &rc->xcap, npts + 2, sizeof(float)) ||
        !svgm_grow((void **)&rc->dirs, &rc->dcap, npts + 2, sizeof(int8_t))) {
        return;
    }
    for (int y = y0; y < y1; y++) {
        const float yc = (float)y + 0.5f;
        int n = 0;
        for (int s = 0; s < nsub; s++) {
            const int first = sub[s].start;
            const int last = (s + 1 < nsub ? sub[s + 1].start : npts) - 1;
            for (int i = first; i <= last; i++) {
                const pt_t p0 = pts[i];
                const pt_t p1 = pts[i == last ? first : i + 1];
                if ((p0.y <= yc && p1.y > yc) || (p1.y <= yc && p0.y > yc)) {
                    const float x = p0.x + (yc - p0.y) * (p1.x - p0.x) / (p1.y - p0.y);
                    /* insertion sort by x */
                    int j = n++;
                    while (j > 0 && rc->xs[j - 1] > x) {
                        rc->xs[j] = rc->xs[j - 1];
                        rc->dirs[j] = rc->dirs[j - 1];
                        j--;
                    }
                    rc->xs[j] = x;
                    rc->dirs[j] = p1.y > p0.y ? 1 : -1;
                }
            }
        }
        int wind = 0;
        for (int i = 0; i + 1 < n; i++) {
            wind += rc->dirs[i];
            if (wind != 0) {
                const int xa = (int)ceilf(rc->xs[i] - 0.5f);
                const int xb = (int)ceilf(rc->xs[i + 1] - 0.5f);
                if (xb > xa) {
                    span(rc, y, xa, xb, rgb, a);
                }
            }
        }
    }
}

static void stroke_seg(rc_t *rc, pt_t p0, pt_t p1, float hw, uint32_t rgb, uint8_t a)
{
    float dx = p1.x - p0.x, dy = p1.y - p0.y;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) {
        return;
    }
    dx /= len;
    dy /= len;
    const float nx = -dy * hw, ny = dx * hw;
    const pt_t q[4] = {{p0.x + nx, p0.y + ny}, {p1.x + nx, p1.y + ny}, {p1.x - nx, p1.y - ny}, {p0.x - nx, p0.y - ny}};
    const sub_t sub0 = {0, true};
    fill_poly(rc, q, &sub0, 1, 4, rgb, a);
}

static void stroke_join(rc_t *rc, pt_t c, float hw, uint32_t rgb, uint8_t a)
{
    pt_t q[8];
    for (int i = 0; i < 8; i++) {
        const float ang = (float)i * (float)M_PI / 4.0f;
        q[i].x = c.x + cosf(ang) * hw;
        q[i].y = c.y + sinf(ang) * hw;
    }
    const sub_t sub0 = {0, true};
    fill_poly(rc, q, &sub0, 1, 8, rgb, a);
}

/* ------------------------------------------------------------ geometry */

static pt_t to_dev(const rc_t *rc, const mat_t *m, float x, float y)
{
    const float ux = m->a * x + m->c * y + m->e;
    const float uy = m->b * x + m->d * y + m->f;
    return (pt_t){rc->ox + rc->s * (ux - rc->vbx), rc->oy + rc->s * (uy - rc->vby)};
}

static void path_reset(rc_t *rc)
{
    rc->npts = 0;
    rc->nsub = 0;
}

static void path_move(rc_t *rc)
{
    /* drop an empty subpath */
    if (rc->nsub > 0 && rc->sub[rc->nsub - 1].start == rc->npts) {
        return;
    }
    if (!svgm_grow((void **)&rc->sub, &rc->subcap, rc->nsub + 1, sizeof(sub_t))) {
        return;
    }
    rc->sub[rc->nsub++] = (sub_t){rc->npts, false};
}

static void path_add(rc_t *rc, const mat_t *m, float x, float y)
{
    if (rc->nsub == 0) {
        path_move(rc);
    }
    if (!svgm_grow((void **)&rc->pts, &rc->cap, rc->npts + 1, sizeof(pt_t))) {
        return;
    }
    rc->pts[rc->npts++] = to_dev(rc, m, x, y);
}

static void path_close(rc_t *rc)
{
    if (rc->nsub > 0) {
        rc->sub[rc->nsub - 1].closed = true;
    }
}

/* Paints the collected path with the element style. */
static void path_paint(rc_t *rc, const style_t *st, bool closed_shape)
{
    if (rc->npts < 2) {
        return;
    }
    if (!st->fill.none && rc->npts >= 3) {
        const float a = st->opacity * st->fill_op;
        fill_poly(rc, rc->pts, rc->sub, rc->nsub, rc->npts, lift(st->fill.rgb, rc->min_value),
            (uint8_t)fminf(255.0f, a * 255.0f + 0.5f));
    }
    if (!st->stroke.none && st->stroke_w > 0.0f) {
        const float a = st->opacity * st->stroke_op;
        const uint8_t a8 = (uint8_t)fminf(255.0f, a * 255.0f + 0.5f);
        const float scale = rc->s * sqrtf(fabsf(st->m.a * st->m.d - st->m.b * st->m.c));
        const float hw = fmaxf(0.5f, st->stroke_w * scale / 2.0f);
        const uint32_t rgb = lift(st->stroke.rgb, rc->min_value);
        for (int s = 0; s < rc->nsub; s++) {
            const int first = rc->sub[s].start;
            const int last = (s + 1 < rc->nsub ? rc->sub[s + 1].start : rc->npts) - 1;
            for (int i = first; i < last; i++) {
                stroke_seg(rc, rc->pts[i], rc->pts[i + 1], hw, rgb, a8);
                if (hw >= 1.5f && i > first) {
                    stroke_join(rc, rc->pts[i], hw, rgb, a8);
                }
            }
            if ((rc->sub[s].closed || closed_shape) && last > first) {
                stroke_seg(rc, rc->pts[last], rc->pts[first], hw, rgb, a8);
            }
        }
    }
}

static void flatten_cubic(rc_t *rc, const mat_t *m, float x0, float y0, float x1, float y1, float x2, float y2,
    float x3, float y3)
{
    for (int i = 1; i <= SVGM_CURVE_STEPS; i++) {
        const float t = (float)i / SVGM_CURVE_STEPS, u = 1.0f - t;
        const float x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        const float y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
        path_add(rc, m, x, y);
    }
}

static void parse_path(rc_t *rc, const mat_t *m, const char *d, const char *dend)
{
    float cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; /* current, subpath start, last control */
    char cmd = 0, prev = 0;
    const char *p = d;
    while (p < dend) {
        p = skip_ws(p);
        if (p >= dend) {
            break;
        }
        if (isalpha((unsigned char)*p)) {
            cmd = *p++;
        } else if (cmd == 0) {
            break;
        }
        const bool rel = islower((unsigned char)cmd);
        const char up = (char)toupper((unsigned char)cmd);
        float a[7];
        int need = 0;
        switch (up) {
        case 'M': case 'L': case 'T': need = 2; break;
        case 'H': case 'V': need = 1; break;
        case 'C': need = 6; break;
        case 'S': case 'Q': need = 4; break;
        case 'A': need = 7; break;
        case 'Z': need = 0; break;
        default: return;
        }
        bool ok = true;
        for (int i = 0; i < need && ok; i++) {
            ok = parse_num(&p, &a[i]);
        }
        if (!ok || p > dend) {
            break;
        }
        const float bx = rel ? cx : 0, by = rel ? cy : 0;
        switch (up) {
        case 'M':
            cx = a[0] + bx;
            cy = a[1] + by;
            sx = cx;
            sy = cy;
            path_move(rc);
            path_add(rc, m, cx, cy);
            cmd = rel ? 'l' : 'L'; /* further pairs are line-tos */
            break;
        case 'L':
            cx = a[0] + bx;
            cy = a[1] + by;
            path_add(rc, m, cx, cy);
            break;
        case 'H':
            cx = a[0] + (rel ? cx : 0);
            path_add(rc, m, cx, cy);
            break;
        case 'V':
            cy = a[0] + (rel ? cy : 0);
            path_add(rc, m, cx, cy);
            break;
        case 'C':
            flatten_cubic(rc, m, cx, cy, a[0] + bx, a[1] + by, a[2] + bx, a[3] + by, a[4] + bx, a[5] + by);
            lcx = a[2] + bx;
            lcy = a[3] + by;
            cx = a[4] + bx;
            cy = a[5] + by;
            break;
        case 'S': {
            const bool smooth = prev == 'C' || prev == 'S';
            const float x1 = smooth ? 2 * cx - lcx : cx, y1 = smooth ? 2 * cy - lcy : cy;
            flatten_cubic(rc, m, cx, cy, x1, y1, a[0] + bx, a[1] + by, a[2] + bx, a[3] + by);
            lcx = a[0] + bx;
            lcy = a[1] + by;
            cx = a[2] + bx;
            cy = a[3] + by;
            break;
        }
        case 'Q': {
            const float qx = a[0] + bx, qy = a[1] + by, ex = a[2] + bx, ey = a[3] + by;
            flatten_cubic(rc, m, cx, cy, cx + 2.0f / 3 * (qx - cx), cy + 2.0f / 3 * (qy - cy),
                ex + 2.0f / 3 * (qx - ex), ey + 2.0f / 3 * (qy - ey), ex, ey);
            lcx = qx;
            lcy = qy;
            cx = ex;
            cy = ey;
            break;
        }
        case 'T': {
            const bool smooth = prev == 'Q' || prev == 'T';
            const float qx = smooth ? 2 * cx - lcx : cx, qy = smooth ? 2 * cy - lcy : cy;
            const float ex = a[0] + bx, ey = a[1] + by;
            flatten_cubic(rc, m, cx, cy, cx + 2.0f / 3 * (qx - cx), cy + 2.0f / 3 * (qy - cy),
                ex + 2.0f / 3 * (qx - ex), ey + 2.0f / 3 * (qy - ey), ex, ey);
            lcx = qx;
            lcy = qy;
            cx = ex;
            cy = ey;
            break;
        }
        case 'A': /* arcs as a straight line to their end point */
            cx = a[5] + bx;
            cy = a[6] + by;
            path_add(rc, m, cx, cy);
            break;
        case 'Z':
            path_close(rc);
            cx = sx;
            cy = sy;
            break;
        }
        prev = up;
    }
}

static void parse_points(rc_t *rc, const mat_t *m, const char *v, const char *vend)
{
    const char *p = v;
    float x, y;
    path_move(rc);
    while (p < vend && parse_num(&p, &x) && parse_num(&p, &y)) {
        path_add(rc, m, x, y);
    }
}

static float attr_num(const char *tag, const char *end, const char *name, float def)
{
    char v[32];
    if (!attr(tag, end, name, v, sizeof(v))) {
        return def;
    }
    const char *p = v;
    float f = def;
    if (!parse_num(&p, &f)) {
        return def;
    }
    return *p == '%' ? NAN : f;
}

static void draw_ellipse(rc_t *rc, const mat_t *m, float cx, float cy, float rx, float ry)
{
    const int n = 32;
    path_move(rc);
    for (int i = 0; i < n; i++) {
        const float ang = (float)i * 2.0f * (float)M_PI / n;
        path_add(rc, m, cx + rx * cosf(ang), cy + ry * sinf(ang));
    }
    path_close(rc);
}

/* ------------------------------------------------------------ document */

static const char *skip_element(const char *p, const char *name, size_t nl)
{
    /* past the matching </name> (no nesting of the same name expected) */
    char close[24];
    snprintf(close, sizeof(close), "</%.*s", (int)(nl < 20 ? nl : 20), name);
    const char *e = strstr(p, close);
    if (e == NULL) {
        return p + strlen(p);
    }
    const char *gt = strchr(e, '>');
    return gt != NULL ? gt + 1 : e + strlen(e);
}

static void draw_document(rc_t *rc, const char *svg)
{
    style_t stack[SVGM_STACK];
    int depth = 0;
    style_t base = {MAT_ID, {0x000000, false}, {0, true}, 1.0f, 1.0f, 1.0f, 1.0f};
    stack[0] = base;

    const char *p = svg;
    while ((p = strchr(p, '<')) != NULL) {
        p++;
        if (*p == '/') {
            if (strncmp(p, "/g", 2) == 0 || strncmp(p, "/a", 2) == 0 || strncmp(p, "/svg", 4) == 0) {
                if (depth > 0) {
                    depth--;
                }
            }
            continue;
        }
        if (*p == '!' || *p == '?') {
            if (strncmp(p, "!--", 3) == 0) {
                const char *e = strstr(p, "-->");
                p = e != NULL ? e + 3 : p + strlen(p);
            }
            continue;
        }
        const char *name = p;
        while (*p && !isspace((unsigned char)*p) && *p != '/' && *p != '>') {
            p++;
        }
        const size_t nl = (size_t)(p - name);
        const char *end = strchr(p, '>');
        if (end == NULL) {
            break;
        }
        const bool self_closing = end > p && end[-1] == '/';
        const char *tag = name - 1; /* attr() looks at the char before a name */
#define NAME(s) (nl == strlen(s) && strncmp(name, s, nl) == 0)
        if (NAME("g") || NAME("a") || NAME("svg")) {
            style_t st = stack[depth];
            if (!NAME("svg")) {
                apply_style(&st, tag, end);
            }
            if (!self_closing && depth + 1 < SVGM_STACK) {
                stack[++depth] = st;
            }
            p = end + 1;
            continue;
        }
        if (NAME("defs") || NAME("text") || NAME("style") || NAME("clipPath") || NAME("mask") || NAME("title") ||
            NAME("desc") || NAME("metadata") || NAME("symbol") || NAME("linearGradient") ||
            NAME("radialGradient") || NAME("pattern")) {
            p = self_closing ? end + 1 : skip_element(end + 1, name, nl);
            continue;
        }
        style_t st = stack[depth];
        apply_style(&st, tag, end);
        path_reset(rc);
        bool closed = false;
        if (NAME("path")) {
            const char *ve = NULL;
            const char *d = attr_ref(tag, end, "d", &ve);
            if (d != NULL) {
                parse_path(rc, &st.m, d, ve);
            }
        } else if (NAME("polygon") || NAME("polyline")) {
            const char *ve = NULL;
            const char *v = attr_ref(tag, end, "points", &ve);
            if (v != NULL) {
                parse_points(rc, &st.m, v, ve);
                closed = NAME("polygon");
                if (closed) {
                    path_close(rc);
                }
            }
        } else if (NAME("line")) {
            path_move(rc);
            path_add(rc, &st.m, attr_num(tag, end, "x1", 0), attr_num(tag, end, "y1", 0));
            path_add(rc, &st.m, attr_num(tag, end, "x2", 0), attr_num(tag, end, "y2", 0));
            st.fill.none = true;
        } else if (NAME("rect")) {
            const float x = attr_num(tag, end, "x", 0), y = attr_num(tag, end, "y", 0);
            const float w = attr_num(tag, end, "width", 0), h = attr_num(tag, end, "height", 0);
            /* percent sizes: the page background, left transparent */
            if (!isnan(x) && !isnan(y) && !isnan(w) && !isnan(h) && w > 0 && h > 0) {
                path_move(rc);
                path_add(rc, &st.m, x, y);
                path_add(rc, &st.m, x + w, y);
                path_add(rc, &st.m, x + w, y + h);
                path_add(rc, &st.m, x, y + h);
                path_close(rc);
            }
        } else if (NAME("circle")) {
            const float r = attr_num(tag, end, "r", 0);
            if (r > 0 && !isnan(r)) {
                draw_ellipse(rc, &st.m, attr_num(tag, end, "cx", 0), attr_num(tag, end, "cy", 0), r, r);
            }
        } else if (NAME("ellipse")) {
            const float rx = attr_num(tag, end, "rx", 0), ry = attr_num(tag, end, "ry", 0);
            if (rx > 0 && ry > 0) {
                draw_ellipse(rc, &st.m, attr_num(tag, end, "cx", 0), attr_num(tag, end, "cy", 0), rx, ry);
            }
        }
#undef NAME
        path_paint(rc, &st, closed);
        p = end + 1;
    }
}

/* ------------------------------------------------------------ main shape */

/* Bounding box of the largest connected shape, found on a grid of
 * SVGM_CELL px cells so that nearby strokes join up. */
static bool main_shape(const uint8_t *buf, int w, int h, int *bx0, int *by0, int *bx1, int *by1)
{
    const int gw = (w + SVGM_CELL - 1) / SVGM_CELL;
    const int gh = (h + SVGM_CELL - 1) / SVGM_CELL;
    const int cells = gw * gh;
    uint16_t *count = heap_caps_calloc((size_t)cells, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    uint8_t *seen = heap_caps_calloc((size_t)cells, 1, MALLOC_CAP_SPIRAM);
    int *stack = heap_caps_malloc((size_t)cells * sizeof(int), MALLOC_CAP_SPIRAM);
    bool found = false;
    uint32_t best = 0;
    if (count == NULL || seen == NULL || stack == NULL) {
        goto done;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *row = buf + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            if (row[x * 4 + 3] > 48) {
                count[(y / SVGM_CELL) * gw + x / SVGM_CELL]++;
            }
        }
    }
    for (int seed = 0; seed < cells; seed++) {
        if (count[seed] == 0 || seen[seed]) {
            continue;
        }
        int top = 0;
        uint32_t px = 0;
        int x0 = seed % gw, x1 = x0, y0 = seed / gw, y1 = y0;
        stack[top++] = seed;
        seen[seed] = 1;
        while (top > 0) {
            const int c = stack[--top];
            const int cx = c % gw, cy = c / gw;
            px += count[c];
            x0 = cx < x0 ? cx : x0;
            x1 = cx > x1 ? cx : x1;
            y0 = cy < y0 ? cy : y0;
            y1 = cy > y1 ? cy : y1;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) {
                        continue;
                    }
                    const int n = ny * gw + nx;
                    if (count[n] > 0 && !seen[n]) {
                        seen[n] = 1;
                        stack[top++] = n;
                    }
                }
            }
        }
        if (px > best) {
            best = px;
            *bx0 = x0 * SVGM_CELL;
            *by0 = y0 * SVGM_CELL;
            *bx1 = ((x1 + 1) * SVGM_CELL < w ? (x1 + 1) * SVGM_CELL : w) - 1;
            *by1 = ((y1 + 1) * SVGM_CELL < h ? (y1 + 1) * SVGM_CELL : h) - 1;
            found = true;
        }
    }
done:
    heap_caps_free(count);
    heap_caps_free(seen);
    heap_caps_free(stack);
    return found;
}

/* ------------------------------------------------------------ API */

static bool view_box(const char *svg, float vb[4])
{
    const char *tag = strstr(svg, "<svg");
    const char *end = tag != NULL ? strchr(tag, '>') : NULL;
    if (end == NULL) {
        return false;
    }
    char v[96];
    if (attr(tag + 1, end, "viewBox", v, sizeof(v))) {
        const char *p = v;
        if (parse_num(&p, &vb[0]) && parse_num(&p, &vb[1]) && parse_num(&p, &vb[2]) && parse_num(&p, &vb[3]) &&
            vb[2] > 0 && vb[3] > 0) {
            return true;
        }
    }
    const float w = attr_num(tag + 1, end, "width", 0), h = attr_num(tag + 1, end, "height", 0);
    if (!isnan(w) && !isnan(h) && w > 0 && h > 0) {
        vb[0] = 0;
        vb[1] = 0;
        vb[2] = w;
        vb[3] = h;
        return true;
    }
    return false;
}

uint8_t *svg_map_render(const char *svg, int w, int h, const svg_map_opts_t *opts)
{
    float vb[4];
    if (svg == NULL || w <= 0 || h <= 0 || !view_box(svg, vb)) {
        return NULL;
    }
    rc_t rc = {0};
    rc.w = w;
    rc.h = h;
    rc.min_value = opts != NULL ? opts->min_value : 0;
    rc.buf = heap_caps_calloc((size_t)w * h, 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc.buf == NULL) {
        return NULL;
    }
    rc.vbx = vb[0];
    rc.vby = vb[1];
    rc.s = fminf((float)w / vb[2], (float)h / vb[3]);
    rc.ox = ((float)w - vb[2] * rc.s) / 2.0f;
    rc.oy = ((float)h - vb[3] * rc.s) / 2.0f;
    draw_document(&rc, svg);

    int x0, y0, x1, y1;
    if (opts != NULL && opts->zoom_main_shape && main_shape(rc.buf, w, h, &x0, &y0, &x1, &y1)) {
        const float bw = (float)(x1 - x0 + 1), bh = (float)(y1 - y0 + 1);
        float zoom = fminf((float)(w - 2 * SVGM_MAP_MARGIN) / bw, (float)(h - 2 * SVGM_MAP_MARGIN) / bh);
        zoom = fminf(zoom, SVGM_MAX_ZOOM);
        if (zoom > 1.06f) {
            /* drawing coordinates of the shape's centre */
            const float ux = rc.vbx + (((float)(x0 + x1 + 1) / 2.0f) - rc.ox) / rc.s;
            const float uy = rc.vby + (((float)(y0 + y1 + 1) / 2.0f) - rc.oy) / rc.s;
            rc.s *= zoom;
            rc.ox = (float)w / 2.0f - (ux - rc.vbx) * rc.s;
            rc.oy = (float)h / 2.0f - (uy - rc.vby) * rc.s;
            memset(rc.buf, 0, (size_t)w * h * 4);
            draw_document(&rc, svg);
        }
    }
    heap_caps_free(rc.pts);
    heap_caps_free(rc.sub);
    heap_caps_free(rc.xs);
    heap_caps_free(rc.dirs);
    return rc.buf;
}
