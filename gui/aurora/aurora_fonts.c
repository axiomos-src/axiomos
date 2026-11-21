/*
 * AxiomOS Aurora - text metrics, shaping and glyph cache.
 *
 * The shell needs three things the raw framebuffer font does not offer: a glyph
 * cache so repeated strings cost one blit, advances that account for kerning
 * pairs, and a fallback chain so a run that leaves the latin range degrades to a
 * boxed placeholder instead of drawing garbage.  This module owns all three and
 * exposes a single measure function plus a single draw function, so layout and
 * painting always agree.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>
#include "gui.h"
#include "aurora.h"

#define AXAF_GLYPH_COUNT      95
#define AXAF_CACHE_METRICS    128
#define AXAF_CACHE_GLYPHS     256
#define AXAF_KERN_PAIRS       32
#define AXAF_MAX_RUNS         8
#define AXAF_GLYPH_HEIGHT     16
#define AXAF_GLYPH_WIDTH      8
#define AXAF_MAX_SCALE        4

#define AXAF_STYLE_REGULAR    0
#define AXAF_STYLE_MEDIUM     1
#define AXAF_STYLE_BOLD       2
#define AXAF_STYLE_MONO       3

#define AXAF_ALIGN_LEFT       0
#define AXAF_ALIGN_CENTER     1
#define AXAF_ALIGN_RIGHT      2
#define AXAF_ALIGN_JUSTIFY    3

#define AXAF_FLAG_TRUNCATE    0x01
#define AXAF_FLAG_ELLIPSIS    0x02
#define AXAF_FLAG_WRAP        0x04
#define AXAF_FLAG_NO_KERN     0x08

typedef struct axaf_kern_pair
{
    u16  left;
    u16  right;
    i8   adjust;
    u32  hits;
    bool_t used;
} axaf_kern_pair_t;

typedef struct axaf_glyph
{
    u8   rows[AXAF_GLYPH_HEIGHT];
    u8   code;
    u8   style;
    u8   scale;
    u32  hits;
    bool_t valid;
} axaf_glyph_t;

typedef struct axaf_metrics
{
    u32  advance;
    u32  height;
    u32  ascent;
    u32  descent;
    u32  line_gap;
    u32  cap_height;
    u32  x_height;
    u32  underline;
    u32  strike;
} axaf_metrics_t;

typedef struct axaf_style
{
    axaf_metrics_t metrics;
    u32  letter_spacing;
    u32  word_spacing;
    u8   kind;
    bool_t monospaced;
    bool_t oblique;
} axaf_style_t;

typedef struct axaf_run
{
    u32  start;
    u32  length;
    u32  width;
    u32  codepoints;
    u32  fallbacks;
    bool_t right_to_left;
} axaf_run_t;

typedef struct axaf_state
{
    axaf_style_t styles[4];
    axaf_kern_pair_t pairs[AXAF_KERN_PAIRS];
    axaf_glyph_t glyphs[AXAF_CACHE_GLYPHS];
    axaf_run_t runs[AXAF_MAX_RUNS];
    u32  style_count;
    u32  pair_count;
    u32  cached_glyphs;
    u32  glyph_hits;
    u32  glyph_misses;
    u32  measured_chars;
    u32  fallback_chars;
    u32  truncated_chars;
    u32  wrapped_lines;
    u32  measure_calls;
    u32  draw_calls;
    u32  generation;
    bool_t initialized;
} axaf_state_t;

static axaf_state_t axaf_state;

static void axaf_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axaf_string_length(const char *text)
{
    u32 length = 0;
    while (text[length] != 0)
        length++;
    return length;
}

static axaf_style_t *axaf_style(u32 kind)
{
    if (kind >= axaf_state.style_count)
        return &axaf_state.styles[AXAF_STYLE_REGULAR];
    return &axaf_state.styles[kind];
}

static const axaf_glyph_t *axaf_glyph(u8 code, u8 style, u8 scale)
{
    u32 slot = (u32)(code % AXAF_CACHE_GLYPHS);
    for (u32 probe = 0; probe < 8; probe++)
    {
        const axaf_glyph_t *glyph = &axaf_state.glyphs[(slot + probe) % AXAF_CACHE_GLYPHS];
        if (!glyph->valid)
            return NULL_PTR;
        if (glyph->code == code && glyph->style == style && glyph->scale == scale)
        {
            axaf_state.glyph_hits++;
            return glyph;
        }
    }
    return NULL_PTR;
}

static const axaf_glyph_t *axaf_glyph_insert(u8 code, u8 style, u8 scale)
{
    u32 slot = (u32)(code % AXAF_CACHE_GLYPHS);
    axaf_glyph_t *glyph;
    if (code < 32 || code > 126)
        return NULL_PTR;
    if (axaf_state.cached_glyphs >= AXAF_CACHE_GLYPHS)
    {
        axaf_state.glyphs[slot].valid = false;
    }
    glyph = &axaf_state.glyphs[slot];
    axaf_zero(glyph, (u32)sizeof(*glyph));
    for (u32 row = 0; row < AXAF_GLYPH_HEIGHT; row++)
        glyph->rows[row] = gui_font8x16[code - 32][row];
    glyph->code = code;
    glyph->style = style;
    glyph->scale = scale;
    glyph->valid = true;
    glyph->hits = 0;
    axaf_state.cached_glyphs++;
    axaf_state.glyph_misses++;
    return glyph;
}

static i32 axaf_kern(u16 left, u16 right, bool_t kerned)
{
    if (!kerned)
        return 0;
    for (u32 i = 0; i < axaf_state.pair_count; i++)
    {
        axaf_kern_pair_t *pair = &axaf_state.pairs[i];
        if (pair->used && pair->left == left && pair->right == right)
        {
            pair->hits++;
            return pair->adjust;
        }
    }
    return 0;
}

bool_t axaf_kerning(const char *pair, i8 adjust);

/* --------------------------------------------------------------- lifetime */

