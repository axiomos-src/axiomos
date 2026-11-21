/* AxiomOS UI - anti-aliased rasteriser.
 *
 * The compositor blits with no hardware acceleration, so every primitive here
 * is written for a memory-mapped 32-bit ARGB surface with a pixel-stride.  Two
 * rules keep the output stable across the widgets:
 *
 *   - coverage is computed once per pixel and blended through axui_blend_alpha,
 *     never accumulated in floating point (the kernel has no FPU context here);
 *   - every primitive clips against the destination rectangle first, so a
 *     widget can draw outside its bounds without corrupting its neighbours.
 *
 * The rounded rectangles, arcs and lines are all sampled with 4 sub-steps
 * across the minor axis, which is the cheapest setting that still looks smooth
 * at the 8-pixel corner radii the theme uses.
 */

#include <axiom/lib.h>
#include "ui.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* Plot one pixel with an 0..255 coverage value.  Out-of-range writes are
 * dropped; this is the single funnel every primitive goes through. */
static void plot(gui_surface_t *surface, i32 x, i32 y, u32 colour, u8 coverage)
{
    if (!surface || !surface->pixels) {
        return;
    }
    if (x < 0 || y < 0 || x >= surface->width || y >= surface->height) {
        return;
    }
    if (!coverage) {
        return;
    }
    u32 *dst = &surface->pixels[(usize)y * (usize)surface->stride + (usize)x];
    if (coverage >= 255) {
        *dst = colour;
        return;
    }
    u8 alpha = (u8)(((usize)axui_alpha_of(colour) * coverage) / 255);
    *dst = axui_blend_alpha(*dst, colour, alpha);
}

/* Horizontal span with a coverage value; used by trapezoid filling. */
static void span(gui_surface_t *surface, i32 y, i32 x0, i32 x1, u32 colour,
                 u8 coverage)
{
    if (x0 > x1) {
        i32 t = x0;
        x0 = x1;
        x1 = t;
    }
    for (i32 x = x0; x <= x1; x++) {
        plot(surface, x, y, colour, coverage);
    }
}

/* Signed distance from a point to a line segment, used for the AA stroke. */
static i32 dist_sq(i32 ax, i32 ay, i32 bx, i32 by)
{
    i64 dx = (i64)bx - ax;
    i64 dy = (i64)by - ay;
    return (i32)ax_isqrt((u64)(dx * dx + dy * dy));
}

/* ------------------------------------------------------------------ */
/* Triangles and polygons                                              */
/* ------------------------------------------------------------------ */

