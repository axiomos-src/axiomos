/* AxiomOS terminal - colour and attribute resolution.
 *
 * Cells carry straight ARGB, so this module is the one place that decides how
 * an attribute combination turns into two pixels of colour:
 *
 *   - dim halves the contrast towards the cell background;
 *   - bold lifts the foreground, which is how the classic bold-is-bright
 *     behaviour is reproduced without a separate 16-colour palette;
 *   - reverse swaps the pair, including for the default colours;
 *   - hidden paints the foreground as the background.
 *
 * The 256-colour table is generated rather than stored: the 6x6x6 cube and the
 * 24-step grey ramp have closed forms.
 */

#include <axiom/lib.h>
#include "term.h"

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

/* The sixteen ANSI colours, built once on first use so they can be written as
 * ARGB calls instead of packed literals. */
static u32 ax_palette_table[16];
static bool_t ax_palette_ready = (bool_t)0;

static void palette_build(void)
{
    if (ax_palette_ready) {
        return;
    }
    ax_palette_table[0]  = axui_rgba(0x1B, 0x1F, 0x28, 255);  /* black */
    ax_palette_table[1]  = axui_rgba(0xE5, 0x4B, 0x5A, 255);  /* red */
    ax_palette_table[2]  = axui_rgba(0x3F, 0xB9, 0x50, 255);  /* green */
    ax_palette_table[3]  = axui_rgba(0xE0, 0xB3, 0x4A, 255);  /* yellow */
    ax_palette_table[4]  = axui_rgba(0x4A, 0x8C, 0xE8, 255);  /* blue */
    ax_palette_table[5]  = axui_rgba(0xB0, 0x6C, 0xD6, 255);  /* magenta */
    ax_palette_table[6]  = axui_rgba(0x3F, 0xB6, 0xC4, 255);  /* cyan */
    ax_palette_table[7]  = axui_rgba(0xC9, 0xD1, 0xDE, 255);  /* white */
    ax_palette_table[8]  = axui_rgba(0x5A, 0x62, 0x72, 255);  /* bright black */
    ax_palette_table[9]  = axui_rgba(0xFF, 0x6E, 0x7A, 255);
    ax_palette_table[10] = axui_rgba(0x64, 0xE0, 0x74, 255);
    ax_palette_table[11] = axui_rgba(0xFF, 0xD1, 0x66, 255);
    ax_palette_table[12] = axui_rgba(0x74, 0xAF, 0xFF, 255);
    ax_palette_table[13] = axui_rgba(0xD2, 0x92, 0xFF, 255);
    ax_palette_table[14] = axui_rgba(0x64, 0xDC, 0xE8, 255);
    ax_palette_table[15] = axui_rgba(0xF2, 0xF6, 0xFC, 255);
    ax_palette_ready = (bool_t)1;
}

u32 axterm_default_palette(i32 index)
{
    palette_build();
    if (index < 0) {
        index = 0;
    }
    if (index > 15) {
        index = 15;
    }
    return ax_palette_table[index];
}

/* The bright half of the ANSI set, used by SGR 90..97 and 100..107. */
u32 axterm_bright_palette(i32 index)
{
    palette_build();
    if (index < 0) {
        index = 0;
    }
    if (index > 7) {
        index = 7;
    }
    return ax_palette_table[8 + index];
}

/* xterm's 256-colour space: 16 system colours, a 6x6x6 cube and 24 greys.
 * The cube levels are 0, 95, 135, 175, 215, 255, the values xterm picked to
 * remain distinguishable on CRT displays. */
u32 axterm_xterm256(i32 index)
{
    palette_build();
    if (index < 0) {
        index = 0;
    }
    if (index > 255) {
        index = 255;
    }
    if (index < 16) {
        return ax_palette_table[index];
    }
    if (index < 232) {
        i32 cube = index - 16;
        i32 r = cube / 36;
        i32 g = (cube / 6) % 6;
        i32 b = cube % 6;
        static const u8 levels[6] = { 0, 95, 135, 175, 215, 255 };
        return axui_rgba(levels[r], levels[g], levels[b], 255);
    }
    i32 grey = index - 232;
    u8 value = (u8)(8 + grey * 10);
    return axui_rgba(value, value, value, 255);
}

/* Override one palette entry.  Cells already on screen keep their colour;
 * the change applies to output from this point on, matching how a real
 * terminal treats OSC 4. */
void axterm_set_colour(term_t *term, i32 index, u32 colour)
{
    if (!term || index < 0 || index >= 256) {
        return;
    }
    term->palette[index] = colour;
    term->dirty = (bool_t)1;
}

/* Resolve an SGR colour index through the terminal's own palette, falling
 * back to the built-in table when the slot has never been written. */
u32 axterm_palette_lookup(const term_t *term, i32 index)
{
    if (term && index >= 0 && index < 256 && term->palette[index]) {
        return term->palette[index];
    }
    return axterm_xterm256(index);
}

/* ------------------------------------------------------------------ */
/* Attribute resolution                                                */
/* ------------------------------------------------------------------ */