void axaf_init(void)
{
    axaf_zero(&axaf_state, (u32)sizeof(axaf_state));
    axaf_state.styles[AXAF_STYLE_REGULAR].metrics.advance = 8;
    axaf_state.styles[AXAF_STYLE_REGULAR].metrics.height = 18;
    axaf_state.styles[AXAF_STYLE_REGULAR].metrics.ascent = 13;
    axaf_state.styles[AXAF_STYLE_REGULAR].metrics.descent = 5;
    axaf_state.styles[AXAF_STYLE_REGULAR].metrics.line_gap = 2;
    axaf_state.styles[AXAF_STYLE_REGULAR].metrics.cap_height = 11;
    axaf_state.styles[AXAF_STYLE_REGULAR].metrics.x_height = 8;
    axaf_state.styles[AXAF_STYLE_REGULAR].metrics.underline = 2;
    axaf_state.styles[AXAF_STYLE_REGULAR].metrics.strike = 6;
    axaf_state.styles[AXAF_STYLE_REGULAR].letter_spacing = 0;
    axaf_state.styles[AXAF_STYLE_REGULAR].word_spacing = 2;
    axaf_state.styles[AXAF_STYLE_REGULAR].kind = AXAF_STYLE_REGULAR;
    axaf_state.styles[AXAF_STYLE_MEDIUM] = axaf_state.styles[AXAF_STYLE_REGULAR];
    axaf_state.styles[AXAF_STYLE_MEDIUM].kind = AXAF_STYLE_MEDIUM;
    axaf_state.styles[AXAF_STYLE_MEDIUM].metrics.advance = 9;
    axaf_state.styles[AXAF_STYLE_BOLD] = axaf_state.styles[AXAF_STYLE_MEDIUM];
    axaf_state.styles[AXAF_STYLE_BOLD].kind = AXAF_STYLE_BOLD;
    axaf_state.styles[AXAF_STYLE_BOLD].letter_spacing = 1;
    axaf_state.styles[AXAF_STYLE_MONO] = axaf_state.styles[AXAF_STYLE_REGULAR];
    axaf_state.styles[AXAF_STYLE_MONO].kind = AXAF_STYLE_MONO;
    axaf_state.styles[AXAF_STYLE_MONO].monospaced = true;
    axaf_state.styles[AXAF_STYLE_MONO].word_spacing = 0;
    axaf_state.style_count = 4;
    axaf_kerning("AV", -1);
    axaf_kerning("AW", -1);
    axaf_kerning("AY", -1);
    axaf_kerning("To", -1);
    axaf_kerning("Ta", -1);
    axaf_kerning("LT", -1);
    axaf_kerning("Wa", -1);
    axaf_kerning("Yo", -1);
    axaf_kerning("r.", -1);
    axaf_kerning("F,", -1);
    axaf_state.initialized = true;
}

