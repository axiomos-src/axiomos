/* AxiomOS terminal - painter.
 *
 * The terminal is drawn straight into a window surface, cell by cell, with no
 * intermediate glyph cache.  Each cell is two fills (background, then the
 * glyph bits), so a full 100x32 screen costs about 3200 small blits, which
 * the software compositor handles comfortably at one frame per redraw.
 *
 * Scrolled-back content is painted from the history ring first; the live grid
 * fills whatever remains.  That keeps a single code path for both views and
 * makes the selection maths identical whether a row came from history or from
 * the screen.
 */

#include <axiom/lib.h>
#include "term.h"

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

void axterm_set_metrics(term_t *term, u8 scale)
{
    if (!term) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 2) {
        scale = 2;
    }
    term->scale = scale;
    term->cell_w = 8 * (i32)scale;
    term->cell_h = 16 * (i32)scale;
    term->padding = 4;
}

i32 axterm_pixel_width(const term_t *term)
{
    if (!term) {
        return 0;
    }
    return term->cols * term->cell_w + term->padding * 2;
}

i32 axterm_pixel_height(const term_t *term)
{
    if (!term) {
        return 0;
    }
    return term->rows * term->cell_h + term->padding * 2;
}

/* Recompute the column and row counts for the given window size.  Called on
 * every resize; a shrink is fed through axterm_resize so the scrollback keeps
 * the lines that fall off. */
void axterm_on_resize(term_t *term, i32 width, i32 height)
{
    if (!term) {
        return;
    }
    if (term->cell_w <= 0) {
        axterm_set_metrics(term, 1);
    }
    i32 cols = (width - term->padding * 2) / term->cell_w;
    i32 rows = (height - term->padding * 2) / term->cell_h;
    if (cols < 8) {
        cols = 8;
    }
    if (rows < 3) {
        rows = 3;
    }
    axterm_resize(term, cols, rows);
}

/* ------------------------------------------------------------------ */
/* Background and glyph                                                */
/* ------------------------------------------------------------------ */

static void fill_rect(gui_surface_t *surface, gui_rect_t rect, u32 colour)
{
    gui_rect_t clipped = gui_rect_intersect(rect, gui_rect(0, 0, surface->width,
                                                           surface->height));
    if (axui_rect_empty(clipped)) {
        return;
    }
    for (i32 y = clipped.y; y < clipped.y + clipped.h; y++) {
        for (i32 x = clipped.x; x < clipped.x + clipped.w; x++) {
            u32 *p = &surface->pixels[(usize)y * (usize)surface->stride + (usize)x];
            *p = colour;
        }
    }
}

/* Blit the 8x16 bitmap of one character scaled up, clipped to `clip`.  The
 * glyph bits come from the face, or from the synthesised box-drawing set. */
static void blit_glyph(gui_surface_t *surface, i32 px, i32 py, u32 ch, u32 colour,
                       u8 scale, gui_rect_t clip, bool_t bold)
{
    for (i32 row = 0; row < 16; row++) {
        u8 bits = 0;
        if (ch < 0x80) {
            bits = axui_glyph_row((u8)ch, row);
        } else if (ch <= 0xFF) {
            u8 synth = 0;
            if (axui_glyph_synthetic((u8)ch, row, &synth)) {
                bits = synth;
            }
        }
        if (!bits) {
            continue;
        }
        for (i32 col = 0; col < 8; col++) {
            if (!(bits & (0x80u >> col))) {
                continue;
            }
            i32 x0 = px + col * (i32)scale;
            i32 y0 = py + row * (i32)scale;
            for (i32 sy = 0; sy < (i32)scale; sy++) {
                for (i32 sx = 0; sx < (i32)scale; sx++) {
                    i32 dx = x0 + sx + (bold ? 1 : 0);
                    i32 dy = y0 + sy;
                    if (dx < clip.x || dy < clip.y ||
                        dx >= clip.x + clip.w || dy >= clip.y + clip.h) {
                        continue;
                    }
                    if (dx >= surface->width || dy >= surface->height) {
                        continue;
                    }
                    u32 *p = &surface->pixels[(usize)dy * (usize)surface->stride +
                                              (usize)dx];
                    *p = axui_blend_alpha(*p, colour, 255);
                }
            }
        }
    }
}

