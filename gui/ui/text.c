/* AxiomOS UI - text.
 *
 * The desktop uses a single built-in 8x16 bitmap face (gui_font8x16) scaled by
 * an integer factor.  There is no glyph cache: at the scales the theme uses
 * (1 and 2) and with a memory-mapped framebuffer, reading the bitmap directly
 * is cheaper than maintaining a cache, and it keeps the kernel's heap out of
 * the text path entirely.
 *
 * The module covers three things the widget layer needs:
 *
 *   - measurement, so widgets can size themselves before painting;
 *   - layout, including word wrap with break-anywhere fallback and the
 *     ellipsis form used in list rows;
 *   - painting, with clipping, bold (double-strike) and underline support.
 *
 * All of the box-drawing characters in 0xB3..0xDA are synthesised, because the
 * 8x16 face only covers printable ASCII and the terminal and the settings app
 * both draw frames.
 */

#include <axiom/lib.h>
#include "ui.h"

/* The font is owned by gui/font.c; only the metrics are needed here. */
extern const u8 gui_font8x16[95][16];
extern const u8 gui_font_width;
extern const u8 gui_font_height;
extern const u8 gui_font_first;
extern const u8 gui_font_last;

#define AXUI_TAB_WIDTH 4

/* ------------------------------------------------------------------ */
/* Style helpers                                                       */
/* ------------------------------------------------------------------ */

axui_text_style_t axui_style(u32 colour, u8 scale)
{
    axui_text_style_t style;
    style.colour = colour;
    style.scale = scale ? scale : 1;
    style.bold = (bool_t)0;
    style.underline = (bool_t)0;
    style.underline_colour = colour;
    return style;
}

i32 axui_char_width(u8 scale)
{
    return (i32)(scale ? scale : 1) * (i32)gui_font_width;
}