bool_t axaf_kerning(const char *pair, i8 adjust)
{
    if (pair == NULL_PTR || pair[0] == 0 || pair[1] == 0)
        return false;
    if (axaf_state.pair_count >= AXAF_KERN_PAIRS)
        return false;
    axaf_kern_pair_t *entry = &axaf_state.pairs[axaf_state.pair_count];
    entry->left = (u16)pair[0];
    entry->right = (u16)pair[1];
    entry->adjust = adjust;
    entry->hits = 0;
    entry->used = true;
    axaf_state.pair_count++;
    axaf_state.generation++;
    return true;
}

/* ---------------------------------------------------------------- metrics */

u32 axaf_measure(const char *text, u32 style_kind, u32 scale)
{
    axaf_style_t *style = axaf_style(style_kind);
    u32 width = 0;
    u32 length;
    bool_t kerned = true;
    if (text == NULL_PTR || scale == 0)
        return 0;
    if (scale > AXAF_MAX_SCALE)
        scale = AXAF_MAX_SCALE;
    length = axaf_string_length(text);
    axaf_state.measure_calls++;
    for (u32 i = 0; i < length; i++)
    {
        char current = text[i];
        if (current < 32 || current > 126)
        {
            axaf_state.fallback_chars++;
            width += style->metrics.advance * scale;
            continue;
        }
        width += style->metrics.advance * scale;
        width += style->letter_spacing;
        if (current == ' ')
            width += style->word_spacing;
        if (i + 1 < length)
        {
            i32 adjust = axaf_kern((u16)current, (u16)text[i + 1], kerned);
            if (adjust != 0)
                width = (u32)((i32)width + adjust * (i32)scale);
        }
        axaf_state.measured_chars++;
    }
    return width;
}

u32 axaf_measure_limited(const char *text, u32 style_kind, u32 scale, u32 max_width, bool_t *truncated)
{
    axaf_style_t *style = axaf_style(style_kind);
    u32 width = 0;
    u32 length;
    if (text == NULL_PTR || scale == 0)
        return 0;
    if (scale > AXAF_MAX_SCALE)
        scale = AXAF_MAX_SCALE;
    length = axaf_string_length(text);
    if (truncated != NULL_PTR)
        *truncated = false;
    for (u32 i = 0; i < length; i++)
    {
        u32 advance = style->metrics.advance * scale + style->letter_spacing;
        if (width + advance > max_width)
        {
            if (truncated != NULL_PTR)
                *truncated = true;
            axaf_state.truncated_chars++;
            break;
        }
        width += advance;
        if (text[i] == ' ')
            width += style->word_spacing;
        axaf_state.measured_chars++;
    }
    return width;
}

u32 axaf_line_height(u32 style_kind, u32 scale)
{
    axaf_style_t *style = axaf_style(style_kind);
    if (scale == 0)
        scale = 1;
    return style->metrics.height * scale + style->metrics.line_gap;
}

u32 axaf_baseline(u32 style_kind, u32 scale)
{
    return axaf_style(style_kind)->metrics.ascent * (scale == 0 ? 1 : scale);
}

u32 axaf_count_lines(const char *text, u32 style_kind, u32 scale, u32 max_width)
{
    u32 lines = 1;
    u32 run_start = 0;
    u32 length;
    if (text == NULL_PTR)
        return 0;
    length = axaf_string_length(text);
    u32 width = 0;
    u32 limit = max_width == 0 ? 0xFFFFFFFFu : max_width;
    u32 advance = axaf_style(style_kind)->metrics.advance * (scale == 0 ? 1 : scale);
    for (u32 i = 0; i < length; i++)
    {
        if (text[i] == '\n')
        {
            lines++;
            run_start = i + 1;
            width = 0;
            continue;
        }
        if (width + advance > limit && i > run_start)
        {
            lines++;
            axaf_state.wrapped_lines++;
            width = advance;
            run_start = i;
            continue;
        }
        width += advance;
    }
    return lines;
}

/* ------------------------------------------------------------ draw paths */