void axterm_draw_cell(term_t *term, gui_surface_t *surface, term_cell_t *cell,
                      i32 px, i32 py, bool_t selected)
{
    if (!term || !cell) {
        return;
    }
    gui_rect_t cell_rect = gui_rect(px, py, term->cell_w, term->cell_h);
    gui_rect_t clip = gui_rect_intersect(cell_rect,
                                         gui_rect(0, 0, surface->width, surface->height));
    if (axui_rect_empty(clip)) {
        return;
    }

    u32 fg = axterm_effective_fg(term, cell);
    u32 bg = axterm_effective_bg(term, cell);

    if (selected) {
        /* Selection inverts, which stays visible whatever the cell's own
         * colours are. */
        u32 t = fg;
        fg = bg;
        bg = t;
        if (!(bg & 0xFF000000u)) {
            bg = axui_rgba(0x2A, 0x4A, 0x86, 255);
        }
    }

    fill_rect(surface, cell_rect, bg);

    if (cell->ch == 0 || cell->ch == ' ') {
        /* A blank cell still needs its background, which the fill did. */
        if (cell->flags & TERM_A_UNDERLINE) {
            i32 uy = py + term->cell_h - 2;
            fill_rect(surface, gui_rect(px, uy, term->cell_w, 1), fg);
        }
        return;
    }

    /* A double-width head paints into the following cell as well; the tail
     * cell is skipped by the caller. */
    i32 span = cell->wide == 1 ? 1 : 0;
    blit_glyph(surface, px, py, (u32)cell->ch, fg, term->scale, clip,
               (bool_t)((cell->flags & TERM_A_BOLD) != 0));
    if (span) {
        /* Double-width: stretch by repeating the glyph in the next cell. */
        blit_glyph(surface, px + term->cell_w, py, (u32)cell->ch, fg, term->scale,
                   clip, (bool_t)((cell->flags & TERM_A_BOLD) != 0));
    }

    if (cell->flags & TERM_A_UNDERLINE) {
        i32 uy = py + term->cell_h - 2;
        fill_rect(surface, gui_rect(px, uy, term->cell_w, 1), fg);
    }
    if (cell->flags & TERM_A_STRIKE) {
        i32 sy = py + term->cell_h / 2;
        fill_rect(surface, gui_rect(px, sy, term->cell_w, 1), fg);
    }
    if (cell->flags & TERM_A_BLINK) {
        /* Blink is a slow on/off driven by the frame counter in app.c; the
         * paint pass simply honours the phase stored in `term->blink_phase`. */
        if (term->modes & TERM_MODE_CURSOR_BLINK) {
            /* Nothing to do here: the app toggles the flag before painting. */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Cursor                                                              */
/* ------------------------------------------------------------------ */

static void paint_cursor(term_t *term, gui_surface_t *surface, i32 px, i32 py,
                         bool_t visible)
{
    if (!visible) {
        return;
    }
    gui_rect_t rect = gui_rect(px, py, term->cell_w, term->cell_h);
    if (axui_rect_empty(gui_rect_intersect(rect, gui_rect(0, 0, surface->width,
                                                          surface->height)))) {
        return;
    }
    /* Outline plus a translucent fill: readable over both a glyph and a
     * coloured background. */
    u32 colour = term->cursor_colour;
    for (i32 x = 0; x < rect.w; x++) {
        for (i32 y = 0; y < rect.h; y++) {
            bool_t edge = (bool_t)(x == 0 || y == 0 || x == rect.w - 1 ||
                                   y == rect.h - 1);
            i32 dx = rect.x + x;
            i32 dy = rect.y + y;
            if (dx < 0 || dy < 0 || dx >= surface->width || dy >= surface->height) {
                continue;
            }
            u32 *p = &surface->pixels[(usize)dy * (usize)surface->stride + (usize)dx];
            *p = axui_blend_alpha(*p, colour, (u8)(edge ? 255 : 70));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main paint                                                          */
/* ------------------------------------------------------------------ */

void axterm_paint(term_t *term, gui_surface_t *surface, gui_rect_t area)
{
    if (!term || !surface) {
        return;
    }
    if (term->cell_w <= 0) {
        axterm_set_metrics(term, 1);
    }

    fill_rect(surface, area, term->default_bg);

    i32 total = axterm_view_lines(term);
    i32 top = axterm_view_top(term);
    i32 max_rows = area.h / term->cell_h;
    if (max_rows > term->rows) {
        max_rows = term->rows;
    }

    i32 done_rows = 0;
    for (i32 visible_row = 0; visible_row < max_rows; visible_row++) {
        i32 row = top + visible_row;
        if (row >= total) {
            break;
        }
        const term_cell_t *line = axterm_view_line(term, row);
        if (!line) {
            break;
        }
        i32 py = area.y + visible_row * term->cell_h;
        for (i32 x = 0; x < term->cols; x++) {
            i32 px = area.x + x * term->cell_w;
            if (px + term->cell_w > area.x + area.w) {
                break;
            }
            bool_t selected = axterm_selection_contains(term, x, visible_row);
            term_cell_t copy = line[x];
            axterm_draw_cell(term, surface, &copy, px, py, selected);
            if (copy.wide == 1) {
                /* Skip the tail cell; its content is covered by the stretch. */
                x++;
            }
        }
        done_rows++;
    }

    /* Blank the remainder of the area below the last row, so a scrolled
     * history view does not leave a coloured band. */
    i32 used = area.y + done_rows * term->cell_h;
    if (used < area.y + area.h) {
        fill_rect(surface, gui_rect(area.x, used, area.w, area.y + area.h - used),
                  term->default_bg);
    }

    /* Cursor: only on the live screen and only when fully scrolled back to
     * the bottom. */
    if (term->history.offset == 0 && term->cursor.visible) {
        i32 px = area.x + term->cursor.x * term->cell_w;
        i32 py = area.y + term->cursor.y * term->cell_h;
        if (px + term->cell_w <= area.x + area.w &&
            py + term->cell_h <= area.y + area.h) {
            paint_cursor(term, surface, px, py,
                         (bool_t)((term->modes & TERM_MODE_CURSOR_BLINK) == 0 ||
                                  term->blink_on));
        }
    }

    term->dirty = (bool_t)0;
    term->scrolled = (bool_t)0;
}

/* ------------------------------------------------------------------ */
/* Status bar                                                          */
/* ------------------------------------------------------------------ */

void axterm_paint_status(term_t *term, gui_surface_t *surface, gui_rect_t area)
{
    if (!term || !surface) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    fill_rect(surface, area, axui_tint(theme->window_bg, theme->accent, 6));

    /* Left: the terminal title or the shell's prompt status. */
    char left[64];
    ax_strlcpy(left, term->title[0] ? term->title : "terminal",
               sizeof(left));

    /* Middle: attributes and geometry. */
    char middle[64];
    (void)ax_snprintf(middle, sizeof(middle), "%dx%d  %s", term->cols, term->rows,
                      axterm_attribute_name(term));

    /* Right: scrollback position and byte counter. */
    char right[64];
    (void)ax_snprintf(right, sizeof(right), "%d/%d  %u B",
                      term->history.count - term->history.offset,
                      term->history.count, (u32)term->bytes_in);

    axui_text_style_t style = axui_style(theme->text_dim, 1);
    axui_draw_text_clip(surface, area, area.x + 6, axui_text_baseline(area, 1),
                        left, style);

    axui_text_style_t mid_style = axui_style(theme->text_dim, 1);
    i32 mid_x = area.x + area.w / 2 - axui_measure_text(middle, 1) / 2;
    axui_draw_text_clip(surface, area, mid_x, axui_text_baseline(area, 1), middle,
                        mid_style);

    axui_text_style_t right_style = axui_style(theme->accent, 1);
    axui_draw_text_right(surface, area, right, right_style);
}

/* ------------------------------------------------------------------ */
/* Scrollbar                                                           */
/* ------------------------------------------------------------------ */

void axterm_paint_scrollbar(term_t *term, gui_surface_t *surface, gui_rect_t area)
{
    if (!term || !surface || area.w <= 0) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    fill_rect(surface, area, axui_tint(theme->window_bg_alt, theme->border, 30));

    i32 total = axterm_history_count(term) + term->rows;
    if (total <= term->rows) {
        /* Nothing to scroll: a faint full-length thumb still communicates
         * "there is a scrollbar here". */
        gui_rect_t track = gui_rect(area.x + 2, area.y + 2, area.w - 4, area.h - 4);
        fill_rect(surface, track, axui_colour_alpha(theme->border, 120));
        return;
    }

    i32 thumb_h = area.h * term->rows / total;
    if (thumb_h < 12) {
        thumb_h = 12;
    }
    if (thumb_h > area.h - 4) {
        thumb_h = area.h - 4;
    }

    i32 max_offset = total - term->rows;
    i32 position = max_offset > 0
                       ? (term->history.count - term->history.offset) * (area.h - 4 - thumb_h) /
                             max_offset
                       : 0;
    if (position < 0) {
        position = 0;
    }

    gui_rect_t thumb = gui_rect(area.x + 2, area.y + 2 + position, area.w - 4,
                                thumb_h);
    gui_rect_t clipped = gui_rect_intersect(thumb, area);
    if (!axui_rect_empty(clipped)) {
        fill_rect(surface, clipped, axui_colour_alpha(theme->accent, 150));
        fill_rect(surface, gui_rect(clipped.x, clipped.y, 1, clipped.h),
                  theme->accent);
    }
}

/* ------------------------------------------------------------------ */
/* Overlays                                                            */
/* ------------------------------------------------------------------ */

/* A translucent "scrolled back" banner at the top edge, so a user cannot
 * mistake a frozen screen for a hung program. */
void axterm_paint_banner(term_t *term, gui_surface_t *surface, gui_rect_t area)
{
    if (!term || !surface || term->history.offset == 0) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    char text[64];
    (void)ax_snprintf(text, sizeof(text), "scrollback: %d lines", term->history.offset);

    gui_rect_t banner = gui_rect(area.x + area.w / 2 - 90, area.y + 4, 180, 18);
    fill_rect(surface, banner, axui_colour_alpha(theme->window_bg, 220));
    for (i32 x = 0; x < banner.w; x++) {
        fill_rect(surface, gui_rect(banner.x + x, banner.y, 1, 1), theme->accent);
        fill_rect(surface, gui_rect(banner.x + x, banner.y + banner.h - 1, 1, 1),
                  theme->accent);
    }
    axui_text_style_t style = axui_style(theme->accent, 1);
    axui_draw_text_center(surface, banner, text, style);
}

/* Draw a 256-colour test strip; the shell's `colours` command renders it and
 * it doubles as a visual check that true-colour SGR works. */
void axterm_paint_palette_strip(term_t *term, gui_surface_t *surface, gui_rect_t area)
{
    if (!term || !surface) {
        return;
    }
    i32 cell_w = area.w / 32 > 2 ? area.w / 32 : 2;
    for (i32 row = 0; row < 8; row++) {
        for (i32 col = 0; col < 32; col++) {
            i32 index = row * 32 + col;
            gui_rect_t cell = gui_rect(area.x + col * cell_w, area.y + row * 12,
                                       cell_w - 1, 11);
            if (axui_rect_empty(gui_rect_intersect(cell,
                                                   gui_rect(0, 0, surface->width,
                                                            surface->height)))) {
                continue;
            }
            fill_rect(surface, cell, axterm_xterm256(index));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Frame bookkeeping                                                   */
/* ------------------------------------------------------------------ */

/* Toggle the cursor blink phase; app.c calls this once per compositor frame
 * with the elapsed milliseconds. */
void axterm_blink_update(term_t *term, u32 elapsed_ms)
{
    if (!term) {
        return;
    }
    if (!(term->modes & TERM_MODE_CURSOR_BLINK)) {
        term->blink_on = (bool_t)1;
        return;
    }
    u32 phase = (elapsed_ms / TERM_BLINK_MS) & 1u;
    bool_t on = (bool_t)(phase == 0);
    if (on != term->blink_on) {
        term->blink_on = on;
        term->dirty = (bool_t)1;
    }
}