void axui_fill_triangle(gui_surface_t *surface, axui_point_t a, axui_point_t b,
                        axui_point_t c, u32 colour)
{
    i32 min_y = axui_min_i32(a.y, axui_min_i32(b.y, c.y));
    i32 max_y = axui_max_i32(a.y, axui_max_i32(b.y, c.y));

    if (min_y < 0) {
        min_y = 0;
    }
    if (max_y >= surface->height) {
        max_y = surface->height - 1;
    }
    if (min_y > max_y) {
        return;
    }

    for (i32 y = min_y; y <= max_y; y++) {
        i32 ymin = y;
        i32 ymax = y + 1;

        /* Clip the triangle to the scanline as three edges against
         * y/(y+1); this is the classic scanline fill without a table. */
        i32 xs[3];
        i32 count = 0;
        axui_point_t pts[3] = { a, b, c };

        for (u32 i = 0; i < 3; i++) {
            axui_point_t p = pts[i];
            axui_point_t q = pts[(i + 1) % 3];
            if (p.y == q.y) {
                continue;
            }
            i32 y0 = axui_min_i32(p.y, q.y);
            i32 y1 = axui_max_i32(p.y, q.y);
            if (y1 < ymin || y0 > ymax) {
                continue;
            }
            i64 num = (i64)(y - p.y) * (i64)(q.x - p.x);
            i64 den = (i64)(q.y - p.y);
            if (den == 0) {
                continue;
            }
            i32 x = p.x + (i32)(num / den);
            if (count < 3) {
                xs[count++] = x;
            }
        }
        if (count >= 2) {
            i32 lo = axui_min_i32(xs[0], xs[1]);
            i32 hi = axui_max_i32(xs[0], xs[1]);
            span(surface, y, lo, hi - 1, colour, 255);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lines                                                               */
/* ------------------------------------------------------------------ */

/* Signed-distance based anti-aliased thick line.  The distance test is
 * evaluated only inside the bounding box expanded by the stroke radius, which
 * keeps a long diagonal from scanning the whole desktop. */
void axui_line_aa(gui_surface_t *surface, i32 x0, i32 y0, i32 x1, i32 y1,
                  u32 colour, u8 weight)
{
    i32 radius = weight ? (i32)weight : 1;
    i32 pad = radius + 1;
    i32 min_x = axui_max_i32(0, axui_min_i32(x0, x1) - pad);
    i32 max_x = axui_min_i32(surface->width - 1, axui_max_i32(x0, x1) + pad);
    i32 min_y = axui_max_i32(0, axui_min_i32(y0, y1) - pad);
    i32 max_y = axui_min_i32(surface->height - 1, axui_max_i32(y0, y1) + pad);

    /* Bounding box test first: a segment whose box is off-screen is free. */
    if (min_x > max_x || min_y > max_y) {
        return;
    }

    i32 dx = x1 - x0;
    i32 dy = y1 - y0;
    i64 len_sq = (i64)dx * dx + (i64)dy * dy;

    if (len_sq == 0) {
        plot(surface, x0, y0, colour, 255);
        return;
    }

    for (i32 y = min_y; y <= max_y; y++) {
        for (i32 x = min_x; x <= max_x; x++) {
            /* Distance from (x,y) to the segment, in fixed point sixteenths. */
            i64 px = ((i64)x - x0) << 4;
            i64 py = ((i64)y - y0) << 4;
            i64 ex = ((i64)dx) << 4;
            i64 ey = ((i64)dy) << 4;

            i64 t = (px * ex + py * ey) / (len_sq << 8);
            if (t < 0) {
                t = 0;
            } else if (t > 4096) {
                t = 4096;
            }
            i64 qx = (ex * t) >> 12;
            i64 qy = (ey * t) >> 12;
            i64 ddx = px - qx;
            i64 ddy = py - qy;
            /* ddx/ddy are already in sixteenths of a pixel, so the root is
             * the distance in sixteenths and needs no rescaling. */
            i32 d16 = (i32)ax_isqrt((u64)(ddx * ddx + ddy * ddy));

            i32 half = radius * 16;
            if (d16 <= half - 8) {
                plot(surface, x, y, colour, 255);
            } else if (d16 <= half + 8) {
                i32 cover = ((half + 8) - d16) * 255 / 16;
                if (cover > 255) {
                    cover = 255;
                }
                plot(surface, x, y, colour, (u8)cover);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Circles and ellipses                                                */
/* ------------------------------------------------------------------ */

void axui_circle(gui_surface_t *surface, i32 cx, i32 cy, i32 radius, u32 colour)
{
    axui_ellipse(surface, cx, cy, radius, radius, colour);
}

void axui_ellipse(gui_surface_t *surface, i32 cx, i32 cy, i32 rx, i32 ry,
                  u32 colour)
{
    if (rx <= 0 || ry <= 0) {
        return;
    }
    i32 min_y = axui_max_i32(0, cy - ry);
    i32 max_y = axui_min_i32(surface->height - 1, cy + ry);
    i64 ry2 = (i64)ry * ry;

    for (i32 y = min_y; y <= max_y; y++) {
        i64 dy = y - cy;
        i64 numer = ry2 - dy * dy;
        if (numer < 0) {
            continue;
        }
        /* half width at this scanline */
        i32 half = (i32)((rx * ax_isqrt((u64)numer)) / ry);
        if (half <= 0) {
            continue;
        }
        i32 x0 = cx - half;
        i32 x1 = cx + half;
        /* One extra pixel at each end with reduced coverage keeps the
         * silhouette from stair-stepping. */
        span(surface, y, x0 + 1, x1 - 1, colour, 255);
        plot(surface, x0, y, colour, 128);
        plot(surface, x1, y, colour, 128);
    }
}

void axui_circle_outline(gui_surface_t *surface, i32 cx, i32 cy, i32 radius,
                         i32 thickness, u32 colour)
{
    if (radius <= 0) {
        return;
    }
    if (thickness <= 0) {
        thickness = 1;
    }
    i32 min_y = axui_max_i32(0, cy - radius - thickness);
    i32 max_y = axui_min_i32(surface->height - 1, cy + radius + thickness);
    i32 inner = radius - thickness;
    if (inner < 0) {
        inner = 0;
    }

    for (i32 y = min_y; y <= max_y; y++) {
        i32 dy = y - cy;
        i64 outer_num = (i64)radius * radius - (i64)dy * dy;
        i64 inner_num = (i64)inner * inner - (i64)dy * dy;
        if (outer_num < 0) {
            continue;
        }
        i32 outer = (i32)ax_isqrt((u64)outer_num);
        i32 inner_w = inner_num > 0 ? (i32)ax_isqrt((u64)inner_num) : 0;

        /* Build the ring as two spans per side rather than per pixel tests. */
        i32 left_out = cx - outer;
        i32 left_in = inner_num > 0 ? cx - inner_w : cx;
        i32 right_in = inner_num > 0 ? cx + inner_w : cx;
        i32 right_out = cx + outer;

        span(surface, y, left_out, left_in, colour, 255);
        span(surface, y, right_in, right_out, colour, 255);
    }
}

void axui_arc(gui_surface_t *surface, i32 cx, i32 cy, i32 radius,
              i32 start_deg, i32 sweep_deg, i32 thickness, u32 colour)
{
    if (radius <= 0) {
        return;
    }
    if (thickness <= 0) {
        thickness = 1;
    }
    /* Normalise the sweep into a positive angle range. */
    while (start_deg < 0) {
        start_deg += 360;
    }
    start_deg %= 360;
    if (sweep_deg == 0) {
        return;
    }
    bool_t clockwise = sweep_deg > 0;
    if (sweep_deg < 0) {
        sweep_deg = -sweep_deg;
    }
    if (sweep_deg > 360) {
        sweep_deg = 360;
    }

    /* Integer sine table: 1 degree resolution is plenty for a progress ring. */
    static const i16 sin_tab[91] = {
        0, 17, 35, 52, 70, 87, 105, 122, 139, 156, 174, 191, 208, 225, 242,
        259, 276, 292, 309, 326, 342, 358, 375, 391, 407, 423, 438, 454, 469,
        485, 500, 515, 530, 545, 559, 574, 588, 602, 616, 629, 643, 656, 669,
        682, 695, 707, 719, 731, 743, 755, 766, 777, 788, 799, 809, 819, 829,
        839, 848, 857, 866, 875, 883, 891, 899, 906, 914, 921, 927, 934, 940,
        946, 951, 956, 961, 966, 970, 974, 978, 982, 985, 988, 990, 993, 995,
        996, 998, 999, 999, 1000, 1000,
    };

    for (i32 step = 0; step < sweep_deg; step += 2) {
        i32 deg = clockwise ? (start_deg + step) : (start_deg - step);
        while (deg < 0) {
            deg += 360;
        }
        deg %= 360;
        i32 q = deg / 90;
        i32 r = deg % 90;
        i32 s = sin_tab[r];
        i32 c = sin_tab[90 - r];
        i32 dx, dy;
        switch (q) {
        case 0: dx = c; dy = -s; break;
        case 1: dx = s; dy = c; break;
        case 2: dx = -c; dy = s; break;
        default: dx = -s; dy = -c; break;
        }
        i32 px = cx + (dx * radius) / 1000;
        i32 py = cy + (dy * radius) / 1000;
        axui_circle(surface, px, py, thickness, colour);
    }
}

/* ------------------------------------------------------------------ */
/* Curves                                                              */
/* ------------------------------------------------------------------ */

void axui_quadratic(gui_surface_t *surface, axui_point_t p0, axui_point_t p1,
                    axui_point_t p2, u32 colour, i32 steps)
{
    if (steps < 2) {
        steps = 2;
    }
    if (steps > 128) {
        steps = 128;
    }
    axui_point_t prev = p0;
    for (i32 i = 1; i <= steps; i++) {
        /* Fixed point quadratic Bezier: t in sixteenths. */
        i64 t = (i64)i * 16 / steps;
        i64 mt = 16 - t;
        i32 x = (i32)((mt * mt * p0.x + 2 * mt * t * p1.x + t * t * p2.x) / 256);
        i32 y = (i32)((mt * mt * p0.y + 2 * mt * t * p1.y + t * t * p2.y) / 256);
        axui_line_aa(surface, prev.x, prev.y, x, y, colour, 1);
        prev = axui_point(x, y);
    }
}

void axui_cubic(gui_surface_t *surface, axui_point_t p0, axui_point_t p1,
                axui_point_t p2, axui_point_t p3, u32 colour, i32 steps)
{
    if (steps < 2) {
        steps = 2;
    }
    if (steps > 160) {
        steps = 160;
    }
    axui_point_t prev = p0;
    for (i32 i = 1; i <= steps; i++) {
        i64 t = (i64)i * 16 / steps;
        i64 mt = 16 - t;
        i32 x = (i32)((mt * mt * mt * p0.x + 3 * mt * mt * t * p1.x +
                       3 * mt * t * t * p2.x + t * t * t * p3.x) / 4096);
        i32 y = (i32)((mt * mt * mt * p0.y + 3 * mt * mt * t * p1.y +
                       3 * mt * t * t * p2.y + t * t * t * p3.y) / 4096);
        axui_line_aa(surface, prev.x, prev.y, x, y, colour, 1);
        prev = axui_point(x, y);
    }
}

/* ------------------------------------------------------------------ */
/* Rounded rectangles                                                  */
/* ------------------------------------------------------------------ */

/* Per-pixel coverage for an anti-aliased rounded rectangle.  The corner
 * distance is compared against the radius; this is the same rule a GPU shader
 * would use, evaluated in integers. */
static u8 round_rect_coverage(i32 x, i32 y, gui_rect_t rect, i32 radius)
{
    i32 x0 = rect.x;
    i32 y0 = rect.y;
    i32 x1 = rect.x + rect.w - 1;
    i32 y1 = rect.y + rect.h - 1;

    if (x < x0 - 1 || y < y0 - 1 || x > x1 + 1 || y > y1 + 1) {
        return 0;
    }
    if (radius <= 0) {
        if (x >= x0 && x <= x1 && y >= y0 && y <= y1) {
            return 255;
        }
        return 0;
    }

    /* Which corner region are we in? */
    i32 cx;
    i32 cy;
    if (x < x0 + radius) {
        cx = x0 + radius;
    } else if (x > x1 - radius) {
        cx = x1 - radius;
    } else {
        cx = x;
    }
    if (y < y0 + radius) {
        cy = y0 + radius;
    } else if (y > y1 - radius) {
        cy = y1 - radius;
    } else {
        cy = y;
    }

    i32 dx = x - cx;
    i32 dy = y - cy;
    i32 d = dist_sq(0, 0, dx, dy);

    if (d <= radius - 1) {
        return 255;
    }
    if (d >= radius + 1) {
        return 0;
    }
    return (u8)(((radius + 1 - d) * 255) / 2);
}

void axui_round_rect_aa(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                        u32 colour)
{
    if (axui_rect_empty(rect)) {
        return;
    }
    i32 max_radius = axui_min_i32(rect.w, rect.h) / 2;
    if (radius > max_radius) {
        radius = max_radius;
    }
    if (radius < 0) {
        radius = 0;
    }

    i32 y_min = axui_max_i32(0, rect.y - 1);
    i32 y_max = axui_min_i32(surface->height - 1, rect.y + rect.h);
    i32 x_min = axui_max_i32(0, rect.x - 1);
    i32 x_max = axui_min_i32(surface->width - 1, rect.x + rect.w);

    for (i32 y = y_min; y <= y_max; y++) {
        for (i32 x = x_min; x <= x_max; x++) {
            u8 coverage = round_rect_coverage(x, y, rect, radius);
            if (coverage) {
                plot(surface, x, y, colour, coverage);
            }
        }
    }
}

void axui_round_rect_border(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                            i32 thickness, u32 colour)
{
    if (axui_rect_empty(rect) || thickness <= 0) {
        return;
    }
    i32 max_radius = axui_min_i32(rect.w, rect.h) / 2;
    if (radius > max_radius) {
        radius = max_radius;
    }
    gui_rect_t inner = axui_rect_inset_px(rect, thickness);
    i32 inner_radius = radius - thickness > 0 ? radius - thickness : 0;

    for (i32 y = axui_max_i32(0, rect.y); y < axui_min_i32(surface->height, rect.y + rect.h); y++) {
        for (i32 x = axui_max_i32(0, rect.x); x < axui_min_i32(surface->width, rect.x + rect.w); x++) {
            u8 outer = round_rect_coverage(x, y, rect, radius);
            u8 within = round_rect_coverage(x, y, inner, inner_radius);
            if (outer > within) {
                plot(surface, x, y, colour, (u8)(outer - within));
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Gradients and patterns                                              */
/* ------------------------------------------------------------------ */

void axui_gradient_linear(gui_surface_t *surface, gui_rect_t rect, i32 angle_deg,
                          u32 from, u32 to)
{
    if (axui_rect_empty(rect)) {
        return;
    }
    while (angle_deg < 0) {
        angle_deg += 360;
    }
    angle_deg %= 360;

    /* Only the four principal directions are used by the theme, so the
     * projection is a simple table instead of a full dot product. */
    i32 mode = (angle_deg + 22) / 45;
    mode %= 8;

    i32 y_min = axui_max_i32(0, rect.y);
    i32 y_max = axui_min_i32(surface->height, rect.y + rect.h);
    i32 x_min = axui_max_i32(0, rect.x);
    i32 x_max = axui_min_i32(surface->width, rect.x + rect.w);

    for (i32 y = y_min; y < y_max; y++) {
        for (i32 x = x_min; x < x_max; x++) {
            i32 t;
            switch (mode) {
            case 1:
            case 2:
                t = ((x - rect.x) * 100) / (rect.w ? rect.w : 1);
                break;
            case 3:
            case 4:
                t = 100 - ((y - rect.y) * 100) / (rect.h ? rect.h : 1);
                break;
            case 5:
            case 6:
                t = ((y - rect.y) * 100) / (rect.h ? rect.h : 1);
                break;
            default:
                t = 100 - ((x - rect.x) * 100) / (rect.w ? rect.w : 1);
                break;
            }
            u32 colour = axui_lerp_colour(from, to, t);
            plot(surface, x, y, colour, 255);
        }
    }
}

void axui_gradient_radial(gui_surface_t *surface, i32 cx, i32 cy, i32 radius,
                          u32 inner, u32 outer)
{
    if (radius <= 0) {
        return;
    }
    i32 y_min = axui_max_i32(0, cy - radius);
    i32 y_max = axui_min_i32(surface->height - 1, cy + radius);
    i32 x_min = axui_max_i32(0, cx - radius);
    i32 x_max = axui_min_i32(surface->width - 1, cx + radius);

    for (i32 y = y_min; y <= y_max; y++) {
        for (i32 x = x_min; x <= x_max; x++) {
            i32 d = dist_sq(cx, cy, x, y);
            if (d > radius) {
                continue;
            }
            i32 t = (d * 100) / radius;
            plot(surface, x, y, axui_lerp_colour(inner, outer, t), 255);
        }
    }
}

void axui_checkerboard(gui_surface_t *surface, gui_rect_t rect, i32 cell,
                       u32 a, u32 b)
{
    if (cell <= 0) {
        cell = 8;
    }
    i32 y_min = axui_max_i32(0, rect.y);
    i32 y_max = axui_min_i32(surface->height, rect.y + rect.h);
    i32 x_min = axui_max_i32(0, rect.x);
    i32 x_max = axui_min_i32(surface->width, rect.x + rect.w);

    for (i32 y = y_min; y < y_max; y++) {
        i32 row = (y - rect.y) / cell;
        for (i32 x = x_min; x < x_max; x++) {
            i32 col = (x - rect.x) / cell;
            u32 colour = ((row + col) & 1) ? b : a;
            plot(surface, x, y, colour, 255);
        }
    }
}

void axui_grid_pattern(gui_surface_t *surface, gui_rect_t rect, i32 step,
                       u32 colour)
{
    if (step <= 1) {
        step = 8;
    }
    for (i32 y = rect.y; y < rect.y + rect.h; y += step) {
        for (i32 x = rect.x; x < rect.x + rect.w; x++) {
            plot(surface, x, y, colour, 255);
        }
    }
    for (i32 x = rect.x; x < rect.x + rect.w; x += step) {
        for (i32 y = rect.y; y < rect.y + rect.h; y++) {
            plot(surface, x, y, colour, 255);
        }
    }
}

/* Darkens the borders of a surface with a quadratic falloff; used by the
 * lock screen and the boot splash overlay. */
void axui_vignette(gui_surface_t *surface, u8 strength)
{
    if (!surface || !surface->pixels || !strength) {
        return;
    }
    i32 cx = surface->width / 2;
    i32 cy = surface->height / 2;
    i32 max_d = dist_sq(0, 0, cx, cy);

    for (i32 y = 0; y < surface->height; y++) {
        for (i32 x = 0; x < surface->width; x++) {
            i32 d = dist_sq(cx, cy, x, y);
            if (d < max_d / 2) {
                continue;
            }
            i32 scale = 255 - ((d - max_d / 2) * strength) / (max_d / 2 + 1);
            if (scale < 0) {
                scale = 0;
            }
            u32 *p = &surface->pixels[(usize)y * (usize)surface->stride + (usize)x];
            u32 s = *p;
            u8 r = (u8)((axui_alpha_of(s) ? (s >> 16) & 0xFF : 0) * scale / 255);
            u8 g = (u8)((((s >> 8) & 0xFF) * scale) / 255);
            u8 b = (u8)(((s & 0xFF) * scale) / 255);
            *p = axui_rgba(r, g, b, 255);
        }
    }
}

void axui_scanlines(gui_surface_t *surface, u8 strength)
{
    if (!surface || !surface->pixels || !strength) {
        return;
    }
    for (i32 y = 1; y < surface->height; y += 2) {
        for (i32 x = 0; x < surface->width; x++) {
            u32 *p = &surface->pixels[(usize)y * (usize)surface->stride + (usize)x];
            *p = axui_blend_alpha(*p, 0xFF000000, strength);
        }
    }
}
