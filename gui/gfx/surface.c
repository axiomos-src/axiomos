/*
 * AxiomOS - software drawing primitives.
 *
 * Everything the compositor needs is implemented here against plain ARGB
 * surfaces: solid fills, alpha blends, rounded rectangles, gradients, lines,
 * surface blits and drop shadows.  No floating point is used anywhere: the
 * compositor must run before the FPU state has been saved for a thread, and
 * integer blending is exact and fast enough at 1024x768 on a single core.
 */

#include <gui.h>

/* ---------------------------------------------------------------- helpers */

static inline u32 blend_pixel(u32 dst, u32 src, u32 alpha)
{
    /* Alpha is 0..255; src colour is premultiplied by the caller's alpha. */
    u32 inv = 255 - alpha;
    u32 r = ((GUI_R(src) * alpha + GUI_R(dst) * inv) / 255) & 0xFF;
    u32 g = ((GUI_G(src) * alpha + GUI_G(dst) * inv) / 255) & 0xFF;
    u32 b = ((GUI_B(src) * alpha + GUI_B(dst) * inv) / 255) & 0xFF;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void gui_surface_wrap(gui_surface_t *surface, u32 *pixels, i32 width, i32 height,
                      i32 stride)
{
    surface->pixels = pixels;
    surface->width = width;
    surface->height = height;
    surface->stride = stride;
    surface->owned = false;
}

gui_surface_t *gui_surface_new(i32 width, i32 height)
{
    if (width <= 0 || height <= 0)
        return 0;
    gui_surface_t *surface = (gui_surface_t *)kzalloc(sizeof(gui_surface_t));
    if (!surface)
        return 0;
    u32 bytes = (u32)width * (u32)height * 4u;
    u32 *pixels = (u32 *)kmalloc((u64)bytes);
    if (!pixels) {
        kfree(surface);
        return 0;
    }
    gui_surface_wrap(surface, pixels, width, height, width);
    surface->owned = true;
    return surface;
}

void gui_surface_free(gui_surface_t *surface)
{
    if (!surface)
        return;
    if (surface->owned && surface->pixels)
        kfree(surface->pixels);
    kfree(surface);
}

/* ---------------------------------------------------------------- fills */

void gui_fill(gui_surface_t *surface, u32 colour)
{
    if (!surface)
        return;
    i32 total = surface->width * surface->height;
    for (i32 i = 0; i < total; i++)
        surface->pixels[i] = colour;
}

void gui_fill_rect(gui_surface_t *surface, gui_rect_t rect, u32 colour)
{
    if (!surface)
        return;
    i32 x0 = rect.x < 0 ? 0 : rect.x;
    i32 y0 = rect.y < 0 ? 0 : rect.y;
    i32 x1 = rect.x + rect.w;
    i32 y1 = rect.y + rect.h;
    if (x1 > surface->width)
        x1 = surface->width;
    if (y1 > surface->height)
        y1 = surface->height;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (i32 y = y0; y < y1; y++) {
        u32 *row = surface->pixels + (u64)y * surface->stride;
        for (i32 x = x0; x < x1; x++)
            row[x] = colour;
    }
}

void gui_blend_rect(gui_surface_t *surface, gui_rect_t rect, u32 colour)
{
    u32 alpha = GUI_ALPHA(colour);
    if (alpha == 0 || !surface)
        return;
    if (alpha == 255) {
        gui_fill_rect(surface, rect, colour | 0xFF000000u);
        return;
    }
    i32 x0 = rect.x < 0 ? 0 : rect.x;
    i32 y0 = rect.y < 0 ? 0 : rect.y;
    i32 x1 = rect.x + rect.w;
    i32 y1 = rect.y + rect.h;
    if (x1 > surface->width)
        x1 = surface->width;
    if (y1 > surface->height)
        y1 = surface->height;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (i32 y = y0; y < y1; y++) {
        u32 *row = surface->pixels + (u64)y * surface->stride;
        for (i32 x = x0; x < x1; x++)
            row[x] = blend_pixel(row[x], colour, alpha);
    }
}

void gui_fill_round_rect(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                         u32 colour)
{
    if (!surface)
        return;
    if (radius <= 0) {
        gui_fill_rect(surface, rect, colour);
        return;
    }
    u8 alpha = (u8)GUI_ALPHA(colour);
    /* Corners: for each corner, only draw pixels inside the circle. */
    gui_fill_rect(surface, gui_rect(rect.x + radius, rect.y,
                                    rect.w - radius * 2, rect.h), colour);
    gui_fill_rect(surface, gui_rect(rect.x, rect.y + radius, radius,
                                    rect.h - radius * 2), colour);
    gui_fill_rect(surface, gui_rect(rect.x + rect.w - radius,
                                    rect.y + radius, radius,
                                    rect.h - radius * 2), colour);
    for (i32 cy = 0; cy < radius; cy++) {
        for (i32 cx = 0; cx < radius; cx++) {
            i32 dx = radius - 1 - cx;
            i32 dy = radius - 1 - cy;
            /* Keep pixels whose centre falls inside the quarter circle. */
            if (dx * dx + dy * dy > radius * radius)
                continue;
            i32 left = rect.x + cx;
            i32 right = rect.x + rect.w - 1 - cx;
            i32 top = rect.y + cy;
            i32 bottom = rect.y + rect.h - 1 - cy;
            if (top >= 0 && top < surface->height) {
                if (left >= 0 && left < surface->width)
                    surface->pixels[(u64)top * surface->stride + left] = colour;
                if (right >= 0 && right < surface->width)
                    surface->pixels[(u64)top * surface->stride + right] = colour;
            }
            if (bottom >= 0 && bottom < surface->height) {
                if (left >= 0 && left < surface->width)
                    surface->pixels[(u64)bottom * surface->stride + left] = colour;
                if (right >= 0 && right < surface->width)
                    surface->pixels[(u64)bottom * surface->stride + right] = colour;
            }
        }
    }
    (void)alpha;
}

void gui_vgradient(gui_surface_t *surface, gui_rect_t rect, u32 top, u32 bottom)
{
    if (!surface || rect.h <= 0)
        return;
    for (i32 y = 0; y < rect.h; y++) {
        i32 sy = rect.y + y;
        if (sy < 0 || sy >= surface->height)
            continue;
        u32 weight = (u32)((y * 255) / (rect.h > 1 ? rect.h - 1 : 1));
        u32 colour = 0xFF000000u
                   | (((GUI_R(top) * (255 - weight) + GUI_R(bottom) * weight) / 255) << 16)
                   | (((GUI_G(top) * (255 - weight) + GUI_G(bottom) * weight) / 255) << 8)
                   |  ((GUI_B(top) * (255 - weight) + GUI_B(bottom) * weight) / 255);
        gui_fill_rect(surface, gui_rect(rect.x, sy, rect.w, 1), colour);
    }
}

void gui_hline(gui_surface_t *surface, i32 x0, i32 x1, i32 y, u32 colour)
{
    if (x0 > x1) {
        i32 tmp = x0;
        x0 = x1;
        x1 = tmp;
    }
    gui_fill_rect(surface, gui_rect(x0, y, x1 - x0 + 1, 1), colour);
}

void gui_vline(gui_surface_t *surface, i32 x, i32 y0, i32 y1, u32 colour)
{
    if (y0 > y1) {
        i32 tmp = y0;
        y0 = y1;
        y1 = tmp;
    }
    gui_fill_rect(surface, gui_rect(x, y0, 1, y1 - y0 + 1), colour);
}

void gui_line(gui_surface_t *surface, i32 x0, i32 y0, i32 x1, i32 y1, u32 colour)
{
    i32 dx = x1 - x0;
    i32 dy = y1 - y0;
    i32 sx = dx < 0 ? -1 : 1;
    i32 sy = dy < 0 ? -1 : 1;
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    i32 err = dx - dy;
    for (;;) {
        gui_fill_rect(surface, gui_rect(x0, y0, 1, 1), colour);
        if (x0 == x1 && y0 == y1)
            break;
        i32 e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gui_rect_outline(gui_surface_t *surface, gui_rect_t rect, u32 colour)
{
    gui_hline(surface, rect.x, rect.x + rect.w - 1, rect.y, colour);
    gui_hline(surface, rect.x, rect.x + rect.w - 1, rect.y + rect.h - 1, colour);
    gui_vline(surface, rect.x, rect.y, rect.y + rect.h - 1, colour);
    gui_vline(surface, rect.x + rect.w - 1, rect.y, rect.y + rect.h - 1, colour);
}

/* ----------------------------------------------------------------- blits */

void gui_blit(gui_surface_t *dst, i32 dx, i32 dy, const gui_surface_t *src,
              gui_rect_t src_rect)
{
    if (!dst || !src)
        return;
    gui_rect_t clipped = gui_rect_intersect(src_rect,
                                            gui_rect(0, 0, src->width, src->height));
    for (i32 y = 0; y < clipped.h; y++) {
        i32 sy = clipped.y + y;
        i32 ty = dy + sy - src_rect.y;
        if (ty < 0 || ty >= dst->height)
            continue;
        u32 *dst_row = dst->pixels + (u64)ty * dst->stride;
        const u32 *src_row = src->pixels + (u64)sy * src->stride;
        for (i32 x = 0; x < clipped.w; x++) {
            i32 sx = clipped.x + x;
            i32 tx = dx + sx - src_rect.x;
            if (tx < 0 || tx >= dst->width)
                continue;
            dst_row[tx] = src_row[sx];
        }
    }
}

void gui_blit_surface(gui_surface_t *dst, i32 dx, i32 dy, const gui_surface_t *src)
{
    gui_blit(dst, dx, dy, src, gui_rect(0, 0, src->width, src->height));
}

void gui_blit_alpha(gui_surface_t *dst, i32 dx, i32 dy, const gui_surface_t *src,
                    u8 alpha)
{
    if (!dst || !src)
        return;
    for (i32 y = 0; y < src->height; y++) {
        i32 ty = dy + y;
        if (ty < 0 || ty >= dst->height)
            continue;
        u32 *dst_row = dst->pixels + (u64)ty * dst->stride;
        const u32 *src_row = src->pixels + (u64)y * src->stride;
        for (i32 x = 0; x < src->width; x++) {
            i32 tx = dx + x;
            if (tx < 0 || tx >= dst->width)
                continue;
            u32 pixel = src_row[x];
            if (GUI_ALPHA(pixel) == 0)
                continue;
            dst_row[tx] = blend_pixel(dst_row[tx], pixel, alpha);
        }
    }
}

void gui_shadow(gui_surface_t *dst, gui_rect_t rect, i32 spread, u8 strength)
{
    if (!dst)
        return;
    /* Cheap two dimensional falloff: draw concentric rectangles whose alpha
     * decays quadratically.  Only the region outside `rect` is touched. */
    for (i32 i = spread; i > 0; i--) {
        u32 alpha = (u32)strength * (u32)(spread - i + 1) * (u32)(spread - i + 1)
                    / (u32)(spread * spread);
        if (alpha == 0)
            continue;
        gui_rect_t band = gui_rect(rect.x - i, rect.y - i,
                                   rect.w + i * 2, rect.h + i * 2);
        gui_rect_t inner = gui_rect(band.x + 1, band.y + 1,
                                    band.w - 2, band.h - 2);
        u32 colour = GUI_ARGB((u8)alpha, 0, 0, 0);
        /* top and bottom bands */
        gui_blend_rect(dst, gui_rect(band.x, band.y, band.w, 1), colour);
        gui_blend_rect(dst, gui_rect(band.x, band.y + band.h - 1, band.w, 1), colour);
        gui_blend_rect(dst, gui_rect(band.x, band.y + 1, 1, band.h - 2), colour);
        gui_blend_rect(dst, gui_rect(band.x + band.w - 1, band.y + 1, 1,
                                     band.h - 2), colour);
        (void)inner;
    }
}