i32 axui_line_height(u8 scale)
{
    return (i32)(scale ? scale : 1) * (i32)gui_font_height;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

/* Decode one code point.  Returns the number of bytes consumed (at least 1);
 * *out_code receives the code point, or '?' for anything outside the face. */
i32 axui_utf8_decode(const char *text, u32 *out_code)
{
    u8 b0 = (u8)text[0];
    if (b0 < 0x80) {
        *out_code = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        u8 b1 = (u8)text[1];
        if ((b1 & 0xC0) != 0x80) {
            *out_code = '?';
            return 1;
        }
        *out_code = ((u32)(b0 & 0x1F) << 6) | (u32)(b1 & 0x3F);
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0) {
        u8 b1 = (u8)text[1];
        u8 b2 = (u8)text[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
            *out_code = '?';
            return 1;
        }
        *out_code = ((u32)(b0 & 0x0F) << 12) | ((u32)(b1 & 0x3F) << 6) |
                    (u32)(b2 & 0x3F);
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        u8 b1 = (u8)text[1];
        u8 b2 = (u8)text[2];
        u8 b3 = (u8)text[3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
            *out_code = '?';
            return 1;
        }
        *out_code = ((u32)(b0 & 0x07) << 18) | ((u32)(b1 & 0x3F) << 12) |
                    ((u32)(b2 & 0x3F) << 6) | (u32)(b3 & 0x3F);
        return 4;
    }
    *out_code = '?';
    return 1;
}

/* How many cells one code point occupies.  The face is ASCII only, so this
 * is always one; the helper exists so the terminal's wide-character path has a
 * single place to change. */
i32 axui_utf8_cells(const char *text)
{
    u32 code;
    (void)axui_utf8_decode(text, &code);
    return 1;
}

usize axui_utf8_length(const char *text)
{
    usize count = 0;
    const char *p = text;
    while (*p) {
        u32 code;
        p += axui_utf8_decode(p, &code);
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Glyph access                                                        */
/* ------------------------------------------------------------------ */

/* Map a byte to a font row bitmap.  Values outside the face return 0 so the
 * caller can fall back to the synthesised box glyphs below. */
u8 axui_glyph_row(u8 ch, i32 row)
{
    if (row < 0 || row >= 16) {
        return 0;
    }
    if (ch < gui_font_first || ch > gui_font_last) {
        return 0;
    }
    return gui_font8x16[ch - gui_font_first][row];
}

/* Synthesised box drawing / block drawing glyphs for the terminal.  Returns
 * (bool_t)0 when the character is not one of the synthesised set. */
bool_t axui_glyph_synthetic(u8 ch, i32 row, u8 *bits)
{
    if (!bits) {
        return (bool_t)0;
    }
    if (row < 0 || row >= 16) {
        return (bool_t)0;
    }
    bool_t top = (bool_t)(row <= 1);
    bool_t bottom = (bool_t)(row >= 14);
    bool_t vmid = (bool_t)(row >= 7 && row <= 8);
    bool_t left = (bool_t)1;
    bool_t right = (bool_t)1;

    switch (ch) {
    case 0xB3:                    /* vertical */
        *bits = 0x18;
        return (bool_t)1;
    case 0xC4:                    /* horizontal */
        *bits = (u8)(left && right ? 0xFF : 0);
        return (bool_t)1;
    case 0xDA:                    /* top left corner */
        if (row >= 7 && row <= 8) {
            *bits = 0xF8;
        } else if (row > 8) {
            *bits = 0x18;
        } else {
            *bits = 0xF8;
        }
        return (bool_t)1;
    case 0xBF:                    /* top right corner */
        if (row >= 7 && row <= 8) {
            *bits = 0x1F;
        } else if (row > 8) {
            *bits = 0x18;
        } else {
            *bits = 0x1F;
        }
        return (bool_t)1;
    case 0xC0:                    /* bottom left corner */
        if (row >= 7 && row <= 8) {
            *bits = 0xF8;
        } else if (row < 7) {
            *bits = 0x18;
        } else {
            *bits = 0x18;
        }
        return (bool_t)1;
    case 0xD9:                    /* bottom right corner */
        if (row >= 7 && row <= 8) {
            *bits = 0x1F;
        } else if (row < 7) {
            *bits = 0x18;
        } else {
            *bits = 0x18;
        }
        return (bool_t)1;
    case 0xC3:                    /* tee left */
        *bits = (u8)((row >= 7 && row <= 8) ? 0xFF : 0x18);
        return (bool_t)1;
    case 0xB4:                    /* tee right */
        *bits = (u8)((row >= 7 && row <= 8) ? 0xFF : 0x18);
        return (bool_t)1;
    case 0xC5:                    /* cross */
        *bits = (u8)((row >= 7 && row <= 8) ? 0xFF : 0x18);
        return (bool_t)1;
    case 0xB0:                    /* light shade */
        *bits = (u8)((row & 1) ? 0xAA : 0x55);
        return (bool_t)1;
    case 0xB1:                    /* medium shade */
        *bits = (u8)((row & 1) ? 0xEE : 0x77);
        return (bool_t)1;
    case 0xB2:                    /* dark shade */
        *bits = (u8)((row & 1) ? 0xFF : 0xDD);
        return (bool_t)1;
    case 0xDB:                    /* full block */
        *bits = 0xFF;
        return (bool_t)1;
    case 0xDC:                    /* lower half block */
        *bits = (u8)(row >= 8 ? 0xFF : 0x00);
        return (bool_t)1;
    case 0xDD:                    /* upper half block */
        *bits = (u8)(row < 8 ? 0xFF : 0x00);
        return (bool_t)1;
    case 0xDE:                    /* upper one eighth */
        *bits = (u8)(row < 2 ? 0xFF : 0x00);
        return (bool_t)1;
    case 0xDF:                    /* lower one eighth */
        *bits = (u8)(row >= 14 ? 0xFF : 0x00);
        return (bool_t)1;
    case 0xFE:                    /* left half */
        *bits = 0xF0;
        return (bool_t)1;
    case 0xAF:                    /* right half */
        *bits = 0x0F;
        return (bool_t)1;
    default:
        break;
    }
    (void)top;
    (void)bottom;
    (void)vmid;
    return (bool_t)0;
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

static void put_glyph(gui_surface_t *surface, i32 x, i32 y, u8 ch, u32 colour,
                      u8 scale, gui_rect_t clip)
{
    i32 gw = axui_char_width(scale);
    i32 gh = axui_line_height(scale);

    if (x + gw <= clip.x || y + gh <= clip.y) {
        return;
    }
    if (x >= clip.x + clip.w || y >= clip.y + clip.h) {
        return;
    }

    for (i32 row = 0; row < 16; row++) {
        u8 bits = axui_glyph_row(ch, row);
        if (!bits) {
            u8 synth;
            if (axui_glyph_synthetic(ch, row, &synth)) {
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
            i32 px = x + col * (i32)scale;
            i32 py = y + row * (i32)scale;
            for (i32 sy = 0; sy < (i32)scale; sy++) {
                for (i32 sx = 0; sx < (i32)scale; sx++) {
                    i32 dx = px + sx;
                    i32 dy = py + sy;
                    if (dx < clip.x || dy < clip.y ||
                        dx >= clip.x + clip.w || dy >= clip.y + clip.h) {
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

void axui_draw_text(gui_surface_t *surface, i32 x, i32 y, const char *text,
                    axui_text_style_t style)
{
    gui_rect_t clip = gui_rect(0, 0, surface->width, surface->height);
    axui_draw_text_clip(surface, clip, x, y, text, style);
}

void axui_draw_text_clip(gui_surface_t *surface, gui_rect_t clip, i32 x, i32 y,
                         const char *text, axui_text_style_t style)
{
    if (!surface || !text) {
        return;
    }
    u8 scale = style.scale ? style.scale : 1;
    i32 advance = axui_char_width(scale);
    i32 pen = x;

    if (style.underline) {
        /* The underline is drawn after the run so it can span the whole
         * measured width, including trailing spaces. */
    }

    while (*text) {
        u32 code;
        i32 used = axui_utf8_decode(text, &code);
        u8 ch = (u8)(code < 0x100 ? code : '?');
        if (ch == '\t') {
            pen += advance * AXUI_TAB_WIDTH;
            text += used;
            continue;
        }
        if (ch == '\n') {
            break;
        }
        put_glyph(surface, pen, y, ch, style.colour, scale, clip);
        if (style.bold) {
            put_glyph(surface, pen + 1, y, ch, style.colour, scale, clip);
        }
        pen += advance;
        text += used;

        /* Cheap early-out: once the pen leaves the clip there is nothing
         * left to draw, so stop decoding. */
        if (pen >= clip.x + clip.w) {
            break;
        }
    }

    if (style.underline) {
        i32 end = pen;
        i32 uy = y + axui_line_height(scale) - 2;
        for (i32 px = x; px < end; px++) {
            if (px < clip.x || px >= clip.x + clip.w) {
                continue;
            }
            if (uy < clip.y || uy >= clip.y + clip.h) {
                continue;
            }
            u32 *p = &surface->pixels[(usize)uy * (usize)surface->stride + (usize)px];
            *p = axui_blend_alpha(*p, style.underline_colour, 255);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Measurement                                                         */
/* ------------------------------------------------------------------ */

i32 axui_measure_text(const char *text, u8 scale)
{
    if (!text) {
        return 0;
    }
    i32 advance = axui_char_width(scale);
    i32 width = 0;
    while (*text) {
        u32 code;
        i32 used = axui_utf8_decode(text, &code);
        if (code == '\n') {
            break;
        }
        if (code == '\t') {
            width += advance * AXUI_TAB_WIDTH;
        } else {
            width += advance;
        }
        text += used;
    }
    return width;
}

i32 axui_measure_text_bold(const char *text, u8 scale)
{
    i32 w = axui_measure_text(text, scale);
    if (w > 0) {
        w += 1;
    }
    return w;
}

/* Advance of a single character, used by the terminal grid. */
i32 axui_advance(char ch, u8 scale)
{
    if (ch == '\t') {
        return axui_char_width(scale) * AXUI_TAB_WIDTH;
    }
    return axui_char_width(scale);
}

/* ------------------------------------------------------------------ */
/* Wrapping                                                            */
/* ------------------------------------------------------------------ */

/* Enumerate candidate break points.  A break is legal after a space, after a
 * hyphen, or between any two characters when the caller allows hard breaks.
 * out_starts[i][0] is the byte offset of line i's first character and
 * out_starts[i][1] the offset one past its last.  Returns the number of lines. */
static i32 wrap_lines_impl(const char *text, i32 width_px, u8 scale,
                             i32 (*out_starts)[2], i32 *out_count, i32 max_lines,
                             bool_t hard_break)
{
    (void)hard_break;
    i32 advance = axui_char_width(scale);
    if (width_px < advance) {
        width_px = advance;
    }
    i32 max_cells = width_px / advance;
    if (max_cells < 1) {
        max_cells = 1;
    }

    i32 line = 0;
    usize offset = 0;
    usize len = ax_strlen(text);

    while (offset < len && line < max_lines) {
        usize start = offset;
        usize cells = 0;
        usize last_space = 0;
        bool_t have_space = (bool_t)0;
        usize cursor = offset;

        while (cursor < len) {
            char c = text[cursor];
            if (c == '\n') {
                break;
            }
            if (c == '\r') {
                cursor++;
                continue;
            }
            if (c == ' ' || c == '-') {
                last_space = cursor + 1;
                have_space = (bool_t)1;
            }
            if (cells >= (usize)max_cells) {
                break;
            }
            cells++;
            cursor++;
        }

        usize end = cursor;
        if (end < len && text[end] != '\n' && have_space && last_space > start) {
            /* Prefer the last whitespace break so words stay intact. */
            end = last_space;
        }
        /* Trim trailing spaces from the emitted line. */
        while (end > start && text[end - 1] == ' ') {
            end--;
        }

        if (out_starts) {
            out_starts[line][0] = (i32)start;
            out_starts[line][1] = (i32)end;
        }
        line++;

        offset = end;
        /* Skip the space we broke on and any run of spaces after it. */
        while (offset < len && (text[offset] == ' ' || text[offset] == '\r')) {
            offset++;
        }
        if (offset < len && text[offset] == '\n') {
            offset++;
        }
    }

    if (out_count) {
        *out_count = line;
    }
    return line;
}

void axui_wrap_lines(const char *text, i32 width_px, u8 scale,
                     i32 (*out_starts)[2], i32 *out_count, i32 max_lines)
{
    (void)wrap_lines_impl(text, width_px, scale, out_starts, out_count,
                                   max_lines, (bool_t)0);
}

/* Draw wrapped text inside a box; returns the height consumed.  Used by
 * dialogs, the about page and every tooltip. */
i32 axui_draw_text_wrapped(gui_surface_t *surface, gui_rect_t box, const char *text,
                           axui_text_style_t style)
{
    enum { AXUI_MAX_WRAP_LINES = 64 };
    i32 starts[AXUI_MAX_WRAP_LINES][2];
    i32 count = 0;
    u8 scale = style.scale ? style.scale : 1;
    i32 line_h = axui_line_height(scale);

    axui_wrap_lines(text, box.w, scale, starts, &count, AXUI_MAX_WRAP_LINES);

    i32 y = box.y;
    for (i32 i = 0; i < count; i++) {
        if (y + line_h > box.y + box.h) {
            break;
        }
        usize start = (usize)starts[i][0];
        usize end = (usize)starts[i][1];
        usize n = end > start ? end - start : 0;

        /* Draw a slice without copying: walk the original string. */
        const char *p = text + start;
        i32 pen = box.x;
        usize consumed = 0;
        while (consumed < n && *p) {
            u32 code;
            i32 used = axui_utf8_decode(p, &code);
            u8 ch = (u8)(code < 0x100 ? code : '?');
            if (ch == '\t') {
                pen += axui_char_width(scale) * AXUI_TAB_WIDTH;
            } else {
                put_glyph(surface, pen, y, ch, style.colour, scale, box);
                if (style.bold) {
                    put_glyph(surface, pen + 1, y, ch, style.colour, scale, box);
                }
                pen += axui_char_width(scale);
            }
            p += used;
            consumed += (usize)used;
        }
        if (style.underline) {
            i32 uy = y + line_h - 2;
            for (i32 px = box.x; px < pen; px++) {
                if (px < box.x || px >= box.x + box.w || uy >= box.y + box.h) {
                    continue;
                }
                u32 *pp = &surface->pixels[(usize)uy * (usize)surface->stride + (usize)px];
                *pp = axui_blend_alpha(*pp, style.underline_colour, 255);
            }
        }
        y += line_h;
    }
    return y - box.y;
}

/* Fit text into a box, replacing the tail with "..." when it does not. */
void axui_draw_text_ellipsis(gui_surface_t *surface, gui_rect_t box, const char *text,
                             axui_text_style_t style)
{
    u8 scale = style.scale ? style.scale : 1;
    i32 advance = axui_char_width(scale);

    if (axui_measure_text(text, scale) <= box.w) {
        axui_draw_text_clip(surface, box, box.x, box.y, text, style);
        return;
    }

    i32 fit = (box.w / advance) - 3;
    if (fit <= 0) {
        return;
    }
    axui_text_style_t plain = style;
    i32 pen = box.x;
    i32 drawn = 0;
    const char *p = text;
    while (*p && drawn < fit) {
        u32 code;
        i32 used = axui_utf8_decode(p, &code);
        u8 ch = (u8)(code < 0x100 ? code : '?');
        put_glyph(surface, pen, box.y, ch, plain.colour, scale, box);
        pen += advance;
        p += used;
        drawn++;
    }
    for (i32 i = 0; i < 3; i++) {
        put_glyph(surface, pen, box.y, '.', plain.colour, scale, box);
        pen += advance;
    }
}

void axui_draw_text_right(gui_surface_t *surface, gui_rect_t box, const char *text,
                          axui_text_style_t style)
{
    u8 scale = style.scale ? style.scale : 1;
    i32 w = style.bold ? axui_measure_text_bold(text, scale)
                       : axui_measure_text(text, scale);
    i32 x = box.x + box.w - w;
    if (x < box.x) {
        x = box.x;
    }
    axui_draw_text_clip(surface, box, x, box.y, text, style);
}

void axui_draw_text_center(gui_surface_t *surface, gui_rect_t box, const char *text,
                           axui_text_style_t style)
{
    u8 scale = style.scale ? style.scale : 1;
    i32 w = style.bold ? axui_measure_text_bold(text, scale)
                       : axui_measure_text(text, scale);
    i32 x = box.x + (box.w - w) / 2;
    if (x < box.x) {
        x = box.x;
    }
    axui_draw_text_clip(surface, box, x, box.y, text, style);
}

/* Draw a line already broken by the caller; keeps the vertical rhythm the
 * wrapped drawer uses. */
i32 axui_draw_line(gui_surface_t *surface, i32 x, i32 y, const char *text,
                   axui_text_style_t style)
{
    axui_draw_text(surface, x, y, text, style);
    return axui_line_height(style.scale);
}

/* ------------------------------------------------------------------ */
/* Selection and caret                                                 */
/* ------------------------------------------------------------------ */

void axui_draw_text_selection(gui_surface_t *surface, gui_rect_t line_rect,
                              i32 x0, i32 x1, u32 colour)
{
    if (x1 <= x0) {
        return;
    }
    if (x0 < line_rect.x) {
        x0 = line_rect.x;
    }
    if (x1 > line_rect.x + line_rect.w) {
        x1 = line_rect.x + line_rect.w;
    }
    for (i32 y = line_rect.y; y < line_rect.y + line_rect.h; y++) {
        for (i32 x = x0; x < x1; x++) {
            u32 *p = &surface->pixels[(usize)y * (usize)surface->stride + (usize)x];
            *p = axui_blend_alpha(*p, colour, 160);
        }
    }
}

/* A blinking caret: a solid bar with a soft one-pixel halo, which is more
 * visible on both light and dark fields than a one-pixel line. */
void axui_draw_caret(gui_surface_t *surface, i32 x, i32 y, i32 height, u8 scale,
                     u32 colour)
{
    i32 width = axui_char_width(scale) > 2 ? 2 : 1;
    for (i32 dy = 0; dy < height; dy++) {
        for (i32 dx = -1; dx <= width; dx++) {
            i32 px = x + dx;
            i32 py = y + dy;
            if (px < 0 || py < 0 || px >= surface->width || py >= surface->height) {
                continue;
            }
            u32 *p = &surface->pixels[(usize)py * (usize)surface->stride + (usize)px];
            u8 a = (u8)((dx >= 0 && dx < width) ? 255 : 60);
            *p = axui_blend_alpha(*p, colour, a);
        }
    }
}

/* Draw the I-beam caret used by the text field, at a character column. */
void axui_draw_caret_at(gui_surface_t *surface, gui_rect_t box, i32 column,
                        u8 scale, u32 colour, bool_t visible)
{
    if (!visible) {
        return;
    }
    i32 x = box.x + column * axui_char_width(scale);
    if (x < box.x || x >= box.x + box.w) {
        return;
    }
    axui_draw_caret(surface, x, box.y + 2, box.h - 4, scale, colour);
}

/* ------------------------------------------------------------------ */
/* Justified paragraph                                                 */
/* ------------------------------------------------------------------ */

/* Draw a paragraph with full justification except on the last line.  Spaces
 * are expanded by up to 3 extra pixels, which is enough for the widths the
 * about page and the tips panel use. */
i32 axui_draw_paragraph(gui_surface_t *surface, gui_rect_t box, const char *text,
                        axui_text_style_t style, bool_t justify)
{
    enum { AXUI_MAX_PARAGRAPH_LINES = 48 };
    i32 starts[AXUI_MAX_PARAGRAPH_LINES][2];
    i32 count = 0;
    u8 scale = style.scale ? style.scale : 1;
    i32 line_h = axui_line_height(scale);
    i32 advance = axui_char_width(scale);

    axui_wrap_lines(text, box.w, scale, starts, &count, AXUI_MAX_PARAGRAPH_LINES);

    i32 y = box.y;
    for (i32 i = 0; i < count; i++) {
        if (y + line_h > box.y + box.h) {
            break;
        }
        usize start = (usize)starts[i][0];
        usize end = (usize)starts[i][1];
        usize n = end > start ? end - start : 0;

        i32 spaces = 0;
        for (usize k = 0; k < n; k++) {
            if (text[start + k] == ' ') {
                spaces++;
            }
        }

        i32 extra_per_space = 0;
        if (justify && i + 1 < count && spaces > 0) {
            i32 used = (i32)n * advance;
            i32 slack = box.w - used;
            if (slack > 0) {
                extra_per_space = slack / spaces;
                if (extra_per_space > 3) {
                    extra_per_space = 3;
                }
            }
        }

        i32 pen = box.x;
        const char *p = text + start;
        usize consumed = 0;
        while (consumed < n && *p) {
            u32 code;
            i32 used = axui_utf8_decode(p, &code);
            u8 ch = (u8)(code < 0x100 ? code : '?');
            if (ch == '\t') {
                pen += advance * AXUI_TAB_WIDTH;
            } else {
                put_glyph(surface, pen, y, ch, style.colour, scale, box);
                pen += advance;
                if (ch == ' ') {
                    pen += extra_per_space;
                }
            }
            p += used;
            consumed += (usize)used;
        }
        y += line_h;
    }
    return y - box.y;
}

/* ------------------------------------------------------------------ */
/* Metrics helpers                                                     */
/* ------------------------------------------------------------------ */

/* Number of characters that fit in a pixel width. */
i32 axui_columns_for_width(i32 width_px, u8 scale)
{
    i32 advance = axui_char_width(scale);
    return advance > 0 ? width_px / advance : 0;
}

/* Number of rows that fit in a pixel height. */
i32 axui_rows_for_height(i32 height_px, u8 scale)
{
    i32 line_h = axui_line_height(scale);
    return line_h > 0 ? height_px / line_h : 0;
}

/* Vertical origin that centres one line of text in a box. */
i32 axui_text_baseline(gui_rect_t box, u8 scale)
{
    i32 line_h = axui_line_height(scale);
    return box.y + (box.h - line_h) / 2;
}

/* Horizontal origin that right-aligns a run inside a box. */
i32 axui_text_right_origin(gui_rect_t box, const char *text, u8 scale)
{
    i32 w = axui_measure_text(text, scale);
    i32 x = box.x + box.w - w;
    return x < box.x ? box.x : x;
}

/* Horizontal origin that centres a run inside a box. */
i32 axui_text_center_origin(gui_rect_t box, const char *text, u8 scale)
{
    i32 w = axui_measure_text(text, scale);
    i32 x = box.x + (box.w - w) / 2;
    return x < box.x ? box.x : x;
}

/* Truncate a string in place so that it fits a pixel width, appending "..."
 * when anything was removed.  The buffer must have room for three extra
 * bytes; this is the string-side twin of axui_draw_text_ellipsis. */
usize axui_text_truncate(char *buffer, usize cap, i32 width_px, u8 scale)
{
    if (!buffer || cap < 4) {
        return 0;
    }
    if (axui_measure_text(buffer, scale) <= width_px) {
        return ax_strlen(buffer);
    }
    i32 advance = axui_char_width(scale);
    i32 fit = width_px / advance - 3;
    if (fit < 1) {
        ax_strlcpy(buffer, ".", cap);
        return 1;
    }
    usize n = (usize)fit < cap - 4 ? (usize)fit : cap - 4;
    buffer[n] = '.';
    buffer[n + 1] = '.';
    buffer[n + 2] = '.';
    buffer[n + 3] = '\0';
    return n + 3;
}

/* Count the caret column for a byte offset; used by the editor when the
 * cursor moves horizontally through a multi-byte buffer. */
i32 axui_column_of_offset(const char *text, usize offset, u8 scale)
{
    i32 advance = axui_char_width(scale);
    if (advance <= 0) {
        return 0;
    }
    i32 width = 0;
    usize i = 0;
    while (i < offset && text[i]) {
        u32 code;
        i += (usize)axui_utf8_decode(text + i, &code);
        width += advance;
    }
    return width / advance;
}

/* Offset of the character at a pixel column, clamped to the string. */
usize axui_offset_at_column(const char *text, i32 column, u8 scale)
{
    (void)scale;
    usize offset = 0;
    i32 index = 0;
    while (text[offset] && index < column) {
        u32 code;
        offset += (usize)axui_utf8_decode(text + offset, &code);
        index++;
    }
    return offset;
}