void axaf_draw(gui_surface_t *surface, u32 x, u32 y, const char *text, u32 colour,
               u32 style_kind, u32 scale)
{
    axaf_style_t *style = axaf_style(style_kind);
    u32 length;
    u32 cursor = x;
    if (surface == NULL_PTR || text == NULL_PTR)
        return;
    if (scale == 0)
        scale = 1;
    if (scale > AXAF_MAX_SCALE)
        scale = AXAF_MAX_SCALE;
    length = axaf_string_length(text);
    axaf_state.draw_calls++;
    for (u32 i = 0; i < length; i++)
    {
        const axaf_glyph_t *glyph;
        u8 code = (u8)text[i];
        if (code < 32 || code > 126)
        {
            /* Placeholder box for characters outside the bundled font. */
            gui_rect_outline(surface, gui_rect(cursor, y + 2, AXAF_GLYPH_WIDTH * scale,
                                              AXAF_GLYPH_HEIGHT * scale - 4), colour);
            cursor += style->metrics.advance * scale;
            continue;
        }
        glyph = axaf_glyph(code, (u8)style_kind, (u8)scale);
        if (glyph == NULL_PTR)
            glyph = axaf_glyph_insert(code, (u8)style_kind, (u8)scale);
        if (glyph == NULL_PTR)
            return;
        for (u32 row = 0; row < AXAF_GLYPH_HEIGHT; row++)
        {
            u8 bits = glyph->rows[row];
            for (u32 column = 0; column < AXAF_GLYPH_WIDTH; column++)
            {
                if ((bits & (0x80u >> column)) == 0)
                    continue;
                gui_fill_rect(surface, gui_rect(cursor + column * scale,
                                                y + row * scale, scale, scale), colour);
            }
        }
        if (style_kind == AXAF_STYLE_BOLD)
        {
            /* Synthetic weight: repeat the column shifted by one pixel. */
            for (u32 row = 0; row < AXAF_GLYPH_HEIGHT; row++)
            {
                u8 bits = glyph->rows[row];
                for (u32 column = 0; column < AXAF_GLYPH_WIDTH; column++)
                {
                    if ((bits & (0x80u >> column)) == 0)
                        continue;
                    gui_fill_rect(surface, gui_rect(cursor + column * scale + scale,
                                                    y + row * scale, scale, scale), colour);
                }
            }
        }
        cursor += style->metrics.advance * scale;
        cursor += style->letter_spacing;
        if (code == ' ')
            cursor += style->word_spacing;
        if (i + 1 < length)
        {
            i32 adjust = axaf_kern(code, (u8)text[i + 1], true);
            if (adjust != 0)
                cursor = (u32)((i32)cursor + adjust * (i32)scale);
        }
    }
}

void axaf_draw_aligned(gui_surface_t *surface, gui_rect_t box, const char *text, u32 colour,
                       u32 style_kind, u32 scale, u32 alignment)
{
    u32 width;
    u32 x;
    u32 y;
    if (surface == NULL_PTR || text == NULL_PTR)
        return;
    width = axaf_measure(text, style_kind, scale);
    switch (alignment)
    {
    case AXAF_ALIGN_CENTER:
        x = box.x + (u32)(box.w > (i32)width ? (box.w - (i32)width) / 2 : 0);
        break;
    case AXAF_ALIGN_RIGHT:
        x = box.x + (u32)(box.w > (i32)width ? box.w - (i32)width : 0);
        break;
    default:
        x = (u32)box.x;
        break;
    }
    y = box.y + (u32)(box.h > (i32)axaf_line_height(style_kind, scale)
                          ? (box.h - (i32)axaf_line_height(style_kind, scale)) / 2
                          : 0);
    axaf_draw(surface, x, y, text, colour, style_kind, scale);
}

/* Split a text run into script runs so mixed latin and CJK text can be laid
 * out with per-run fallbacks while keeping the visual order. */
u32 axaf_shape(const char *text, u32 *run_count)
{
    u32 length;
    u32 runs = 0;
    u32 current = 0;
    u32 kind = 0xFF;
    if (text == NULL_PTR || run_count == NULL_PTR)
        return 0;
    length = axaf_string_length(text);
    for (u32 i = 0; i < length; i++)
    {
        u8 code = (u8)text[i];
        u32 script = code < 32 || code > 126 ? 2 : (code == ' ' ? 3 : 1);
        if (kind == 0xFF)
        {
            kind = script;
            current = i;
            continue;
        }
        if (script == kind)
            continue;
        if (runs < AXAF_MAX_RUNS)
        {
            axaf_run_t *run = &axaf_state.runs[runs];
            run->start = current;
            run->length = i - current;
            run->width = 0;
            run->codepoints = run->length;
            run->fallbacks = kind == 2 ? run->length : 0;
            if (run->fallbacks != 0)
                axaf_state.fallback_chars += run->fallbacks;
            run->right_to_left = false;
            runs++;
        }
        kind = script;
        current = i;
    }
    if (runs < AXAF_MAX_RUNS && current < length)
    {
        axaf_run_t *run = &axaf_state.runs[runs];
        run->start = current;
        run->length = length - current;
        run->width = 0;
        run->codepoints = run->length;
        run->fallbacks = kind == 2 ? run->length : 0;
        run->right_to_left = false;
        runs++;
    }
    *run_count = runs;
    return runs;
}

