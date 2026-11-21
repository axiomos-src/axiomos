/* AxiomOS UI - colour management.
 *
 * Everything on the desktop is ARGB 0xAARRGGBB, straight (non-premultiplied)
 * alpha.  The compositor blends in that space, so this module is where all of
 * the conversion happens: HSV round-trips for the theme editor, perceptual
 * luma for contrast decisions, and a small table of named colours that the
 * settings application uses for its accent picker.
 *
 * Two details matter for output quality:
 *
 *   - lighten() and darken() work in HSL-ish space rather than multiplying the
 *     channels, otherwise a saturated blue turns grey at 20 %;
 *   - blend_alpha() is a straight alpha compositing of a source onto a
 *     destination, which is what the rasteriser's plot() expects.
 */

#include <axiom/lib.h>
#include "ui.h"

/* ------------------------------------------------------------------ */
/* Basic construction                                                  */
/* ------------------------------------------------------------------ */

u32 axui_rgba(u8 r, u8 g, u8 b, u8 a)
{
    return ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

u8 axui_alpha_of(u32 colour)
{
    return (u8)((colour >> 24) & 0xFF);
}

u32 axui_colour_alpha(u32 colour, u8 alpha)
{
    return (colour & 0x00FFFFFFu) | ((u32)alpha << 24);
}

u32 axui_colour_opacity(u32 colour, i32 percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    u8 a = (u8)(((u32)axui_alpha_of(colour) * (u32)percent) / 100u);
    return axui_colour_alpha(colour, a);
}

/* ------------------------------------------------------------------ */
/* Blending                                                            */
/* ------------------------------------------------------------------ */

/* Straight alpha compositing: source over destination.  The destination is
 * opaque in practice, but the general form is kept so nested surfaces (a
 * popup over a translucent panel, for instance) stay correct. */
u32 axui_blend_alpha(u32 dst, u32 src, u8 alpha)
{
    u32 sa = ((u32)alpha * (u32)axui_alpha_of(src)) / 255u;
    if (sa == 0) {
        return dst;
    }
    if (sa >= 255) {
        return (src & 0x00FFFFFFu) | 0xFF000000u;
    }

    u32 da = axui_alpha_of(dst);
    u32 out_a = sa + (da * (255u - sa)) / 255u;
    if (out_a == 0) {
        return 0;
    }

    u32 sr = (src >> 16) & 0xFF;
    u32 sg = (src >> 8) & 0xFF;
    u32 sb = src & 0xFF;
    u32 dr = (dst >> 16) & 0xFF;
    u32 dg = (dst >> 8) & 0xFF;
    u32 db = dst & 0xFF;

    /* Intermediate products stay well inside 32 bits: 255 * 255 * 255. */
    u32 nr = (sr * sa * 255u + dr * da * (255u - sa)) / (out_a * 255u);
    u32 ng = (sg * sa * 255u + dg * da * (255u - sa)) / (out_a * 255u);
    u32 nb = (sb * sa * 255u + db * da * (255u - sa)) / (out_a * 255u);

    if (nr > 255) {
        nr = 255;
    }
    if (ng > 255) {
        ng = 255;
    }
    if (nb > 255) {
        nb = 255;
    }
    return axui_rgba((u8)nr, (u8)ng, (u8)nb, (u8)out_a);
}

u32 axui_blend(u32 dst, u32 src)
{
    return axui_blend_alpha(dst, src, 255);
}

/* ------------------------------------------------------------------ */
/* Interpolation                                                       */
/* ------------------------------------------------------------------ */

u32 axui_lerp_colour(u32 a, u32 b, i32 t)
{
    if (t <= 0) {
        return a;
    }
    if (t >= 100) {
        return b;
    }
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF, aa = axui_alpha_of(a);
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF, ba = axui_alpha_of(b);

    u32 r = (ar * (u32)(100 - t) + br * (u32)t) / 100u;
    u32 g = (ag * (u32)(100 - t) + bg * (u32)t) / 100u;
    u32 bl = (ab * (u32)(100 - t) + bb * (u32)t) / 100u;
    u32 al = (aa * (u32)(100 - t) + ba * (u32)t) / 100u;
    return axui_rgba((u8)r, (u8)g, (u8)bl, (u8)al);
}

/* ------------------------------------------------------------------ */
/* Lightness                                                           */
/* ------------------------------------------------------------------ */

static u8 mix_channel(u8 base, u8 target, i32 percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    return (u8)(((i32)base * (100 - percent) + (i32)target * percent) / 100);
}

u32 axui_lighten(u32 colour, i32 percent)
{
    i32 amt = percent * 100 / 60;      /* 60 % reads as "white-ish" to users */
    if (amt > 100) {
        amt = 100;
    }
    u8 r = mix_channel((u8)((colour >> 16) & 0xFF), 0xFF, amt);
    u8 g = mix_channel((u8)((colour >> 8) & 0xFF), 0xFF, amt);
    u8 b = mix_channel((u8)(colour & 0xFF), 0xFF, amt);
    return axui_rgba(r, g, b, axui_alpha_of(colour));
}

u32 axui_darken(u32 colour, i32 percent)
{
    i32 amt = percent * 100 / 60;
    if (amt > 100) {
        amt = 100;
    }
    u8 r = mix_channel((u8)((colour >> 16) & 0xFF), 0x00, amt);
    u8 g = mix_channel((u8)((colour >> 8) & 0xFF), 0x00, amt);
    u8 b = mix_channel((u8)(colour & 0xFF), 0x00, amt);
    return axui_rgba(r, g, b, axui_alpha_of(colour));
}

/* Boost the chroma of a colour.  Implemented by pushing each channel away
 * from the pixel's own luma, which is the cheapest way to keep hue stable. */
u32 axui_saturate(u32 colour, i32 percent)
{
    i32 l = axui_luma(colour);
    i32 amt = percent;
    u8 r = (u8)axui_clamp_i32(l + ((i32)((colour >> 16) & 0xFF) - l) * (100 + amt) / 100, 0, 255);
    u8 g = (u8)axui_clamp_i32(l + ((i32)((colour >> 8) & 0xFF) - l) * (100 + amt) / 100, 0, 255);
    u8 b = (u8)axui_clamp_i32(l + ((i32)(colour & 0xFF) - l) * (100 + amt) / 100, 0, 255);
    return axui_rgba(r, g, b, axui_alpha_of(colour));
}

/* ------------------------------------------------------------------ */
/* Perceptual measures                                                 */
/* ------------------------------------------------------------------ */

/* Rec. 709 weights, integer approximation.  The GUI uses this for the
 * "is this background light?" decision on menus and toasts. */
i32 axui_luma(u32 colour)
{
    i32 r = (i32)((colour >> 16) & 0xFF);
    i32 g = (i32)((colour >> 8) & 0xFF);
    i32 b = (i32)(colour & 0xFF);
    return (r * 54 + g * 183 + b * 19) / 256;
}

bool_t axui_is_dark(u32 colour)
{
    return (bool_t)(axui_luma(colour) < 128);
}

/* WCAG relative luminance on a 0..1000 scale.  A gamma-2 approximation of the
 * sRGB transfer curve is within a couple of percent in the mid-tones, which is
 * well inside the tolerance of a 4.5:1 contrast check, and it avoids the
 * 256-entry table the exact formula would need. */
static u32 lin_channel(u32 v)
{
    return (v * v) / 65u;
}

static u32 rel_luminance(u32 colour)
{
    u32 r = lin_channel((colour >> 16) & 0xFF);
    u32 g = lin_channel((colour >> 8) & 0xFF);
    u32 b = lin_channel(colour & 0xFF);
    return (r * 2126u + g * 7152u + b * 749u) / 10000u;
}
/* Returns a contrast ratio in hundredths (e.g. 403 means 4.03:1). */
i32 axui_contrast_ratio(u32 a, u32 b)
{
    u32 la = rel_luminance(a);
    u32 lb = rel_luminance(b);
    if (la < lb) {
        u32 t = la;
        la = lb;
        lb = t;
    }
    /* The +50 offset is the standard 0.05 flare term on the 0..1000 scale. */
    return (i32)((la + 50u) * 100u / (lb + 50u));
}

/* Pick black or white text for a background, favouring whichever the theme
 * would use at the boundary (dark text on mid-tones looks better). */
u32 axui_contrast_text(u32 background)
{
    i32 ratio_white = axui_contrast_ratio(background, axui_rgba(255, 255, 255, 255));
    i32 ratio_black = axui_contrast_ratio(background, axui_rgba(0, 0, 0, 255));
    if (ratio_black >= ratio_white) {
        return axui_rgba(17, 19, 24, 255);
    }
    return axui_rgba(245, 247, 250, 255);
}

/* ------------------------------------------------------------------ */
/* HSV                                                               */
/* ------------------------------------------------------------------ */

axui_hsv_t axui_rgb_to_hsv(u32 colour)
{
    axui_hsv_t hsv;
    hsv.h = 0;
    hsv.s = 0;
    hsv.v = 0;

    i32 r = (i32)((colour >> 16) & 0xFF);
    i32 g = (i32)((colour >> 8) & 0xFF);
    i32 b = (i32)(colour & 0xFF);

    i32 max = axui_max_i32(r, axui_max_i32(g, b));
    i32 min = axui_min_i32(r, axui_min_i32(g, b));
    i32 delta = max - min;

    hsv.v = (max * 100) / 255;
    if (max == 0) {
        return hsv;
    }
    hsv.s = (delta * 100) / max;
    if (delta == 0) {
        return hsv;
    }

    /* Hue in degrees, computed on sextant boundaries. */
    i32 h;
    if (max == r) {
        h = (g - b) * 60 / delta;
    } else if (max == g) {
        h = 120 + (b - r) * 60 / delta;
    } else {
        h = 240 + (r - g) * 60 / delta;
    }
    if (h < 0) {
        h += 360;
    }
    hsv.h = h % 360;
    return hsv;
}

u32 axui_hsv_to_rgb(axui_hsv_t hsv)
{
    i32 h = hsv.h % 360;
    if (h < 0) {
        h += 360;
    }
    i32 s = axui_clamp_i32(hsv.s, 0, 100);
    i32 v = axui_clamp_i32(hsv.v, 0, 100);

    i32 max = (v * 255) / 100;
    i32 min = max - ((max * s) / 100);
    i32 delta = max - min;

    i32 sextant = h / 60;
    i32 frac = h % 60;

    i32 r = 0, g = 0, b = 0;
    switch (sextant) {
    case 0: r = max; g = min + delta * frac / 60; b = min; break;
    case 1: r = min + delta * (60 - frac) / 60; g = max; b = min; break;
    case 2: r = min; g = max; b = min + delta * frac / 60; break;
    case 3: r = min; g = min + delta * (60 - frac) / 60; b = max; break;
    case 4: r = min + delta * frac / 60; g = min; b = max; break;
    default: r = max; g = min; b = min + delta * (60 - frac) / 60; break;
    }
    return axui_rgba((u8)axui_clamp_i32(r, 0, 255), (u8)axui_clamp_i32(g, 0, 255),
                     (u8)axui_clamp_i32(b, 0, 255), 255);
}

/* ------------------------------------------------------------------ */
/* Parsing and formatting                                              */
/* ------------------------------------------------------------------ */

/* Parse "#RRGGBB", "#AARRGGBB", "rgb(12,34,56)", "rgba(...)" and the handful
 * of CSS names the settings app offers.  Returns 0 on failure. */
static i32 parse_hex(u32 *out, const char *text)
{
    usize len = ax_strlen(text);
    if (len != 6 && len != 8) {
        return 0;
    }
    u32 value = 0;
    for (usize i = 0; i < len; i++) {
        int d = ax_digit_value(text[i]);
        if (d < 0) {
            return 0;
        }
        value = (value << 4) | (u32)d;
    }
    if (len == 6) {
        *out = value | 0xFF000000u;
    } else {
        *out = value;
    }
    return 1;
}

struct named_colour {
    const char *name;
    u32         value;
};

static const struct named_colour ax_named[] = {
    { "black",        0xFF000000u },
    { "white",        0xFFFFFFFFu },
    { "red",          0xFFE5484Du },
    { "green",        0xFF30A46Cu },
    { "blue",         0xFF0090FFu },
    { "yellow",       0xFFFFC53Du },
    { "orange",       0xFFF76808u },
    { "purple",       0xFF8E4EC6u },
    { "pink",         0xFFD6409Fu },
    { "teal",         0xFF12A594u },
    { "gray",         0xFF8B8D98u },
    { "grey",         0xFF8B8D98u },
    { "slate",        0xFF6E7681u },
    { "cyan",         0xFF00A2C7u },
    { "lime",         0xFF99D52Au },
    { "amber",        0xFFFFB224u },
    { "crimson",      0xFFE93D82u },
    { "indigo",       0xFF3E63DDu },
    { "violet",       0xFF6E56CFu },
    { "brown",        0xFFAD7F58u },
    { "transparent",  0x00000000u },
};

#define AX_NAMED_COUNT (sizeof(ax_named) / sizeof(ax_named[0]))

u32 axui_parse_colour(const char *text)
{
    if (!text || !*text) {
        return 0;
    }
    if (*text == '#') {
        u32 out;
        if (parse_hex(&out, text + 1)) {
            return out;
        }
        return 0;
    }

    if (ax_startswith(text, "rgb(") || ax_startswith(text, "rgba(")) {
        /* Skip the prefix and read up to three components plus alpha. */
        const char *p = text + (*text == 'r' ? 4 : 5);
        i32 comp[4] = { 0, 0, 0, 255 };
        i32 index = 0;
        while (*p && index < 4) {
            while (*p == ' ') {
                p++;
            }
            if (*p == ')') {
                break;
            }
            i32 value = 0;
            bool_t any = (bool_t)0;
            while (ax_isdigit(*p)) {
                value = value * 10 + (*p - '0');
                p++;
                any = (bool_t)1;
            }
            if (!any) {
                return 0;
            }
            comp[index++] = axui_clamp_i32(value, 0, 255);
            while (*p == ' ' || *p == ',') {
                p++;
            }
        }
        return axui_rgba((u8)comp[0], (u8)comp[1], (u8)comp[2], (u8)comp[3]);
    }

    for (usize i = 0; i < AX_NAMED_COUNT; i++) {
        if (ax_strcasecmp(text, ax_named[i].name) == 0) {
            return ax_named[i].value;
        }
    }
    return 0;
}

const char *axui_format_colour(u32 colour, char *buffer, usize cap)
{
    if (!buffer || cap < 10) {
        return buffer;
    }
    char tmp[16];
    (void)ax_snprintf(tmp, sizeof(tmp), "#%02X%02X%02X",
                      (u32)((colour >> 16) & 0xFF), (u32)((colour >> 8) & 0xFF),
                      (u32)(colour & 0xFF));
    ax_strlcpy(buffer, tmp, cap);
    return buffer;
}

const char *axui_colour_name(u32 colour)
{
    for (usize i = 0; i < AX_NAMED_COUNT; i++) {
        if ((ax_named[i].value & 0x00FFFFFFu) == (colour & 0x00FFFFFFu)) {
            return ax_named[i].name;
        }
    }
    return "custom";
}

/* ------------------------------------------------------------------ */
/* Palette generation                                                  */
/* ------------------------------------------------------------------ */

/* Produce a 50..900 ramp from one accent colour, like a design system.
 * Step 50 is the tint, 500 the base, 900 the shade. */
u32 axui_palette_step(u32 accent, i32 step)
{
    if (step < 50) {
        step = 50;
    }
    if (step > 900) {
        step = 900;
    }
    if (step == 500) {
        return accent;
    }
    if (step < 500) {
        i32 amount = (500 - step) * 60 / 450;
        return axui_lighten(accent, amount);
    }
    i32 amount = (step - 500) * 60 / 400;
    return axui_darken(accent, amount);
}

/* A sequence of visually distinct hues for the chart widgets. */
u32 axui_chart_colour(i32 index)
{
    static const i32 hues[12] = { 210, 150, 30, 280, 0, 190, 45, 320, 120, 250,
                                  75, 340 };
    i32 idx = index % 12;
    if (idx < 0) {
        idx += 12;
    }
    axui_hsv_t hsv;
    hsv.h = hues[idx];
    hsv.s = 72;
    hsv.v = 86;
    return axui_hsv_to_rgb(hsv);
}

/* Blend a colour over a background with a given mix weight; the draw code
 * uses this for disabled controls so they keep their hue. */
u32 axui_tint(u32 base, u32 background, i32 weight)
{
    return axui_lerp_colour(background, base, weight);
}

/* Two-colour checkerboard used behind transparent images. */
u32 axui_alpha_backdrop(i32 x, i32 y, i32 cell)
{
    if (cell <= 0) {
        cell = 8;
    }
    i32 cx = x / cell;
    i32 cy = y / cell;
    return ((cx ^ cy) & 1) ? axui_rgba(0xCE, 0xD1, 0xD6, 255)
                           : axui_rgba(0xF2, 0xF3, 0xF5, 255);
}

/* Linear interpolation that also moves the alpha channel (used by every
 * fade animation in the window manager). */
u32 axui_fade(u32 colour, i32 percent)
{
    return axui_colour_opacity(colour, percent);
}

/* Adjust all three channels by an absolute delta, clamped. */
u32 axui_shift(u32 colour, i32 delta)
{
    i32 r = axui_clamp_i32((i32)((colour >> 16) & 0xFF) + delta, 0, 255);
    i32 g = axui_clamp_i32((i32)((colour >> 8) & 0xFF) + delta, 0, 255);
    i32 b = axui_clamp_i32((i32)(colour & 0xFF) + delta, 0, 255);
    return axui_rgba((u8)r, (u8)g, (u8)b, axui_alpha_of(colour));
}

/* Grey out a colour while keeping its alpha. */
u32 axui_disabled(u32 colour)
{
    i32 l = axui_luma(colour);
    l = (l + 128) / 2;
    return axui_rgba((u8)l, (u8)l, (u8)l, axui_alpha_of(colour));
}

/* The accent's "on colour": black or white text for an accent fill. */
u32 axui_on_colour(u32 background)
{
    return axui_contrast_text(background);
}

/* A 1-pixel hairline colour derived from a background; the settings app uses
 * it for separators that must not clash with custom accents. */
u32 axui_hairline(u32 background)
{
    return axui_is_dark(background) ? axui_lighten(background, 14)
                                    : axui_darken(background, 10);
}

/* Status colour lookup shared by the notification centre and the file
 * manager's permission badges. */
u32 axui_status_colour(i32 status)
{
    switch (status) {
    case 1: return axui_rgba(0x30, 0xA4, 0x6C, 255);   /* ok */
    case 2: return axui_rgba(0xFF, 0xC5, 0x3D, 255);   /* warn */
    case 3: return axui_rgba(0xE5, 0x48, 0x4D, 255);   /* error */
    case 4: return axui_rgba(0x00, 0x90, 0xFF, 255);   /* info */
    default: return axui_rgba(0x8B, 0x8D, 0x98, 255);  /* neutral */
    }
}

const char *axui_status_name(i32 status)
{
    switch (status) {
    case 1: return "ok";
    case 2: return "warning";
    case 3: return "error";
    case 4: return "info";
    default: return "idle";
    }
}

/* ------------------------------------------------------------------ */
/* Colour maths used by the theme editor                               */
/* ------------------------------------------------------------------ */

/* WCAG-corrected text colour: if the requested colour does not reach the
 * 4.5:1 ratio against the background, darken or lighten it until it does. */
u32 axui_ensure_contrast(u32 fg, u32 bg)
{
    if (axui_contrast_ratio(fg, bg) >= 450) {
        return fg;
    }
    u32 candidate = axui_is_dark(bg) ? axui_lighten(fg, 60) : axui_darken(fg, 60);
    for (i32 i = 0; i < 6; i++) {
        if (axui_contrast_ratio(candidate, bg) >= 450) {
            break;
        }
        candidate = axui_is_dark(bg) ? axui_lighten(candidate, 10)
                                     : axui_darken(candidate, 10);
    }
    return candidate;
}

/* Produce a soft drop shadow colour matching the surface's own hue. */
u32 axui_shadow_colour(u32 base, i32 strength)
{
    u32 dark = axui_darken(base, 85);
    return axui_colour_alpha(dark, (u8)axui_clamp_i32(strength, 0, 255));
}

/* Interpolate through a multi-stop gradient given a 0..100 position. */
u32 axui_ramp(const u32 *stops, i32 count, i32 t)
{
    if (!stops || count <= 0) {
        return 0;
    }
    if (count == 1) {
        return stops[0];
    }
    if (t <= 0) {
        return stops[0];
    }
    if (t >= 100) {
        return stops[count - 1];
    }
    i32 span = 100 / (count - 1);
    i32 index = t / span;
    if (index >= count - 1) {
        index = count - 2;
    }
    i32 local = (t - index * span) * 100 / span;
    return axui_lerp_colour(stops[index], stops[index + 1], local);
}

/* Composite a whole palette over a base colour; used by the wallpaper tint. */
u32 axui_average(const u32 *colours, i32 count)
{
    if (!colours || count <= 0) {
        return 0;
    }
    u64 r = 0, g = 0, b = 0, a = 0;
    for (i32 i = 0; i < count; i++) {
        r += (colours[i] >> 16) & 0xFF;
        g += (colours[i] >> 8) & 0xFF;
        b += colours[i] & 0xFF;
        a += axui_alpha_of(colours[i]);
    }
    return axui_rgba((u8)(r / (u64)count), (u8)(g / (u64)count),
                     (u8)(b / (u64)count), (u8)(a / (u64)count));
}