u32 axterm_effective_fg(const term_t *term, const term_cell_t *cell)
{
    if (!term || !cell) {
        return 0;
    }
    u32 fg = cell->fg ? cell->fg : term->default_fg;
    u32 bg = cell->bg ? cell->bg : term->default_bg;

    if (cell->flags & TERM_A_HIDDEN) {
        return bg;
    }
    if (cell->flags & TERM_A_REVERSE) {
        return bg;
    }
    if (cell->flags & TERM_A_BOLD) {
        /* Lift the foreground by 30 % towards white, which reads as the
         * classic bright variant without changing hue. */
        fg = axui_lerp_colour(fg, axui_rgba(255, 255, 255, 255), 30);
    }
    if (cell->flags & TERM_A_DIM) {
        fg = axui_lerp_colour(fg, bg, 40);
    }
    return fg;
}

u32 axterm_effective_bg(const term_t *term, const term_cell_t *cell)
{
    if (!term || !cell) {
        return 0;
    }
    u32 bg = cell->bg ? cell->bg : term->default_bg;
    u32 fg = cell->fg ? cell->fg : term->default_fg;

    if (cell->flags & TERM_A_REVERSE) {
        return fg;
    }
    if (cell->flags & TERM_A_HIDDEN) {
        return bg;
    }
    return bg;
}

/* Build a cell carrying the current attribute set.  Used by the shell so its
 * coloured output goes through exactly the same path as parsed output. */
term_cell_t axterm_make_cell(term_t *term, u32 ch, u8 extra_flags)
{
    term_cell_t cell;
    cell.ch = (u16)ch;
    cell.fg = term ? term->cursor.fg : 0;
    cell.bg = term ? term->cursor.bg : 0;
    cell.flags = (u8)((term ? term->cursor.flags : 0) | extra_flags);
    cell.wide = 0;
    return cell;
}

/* ------------------------------------------------------------------ */
/* Nominal colours named by the shell                                  */
/* ------------------------------------------------------------------ */

u32 axterm_named(term_t *term, const char *name)
{
    if (!name || !*name) {
        return term ? term->default_fg : 0xFFFFFFFFu;
    }
    if (ax_strcasecmp(name, "default") == 0) {
        return term ? term->default_fg : 0xFFFFFFFFu;
    }
    if (ax_strcasecmp(name, "black") == 0) {
        return axterm_palette_lookup(term, 0);
    }
    if (ax_strcasecmp(name, "red") == 0) {
        return axterm_palette_lookup(term, 1);
    }
    if (ax_strcasecmp(name, "green") == 0) {
        return axterm_palette_lookup(term, 2);
    }
    if (ax_strcasecmp(name, "yellow") == 0) {
        return axterm_palette_lookup(term, 3);
    }
    if (ax_strcasecmp(name, "blue") == 0) {
        return axterm_palette_lookup(term, 4);
    }
    if (ax_strcasecmp(name, "magenta") == 0) {
        return axterm_palette_lookup(term, 5);
    }
    if (ax_strcasecmp(name, "cyan") == 0) {
        return axterm_palette_lookup(term, 6);
    }
    if (ax_strcasecmp(name, "white") == 0) {
        return axterm_palette_lookup(term, 7);
    }
    if (ax_strcasecmp(name, "grey") == 0 || ax_strcasecmp(name, "gray") == 0) {
        return axterm_palette_lookup(term, 8);
    }
    if (ax_strcasecmp(name, "accent") == 0) {
        return axui_theme_current()->accent;
    }
    return axui_parse_colour(name);
}

/* A short mnemonic for the current attribute set, shown in the status bar. */
const char *axterm_attribute_name(const term_t *term)
{
    if (!term) {
        return "-";
    }
    u8 flags = term->cursor.flags;
    if (flags & TERM_A_BOLD) {
        return "bold";
    }
    if (flags & TERM_A_DIM) {
        return "dim";
    }
    if (flags & TERM_A_REVERSE) {
        return "reverse";
    }
    if (flags & TERM_A_UNDERLINE) {
        return "underline";
    }
    return "normal";
}

/* ------------------------------------------------------------------ */
/* Colour distance used by the ANSI fallback                           */
/* ------------------------------------------------------------------ */

/* When a true-colour sequence has to be reproduced on a palette-limited
 * renderer (the boot console mirror), pick the nearest entry by squared
 * distance in RGB.  Keeping this here means the console and the window agree
 * on which entry is "the same" colour. */
i32 axterm_nearest_palette(u32 colour, const u32 *palette, i32 count)
{
    if (!palette || count <= 0) {
        return 0;
    }
    i32 best = 0;
    i64 best_distance = (i64)1 << 62;
    for (i32 i = 0; i < count; i++) {
        i32 dr = (i32)((colour >> 16) & 0xFF) - (i32)((palette[i] >> 16) & 0xFF);
        i32 dg = (i32)((colour >> 8) & 0xFF) - (i32)((palette[i] >> 8) & 0xFF);
        i32 db = (i32)(colour & 0xFF) - (i32)(palette[i] & 0xFF);
        i64 d = (i64)dr * dr + (i64)dg * dg + (i64)db * db;
        if (d < best_distance) {
            best_distance = d;
            best = i;
        }
    }
    return best;
}
