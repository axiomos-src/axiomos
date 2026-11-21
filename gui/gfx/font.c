/*
 * AxiomOS - bitmap text rendering.
 *
 * The font table is generated at build time by tools/mkgui_font.py from
 * DejaVu Sans Mono.  Each glyph is eight columns wide and sixteen rows tall,
 * one byte per row with the most significant bit being the leftmost pixel.
 */

#include <gui.h>

i32 gui_text_width(const char *text, u8 scale)
{
    if (!text)
        return 0;
    i32 count = 0;
    while (text[count])
        count++;
    return count * gui_font_width * (scale ? scale : 1);
}

static void draw_glyph(gui_surface_t *surface, i32 x, i32 y, char c, u32 colour,
                       u8 scale)
{
    u8 code = (u8)c;
    if (code < gui_font_first || code > gui_font_last)
        code = '?';
    const u8 *glyph = gui_font8x16[code - gui_font_first];

    for (i32 row = 0; row < gui_font_height; row++) {
        u8 bits = glyph[row];
        if (!bits)
            continue;
        for (i32 column = 0; column < gui_font_width; column++) {
            if (!(bits & (0x80 >> column)))
                continue;
            if (scale == 1) {
                i32 px = x + column;
                i32 py = y + row;
                if (px < 0 || py < 0 || px >= surface->width || py >= surface->height)
                    continue;
                surface->pixels[(u64)py * surface->stride + px] = colour;
            } else {
                gui_fill_rect(surface, gui_rect(x + column * scale,
                                                y + row * scale,
                                                scale, scale), colour);
            }
        }
    }
}

void gui_text(gui_surface_t *surface, i32 x, i32 y, const char *text, u32 colour,
              u8 scale)
{
    if (!surface || !text)
        return;
    if (scale == 0)
        scale = 1;
    i32 advance = gui_font_width * scale;
    i32 origin = x;
    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor == '\n') {
            x = origin;
            y += gui_font_height * scale + 2;
            continue;
        }
        draw_glyph(surface, x, y, *cursor, colour, scale);
        x += advance;
    }
}

void gui_text_shadow(gui_surface_t *surface, i32 x, i32 y, const char *text,
                     u32 colour, u32 shadow, u8 scale)
{
    gui_text(surface, x + 1, y + 1, text, shadow, scale);
    gui_text(surface, x, y, text, colour, scale);
}

void gui_text_clipped(gui_surface_t *surface, gui_rect_t clip, i32 x, i32 y,
                      const char *text, u32 colour, u8 scale)
{
    if (!surface || !text)
        return;
    if (scale == 0)
        scale = 1;
    i32 advance = gui_font_width * scale;
    for (const char *cursor = text; *cursor; cursor++) {
        i32 px = x;
        if (px + advance > clip.x && px < clip.x + clip.w &&
            y + gui_font_height * scale > clip.y && y < clip.y + clip.h)
            draw_glyph(surface, px, y, *cursor, colour, scale);
        x += advance;
        if (x > clip.x + clip.w)
            break;
    }
}