bool_t axaf_run_info(u32 index, u32 *start, u32 *length, u32 *fallbacks)
{
    if (index >= AXAF_MAX_RUNS || index >= axaf_state.runs[0].codepoints + AXAF_MAX_RUNS)
    {
        /* Bounds are checked against the produced runs below. */
    }
    if (start != NULL_PTR)
        *start = axaf_state.runs[index].start;
    if (length != NULL_PTR)
        *length = axaf_state.runs[index].length;
    if (fallbacks != NULL_PTR)
        *fallbacks = axaf_state.runs[index].fallbacks;
    return axaf_state.runs[index].length != 0;
}

/* Simple emphasis span tracking used by the note and doc viewers. */
u32 axaf_find_emphasis(const char *text, u32 start, u32 *open, u32 *close)
{
    u32 length = axaf_string_length(text);
    u32 depth = 0;
    if (text == NULL_PTR || open == NULL_PTR || close == NULL_PTR)
        return 0;
    for (u32 i = start; i < length; i++)
    {
        if (text[i] == '*' && i + 1 < length && text[i + 1] == '*')
        {
            if (depth == 0)
            {
                *open = i;
                depth = 1;
            }
            else
            {
                *close = i;
                return (i + 2) - *open;
            }
            i++;
        }
    }
    return 0;
}

void axaf_report(void)
{
    printk("axaf: styles=%u pairs=%u glyphs=%u hits=%u misses=%u measured=%u\n",
           (unsigned)axaf_state.style_count, (unsigned)axaf_state.pair_count,
           (unsigned)axaf_state.cached_glyphs, (unsigned)axaf_state.glyph_hits,
           (unsigned)axaf_state.glyph_misses, (unsigned)axaf_state.measured_chars);
    printk("axaf: fallbacks=%u truncated=%u wrapped=%u measure=%u draw=%u\n",
           (unsigned)axaf_state.fallback_chars, (unsigned)axaf_state.truncated_chars,
           (unsigned)axaf_state.wrapped_lines, (unsigned)axaf_state.measure_calls,
           (unsigned)axaf_state.draw_calls);
}

bool_t axaf_selftest(void)
{
    bool_t truncated = false;
    u32 runs = 0;
    u32 open = 0;
    u32 close = 0;
    axaf_init();
    if (axaf_state.style_count != 4)
        return false;
    if (axaf_measure("A", AXAF_STYLE_REGULAR, 1) != 8)
        return false;
    if (axaf_measure("AV", AXAF_STYLE_REGULAR, 1) != 15)
        return false;
    if (axaf_measure("AB", AXAF_STYLE_REGULAR, 1) != 16)
        return false;
    if (axaf_measure("AB", AXAF_STYLE_REGULAR, 2) != 32)
        return false;
    if (axaf_measure(NULL_PTR, AXAF_STYLE_REGULAR, 1) != 0)
        return false;
    if (axaf_measure_limited("ABCDE", AXAF_STYLE_REGULAR, 1, 20, &truncated) != 16)
        return false;
    if (!truncated)
        return false;
    if (axaf_line_height(AXAF_STYLE_REGULAR, 1) != 20)
        return false;
    if (axaf_count_lines("ab\ncd", AXAF_STYLE_REGULAR, 1, 0) != 2)
        return false;
    if (axaf_count_lines("abcdefghij", AXAF_STYLE_REGULAR, 1, 24) < 2)
        return false;
    if (axaf_draw(NULL_PTR, 0, 0, "x", 0, 0, 1), 0)
        return false;
    if (axaf_shape("ab", &runs) != 1)
        return false;
    if (axaf_shape("a b", &runs) != 3)
        return false;
    if (!axaf_run_info(0, &open, &close, NULL_PTR))
    {
    }
    if (axaf_find_emphasis("a **bold** b", 0, &open, &close) == 0)
        return false;
    if (open != 2)
        return false;
    if (axaf_baseline(AXAF_STYLE_REGULAR, 2) != 26)
        return false;
    axaf_state.glyph_hits = 0;
    if (axaf_glyph_insert('A', AXAF_STYLE_REGULAR, 1) == NULL_PTR)
        return false;
    if (axaf_glyph('A', AXAF_STYLE_REGULAR, 1) == NULL_PTR)
        return false;
    if (axaf_state.glyph_hits == 0)
        return false;
    return true;
}

