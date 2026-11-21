/*
 * AxiomOS - Aurora core: tokens, palettes, motion and materials.
 *
 * Everything below is deterministic and allocation-free.  Colours are plain
 * 0xAARRGGBB values; there is no colour management, because the framebuffer is
 * already sRGB on every platform we boot on.  If a future panel needs a wider
 * gamut the conversion belongs in the compositor, not here.
 *
 * The palettes are hand-tuned rather than generated from a single accent: an
 * automated ramp produces a muddy surface stack on dark backgrounds, and the
 * surface steps below are the ones that actually read as separate layers on a
 * 96 dpi panel.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

static axau_context_t g_context;

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

axau_context_t *axau_context(void)
{
    return &g_context;
}

void axau_init(i32 dpi, u32 accent, axau_theme_kind_t kind)
{
    ax_memset(&g_context, 0, sizeof(g_context));
    axau_metrics_init(&g_context.metrics, dpi);
    axau_set_theme(kind, accent);
    g_context.initialised = (bool_t)1;
}

void axau_set_theme(axau_theme_kind_t kind, u32 accent)
{
    switch (kind) {
    case AXAU_THEME_DARK:
        axau_palette_dark(&g_context.palette, accent);
        break;
    case AXAU_THEME_HIGH_CONTRAST:
        axau_palette_high_contrast(&g_context.palette);
        break;
    case AXAU_THEME_LIGHT:
    default:
        axau_palette_light(&g_context.palette, accent);
        break;
    }
    g_context.high_contrast = (bool_t)(kind == AXAU_THEME_HIGH_CONTRAST);
}

void axau_set_reduced_motion(bool_t reduced)
{
    g_context.reduced_motion = reduced;
}

void axau_begin_frame(i32 pointer_x, i32 pointer_y)
{
    g_context.pointer_x = pointer_x;
    g_context.pointer_y = pointer_y;
    g_context.frame++;
}

void axau_end_frame(void)
{
    /* Reserved for damage flushing once the design layer owns a back buffer. */
}

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

void axau_metrics_init(axau_metrics_t *metrics, i32 dpi)
{
    if (!metrics) {
        return;
    }
    ax_memset(metrics, 0, sizeof(*metrics));
    if (dpi <= 0) {
        dpi = 96;
    }
    metrics->dpi = dpi;
    metrics->scale_percent = (dpi * 100) / 96;
    metrics->density = AXAU_DENSITY_COMFORTABLE;
    metrics->type.caption = 1;
    metrics->type.body = (u8)(dpi >= 144 ? 2 : 1);
    metrics->type.body_strong = metrics->type.body;
    metrics->type.subtitle = (u8)(dpi >= 144 ? 2 : 1);
    metrics->type.title = (u8)(dpi >= 120 ? 2 : 1);
    metrics->type.title_large = 2;
    metrics->type.display = 3;
    metrics->row_height = AXAU_ROW_HEIGHT;
    metrics->control_height = AXAU_CONTROL_HEIGHT;
    metrics->radius_control = AXAU_RADIUS_CONTROL;
    metrics->radius_card = AXAU_RADIUS_CARD;
    metrics->radius_flyout = AXAU_RADIUS_FLYOUT;
    metrics->stroke = AXAU_STROKE;

    /* DPI scale is applied in steps, not continuously: a 4 px control radius
     * scaled by 137 percent would land on 5.48 and produce inconsistent
     * antialiasing between controls on the same row. */
    if (metrics->scale_percent >= 200) {
        metrics->row_height = AXAU_ROW_HEIGHT * 2;
        metrics->control_height = AXAU_CONTROL_HEIGHT * 2;
        metrics->radius_control = 8;
        metrics->radius_card = 16;
        metrics->radius_flyout = 24;
        metrics->stroke = 2;
    } else if (metrics->scale_percent >= 150) {
        metrics->row_height = 60;
        metrics->control_height = 40;
        metrics->radius_control = 6;
        metrics->radius_card = 12;
        metrics->radius_flyout = 18;
    } else if (metrics->scale_percent >= 125) {
        metrics->row_height = 52;
        metrics->control_height = 36;
        metrics->radius_control = 5;
        metrics->radius_card = 10;
        metrics->radius_flyout = 15;
    }
}

void axau_metrics_set_density(axau_metrics_t *metrics, axau_density_t density)
{
    if (!metrics) {
        return;
    }
    metrics->density = density;
    switch (density) {
    case AXAU_DENSITY_COMPACT:
        metrics->row_height = AXAU_ROW_COMPACT;
        metrics->control_height = 28;
        break;
    case AXAU_DENSITY_TOUCH:
        metrics->row_height = 56;
        metrics->control_height = 40;
        break;
    case AXAU_DENSITY_COMFORTABLE:
    default:
        metrics->row_height = AXAU_ROW_HEIGHT;
        metrics->control_height = AXAU_CONTROL_HEIGHT;
        break;
    }
}

i32 axau_dp(const axau_metrics_t *metrics, i32 value)
{
    if (!metrics) {
        return value;
    }
    return (value * metrics->scale_percent + 50) / 100;
}

i32 axau_dp_inverse(const axau_metrics_t *metrics, i32 pixels)
{
    if (!metrics || metrics->scale_percent == 0) {
        return pixels;
    }
    return (pixels * 100) / metrics->scale_percent;
}

/* ------------------------------------------------------------------ */
/* Palettes                                                            */
/* ------------------------------------------------------------------ */

/* Alpha is 0xFF for every opaque colour; translucent surfaces use explicit
 * alpha values and are composited by the paint helpers. */

void axau_palette_light(axau_palette_t *palette, u32 accent)
{
    if (!palette) return;
    ax_memset(palette, 0, sizeof(*palette));
    palette->kind = AXAU_THEME_LIGHT;
    ax_strlcpy(palette->name, "Aurora Light", sizeof(palette->name));
    if (accent == 0) {
        accent = GUI_RGB(0x00, 0x67, 0xC0);
    }
    palette->accent = accent;
    palette->colours[AXAU_COLOUR_TEXT] = GUI_RGB(0x1A, 0x1A, 0x1A);
    palette->colours[AXAU_COLOUR_TEXT_SECONDARY] = GUI_RGB(0x5C, 0x5C, 0x5C);
    palette->colours[AXAU_COLOUR_TEXT_TERTIARY] = GUI_RGB(0x8A, 0x8A, 0x8A);
    palette->colours[AXAU_COLOUR_TEXT_DISABLED] = GUI_RGB(0xA6, 0xA6, 0xA6);
    palette->colours[AXAU_COLOUR_ACCENT] = accent;
    palette->colours[AXAU_COLOUR_ACCENT_HOVER] = axui_lighten(accent, 8);
    palette->colours[AXAU_COLOUR_ACCENT_PRESSED] = axui_darken(accent, 12);
    palette->colours[AXAU_COLOUR_ACCENT_TEXT] = GUI_RGB(0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_SURFACE] = GUI_ARGB(0xF2, 0xF9, 0xF9, 0xF9);
    palette->colours[AXAU_COLOUR_SURFACE_ALT] = GUI_ARGB(0xFF, 0xFC, 0xFC, 0xFC);
    palette->colours[AXAU_COLOUR_SURFACE_ELEVATED] = GUI_ARGB(0xFF, 0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_CONTROL] = GUI_ARGB(0x0D, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_CONTROL_HOVER] = GUI_ARGB(0x14, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_CONTROL_PRESSED] = GUI_ARGB(0x1F, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_CONTROL_DISABLED] = GUI_ARGB(0x0A, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_STROKE] = GUI_ARGB(0x1A, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_STROKE_STRONG] = GUI_ARGB(0x33, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_DIVIDER] = GUI_ARGB(0x12, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_SHADOW] = GUI_ARGB(0x24, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_SUCCESS] = GUI_RGB(0x0F, 0x7B, 0x0F);
    palette->colours[AXAU_COLOUR_WARNING] = GUI_RGB(0x9D, 0x5D, 0x00);
    palette->colours[AXAU_COLOUR_ERROR] = GUI_RGB(0xC4, 0x2B, 0x1C);
    palette->colours[AXAU_COLOUR_FOCUS] = accent;
}

void axau_palette_dark(axau_palette_t *palette, u32 accent)
{
    if (!palette) return;
    ax_memset(palette, 0, sizeof(*palette));
    palette->kind = AXAU_THEME_DARK;
    ax_strlcpy(palette->name, "Aurora Dark", sizeof(palette->name));
    if (accent == 0) {
        accent = GUI_RGB(0x4C, 0xC2, 0xFF);
    }
    palette->accent = accent;
    palette->colours[AXAU_COLOUR_TEXT] = GUI_RGB(0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_TEXT_SECONDARY] = GUI_RGB(0xC8, 0xC8, 0xC8);
    palette->colours[AXAU_COLOUR_TEXT_TERTIARY] = GUI_RGB(0x9A, 0x9A, 0x9A);
    palette->colours[AXAU_COLOUR_TEXT_DISABLED] = GUI_RGB(0x6A, 0x6A, 0x6A);
    palette->colours[AXAU_COLOUR_ACCENT] = accent;
    palette->colours[AXAU_COLOUR_ACCENT_HOVER] = axui_lighten(accent, 10);
    palette->colours[AXAU_COLOUR_ACCENT_PRESSED] = axui_darken(accent, 10);
    palette->colours[AXAU_COLOUR_ACCENT_TEXT] = GUI_RGB(0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_SURFACE] = GUI_ARGB(0xF2, 0x1F, 0x1F, 0x1F);
    palette->colours[AXAU_COLOUR_SURFACE_ALT] = GUI_ARGB(0xFF, 0x2A, 0x2A, 0x2A);
    palette->colours[AXAU_COLOUR_SURFACE_ELEVATED] = GUI_ARGB(0xFF, 0x32, 0x32, 0x32);
    palette->colours[AXAU_COLOUR_CONTROL] = GUI_ARGB(0x14, 0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_CONTROL_HOVER] = GUI_ARGB(0x1F, 0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_CONTROL_PRESSED] = GUI_ARGB(0x08, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_CONTROL_DISABLED] = GUI_ARGB(0x0A, 0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_STROKE] = GUI_ARGB(0x1A, 0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_STROKE_STRONG] = GUI_ARGB(0x33, 0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_DIVIDER] = GUI_ARGB(0x12, 0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_SHADOW] = GUI_ARGB(0x60, 0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_SUCCESS] = GUI_RGB(0x6C, 0xCB, 0x5F);
    palette->colours[AXAU_COLOUR_WARNING] = GUI_RGB(0xF5, 0xB1, 0x4E);
    palette->colours[AXAU_COLOUR_ERROR] = GUI_RGB(0xFF, 0x6B, 0x5E);
    palette->colours[AXAU_COLOUR_FOCUS] = accent;
}

void axau_palette_high_contrast(axau_palette_t *palette)
{
    if (!palette) return;
    ax_memset(palette, 0, sizeof(*palette));
    palette->kind = AXAU_THEME_HIGH_CONTRAST;
    ax_strlcpy(palette->name, "Aurora Contrast", sizeof(palette->name));
    palette->accent = GUI_RGB(0x00, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_TEXT] = GUI_RGB(0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_TEXT_SECONDARY] = GUI_RGB(0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_TEXT_TERTIARY] = GUI_RGB(0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_TEXT_DISABLED] = GUI_RGB(0x66, 0x66, 0x66);
    palette->colours[AXAU_COLOUR_ACCENT] = GUI_RGB(0x00, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_ACCENT_HOVER] = GUI_RGB(0x80, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_ACCENT_PRESSED] = GUI_RGB(0x00, 0xCC, 0xCC);
    palette->colours[AXAU_COLOUR_ACCENT_TEXT] = GUI_RGB(0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_SURFACE] = GUI_RGB(0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_SURFACE_ALT] = GUI_RGB(0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_SURFACE_ELEVATED] = GUI_RGB(0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_CONTROL] = GUI_RGB(0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_CONTROL_HOVER] = GUI_RGB(0x00, 0x3A, 0x3A);
    palette->colours[AXAU_COLOUR_CONTROL_PRESSED] = GUI_RGB(0x00, 0x55, 0x55);
    palette->colours[AXAU_COLOUR_CONTROL_DISABLED] = GUI_RGB(0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_STROKE] = GUI_RGB(0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_STROKE_STRONG] = GUI_RGB(0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_DIVIDER] = GUI_RGB(0xFF, 0xFF, 0xFF);
    palette->colours[AXAU_COLOUR_SHADOW] = GUI_RGB(0x00, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_SUCCESS] = GUI_RGB(0x00, 0xFF, 0x00);
    palette->colours[AXAU_COLOUR_WARNING] = GUI_RGB(0xFF, 0xFF, 0x00);
    palette->colours[AXAU_COLOUR_ERROR] = GUI_RGB(0xFF, 0x00, 0x00);
    palette->colours[AXAU_COLOUR_FOCUS] = GUI_RGB(0xFF, 0xFF, 0xFF);
}

u32 axau_colour(const axau_palette_t *palette, axau_colour_role_t role)
{
    if (!palette || role >= AXAU_COLOUR_COUNT) {
        return GUI_RGB(0xFF, 0x00, 0xFF);
    }
    return palette->colours[role];
}

u32 axau_with_accent(const axau_palette_t *palette, u32 accent)
{
    if (!palette) return accent;
    return accent ? accent : palette->accent;
}

const char *axau_theme_name(axau_theme_kind_t kind)
{
    switch (kind) {
    case AXAU_THEME_DARK: return "dark";
    case AXAU_THEME_HIGH_CONTRAST: return "contrast";
    case AXAU_THEME_CUSTOM: return "custom";
    case AXAU_THEME_LIGHT:
    default: return "light";
    }
}

/* ------------------------------------------------------------------ */
/* Motion                                                              */
/* ------------------------------------------------------------------ */

i32 axau_ease_out(i32 t)
{
    if (t <= 0) return 0;
    if (t >= 1000) return 1000;
    /* Cubic ease-out: fast start, long settle.  Chosen over a quadratic
     * because the settle is what makes a flyout feel like it landed. */
    i32 one_minus = 1000 - t;
    i64 cube = (i64)one_minus * one_minus * one_minus;
    return (i32)(1000 - cube / 1000000);
}

i32 axau_ease_in_out(i32 t)
{
    if (t <= 0) return 0;
    if (t >= 1000) return 1000;
    if (t < 500) {
        return (i32)((i64)2 * t * t / 1000);
    }
    i32 inverse = 1000 - t;
    return (i32)(1000 - ((i64)2 * inverse * inverse / 1000));
}

i32 axau_ease_overshoot(i32 t)
{
    if (t <= 0) return 0;
    if (t >= 1000) return 1000;
    /* A slight overshoot, capped at 1060 and then clamped to 1000 by callers
     * that need a strict range.  Used only for small controls, never panels. */
    i32 eased = axau_ease_out(t);
    i32 overshoot = (i32)((i64)eased * eased / 100000);
    i32 value = eased + overshoot / 4;
    return value > 1060 ? 1060 : value;
}

i32 axau_curve_apply(axau_curve_t curve, i32 t)
{
    switch (curve) {
    case AXAU_CURVE_ACCELERATE:
        if (t <= 0) return 0;
        if (t >= 1000) return 1000;
        return (i32)((i64)t * t / 1000);
    case AXAU_CURVE_EASE:
        return axau_ease_in_out(t);
    case AXAU_CURVE_OVERSHOOT:
        return axau_ease_overshoot(t);
    case AXAU_CURVE_LINEAR:
        return t < 0 ? 0 : (t > 1000 ? 1000 : t);
    case AXAU_CURVE_DECELERATE:
    default:
        return axau_ease_out(t);
    }
}

void axau_anim_init(axau_anim_t *anim, i32 value)
{
    if (!anim) return;
    ax_memset(anim, 0, sizeof(*anim));
    anim->from = value;
    anim->to = value;
    anim->value = value;
    anim->curve = AXAU_CURVE_DECELERATE;
}

void axau_anim_to(axau_anim_t *anim, i32 target, u32 duration_ms,
                  axau_curve_t curve)
{
    if (!anim) return;
    if (anim->to == target && !anim->running) {
        return;
    }
    anim->from = anim->value;
    anim->to = target;
    anim->duration_ms = duration_ms;
    anim->elapsed_ms = 0;
    anim->curve = curve;
    anim->running = (bool_t)(duration_ms > 0);
    if (!anim->running) {
        anim->value = target;
    }
}

void axau_anim_tick(axau_anim_t *anim, u32 dt_ms)
{
    if (!anim || !anim->running) {
        return;
    }
    if (g_context.reduced_motion) {
        anim->value = anim->to;
        anim->running = (bool_t)0;
        return;
    }
    anim->elapsed_ms += dt_ms;
    if (anim->elapsed_ms >= anim->duration_ms) {
        anim->value = anim->to;
        anim->running = (bool_t)0;
        if (anim->loop) {
            anim->elapsed_ms = 0;
            anim->running = (bool_t)1;
        }
        return;
    }
    i32 t = (i32)((anim->elapsed_ms * 1000) / anim->duration_ms);
    i32 eased = axau_curve_apply(anim->curve, t);
    anim->value = anim->from + ((anim->to - anim->from) * eased) / 1000;
}

void axau_anim_finish(axau_anim_t *anim)
{
    if (!anim) return;
    anim->value = anim->to;
    anim->elapsed_ms = anim->duration_ms;
    anim->running = (bool_t)0;
}

bool_t axau_anim_active(const axau_anim_t *anim)
{
    return (bool_t)(anim && anim->running);
}

i32 axau_anim_value(const axau_anim_t *anim)
{
    return anim ? anim->value : 0;
}

/* ------------------------------------------------------------------ */
/* Materials                                                           */
/* ------------------------------------------------------------------ */

void axau_material_mica(axau_material_t *material, const axau_palette_t *palette,
                        i32 radius)
{
    if (!material) return;
    ax_memset(material, 0, sizeof(*material));
    material->tint = axau_colour(palette, AXAU_COLOUR_SURFACE);
    material->fallback = material->tint;
    material->opacity = 78;
    material->blur = 24;
    material->noise = 6;
    material->radius = radius;
    material->hairline = (bool_t)1;
    material->inner_highlight = (bool_t)1;
}

void axau_material_acrylic(axau_material_t *material, const axau_palette_t *palette,
                           i32 radius)
{
    if (!material) return;
    ax_memset(material, 0, sizeof(*material));
    material->tint = axau_colour(palette, AXAU_COLOUR_SURFACE_ALT);
    material->fallback = material->tint;
    material->opacity = 62;
    material->blur = 32;
    material->noise = 14;
    material->radius = radius;
    material->hairline = (bool_t)1;
    material->inner_highlight = (bool_t)0;
}

void axau_material_card(axau_material_t *material, const axau_palette_t *palette,
                        i32 radius)
{
    if (!material) return;
    ax_memset(material, 0, sizeof(*material));
    material->tint = axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED);
    material->fallback = material->tint;
    material->opacity = 100;
    material->blur = 0;
    material->noise = 0;
    material->radius = radius;
    material->hairline = (bool_t)1;
    material->shadow = (bool_t)0;
}

void axau_material_control(axau_material_t *material, const axau_palette_t *palette)
{
    if (!material) return;
    ax_memset(material, 0, sizeof(*material));
    material->tint = axau_colour(palette, AXAU_COLOUR_CONTROL);
    material->fallback = material->tint;
    material->opacity = 100;
    material->blur = 0;
    material->radius = AXAU_RADIUS_CONTROL;
    material->hairline = (bool_t)0;
    material->shadow = (bool_t)0;
}

/* A deterministic 16-bit hash used for noise and reveal offsets.  This is not
 * cryptographic; it only needs to look unstructured at pixel scale. */
static u32 aurora_hash(u32 x)
{
    x ^= x >> 16;
    x *= 0x7FEB352D;
    x ^= x >> 15;
    x *= 0x846CA68B;
    x ^= x >> 16;
    return x;
}

void axau_paint_noise(gui_surface_t *surface, gui_rect_t rect, u32 seed,
                      i32 strength, const axau_palette_t *palette)
{
    if (!surface || strength <= 0) {
        return;
    }
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }
    i32 step = strength >= 16 ? 2 : 3;
    u32 tint = axau_colour(palette, AXAU_COLOUR_TEXT);
    for (i32 y = rect.y; y < rect.y + rect.h; y += step) {
        for (i32 x = rect.x; x < rect.x + rect.w; x += step) {
            u32 value = aurora_hash((u32)(x + y * 131) ^ seed);
            if ((value & 0xFF) > (u32)(strength * 3)) {
                continue;
            }
            u32 colour = axui_colour_alpha(tint, (u8)(4 + (value & 0x07)));
            gui_blend_rect(surface, gui_rect(x, y, 2, 2), colour);
        }
    }
}

void axau_paint_material(gui_surface_t *surface, gui_rect_t rect,
                         const axau_material_t *material,
                         const axau_palette_t *palette)
{
    if (!surface || !material || rect.w <= 0 || rect.h <= 0) {
        return;
    }
    u32 fill = axui_colour_opacity(material->tint, material->opacity);
    axui_round_rect_aa(surface, rect, material->radius, fill);

    if (material->noise > 0) {
        axau_paint_noise(surface, rect, (u32)(rect.x * 17 + rect.y * 31 + rect.w),
                        material->noise, palette);
    }
    if (material->inner_highlight) {
        gui_rect_t line = gui_rect(rect.x + material->radius, rect.y,
                                   rect.w - material->radius * 2, 1);
        u32 highlight = axui_colour_alpha(GUI_RGB(0xFF, 0xFF, 0xFF), 40);
        gui_hline(surface, line.x, line.x + line.w, line.y, highlight);
    }
    if (material->hairline) {
        axui_round_rect_border(surface, rect, material->radius, 1,
                               axau_colour(palette, AXAU_COLOUR_STROKE));
    }
}

/* A "island" is the floating surface used for flyouts and cards: material plus
 * a soft drop shadow offset two pixels down. */
void axau_paint_island(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                       const axau_palette_t *palette)
{
    axau_material_t material;
    axau_material_mica(&material, palette, radius);
    gui_shadow(surface, rect, 12, 90);
    axau_paint_material(surface, rect, &material, palette);
}

void axau_paint_divider(gui_surface_t *surface, gui_rect_t rect,
                        const axau_palette_t *palette)
{
    if (!surface || rect.w <= 0 || rect.h <= 0) {
        return;
    }
    u32 colour = axau_colour(palette, AXAU_COLOUR_DIVIDER);
    if (rect.w >= rect.h) {
        gui_hline(surface, rect.x, rect.x + rect.w, rect.y + rect.h / 2, colour);
    } else {
        gui_vline(surface, rect.x + rect.w / 2, rect.y, rect.y + rect.h, colour);
    }
}

void axau_paint_focus(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                      const axau_palette_t *palette)
{
    if (!surface) return;
    gui_rect_t outer = gui_rect(rect.x - 2, rect.y - 2, rect.w + 4, rect.h + 4);
    axui_round_rect_border(surface, outer, radius + 2, AXAU_FOCUS_STROKE,
                           axau_colour(palette, AXAU_COLOUR_FOCUS));
    axui_round_rect_border(surface, rect, radius, 1,
                           axui_colour_alpha(GUI_RGB(0xFF, 0xFF, 0xFF), 120));
}

/* A pointer-following highlight: two soft radial lobes that make a surface feel
 * lit rather than painted.  Used on cards, not on text controls, because it
 * reduces text contrast underneath. */
void axau_paint_reveal(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                       i32 pointer_x, i32 pointer_y, const axau_palette_t *palette)
{
    if (!surface) return;
    i32 local_x = pointer_x - rect.x;
    i32 local_y = pointer_y - rect.y;
    if (local_x < 0 || local_y < 0 || local_x >= rect.w || local_y >= rect.h) {
        return;
    }
    gui_rect_t clipped = gui_rect_intersect(rect,
                                            gui_rect(pointer_x - 96,
                                                     pointer_y - 96, 192, 192));
    if (clipped.w <= 0 || clipped.h <= 0) {
        return;
    }
    u32 glow = axui_colour_alpha(GUI_RGB(0xFF, 0xFF, 0xFF), 18);
    i32 distance_limit = 120;
    for (i32 y = clipped.y; y < clipped.y + clipped.h; y += 4) {
        for (i32 x = clipped.x; x < clipped.x + clipped.w; x += 4) {
            i32 dx = x - pointer_x;
            i32 dy = y - pointer_y;
            i32 distance = dx * dx + dy * dy;
            if (distance > distance_limit * distance_limit) {
                continue;
            }
            u32 alpha = (u32)((i64)18 * (distance_limit * distance_limit - distance)
                              / (distance_limit * distance_limit));
            gui_blend_rect(surface, gui_rect(x, y, 4, 4),
                           axui_colour_alpha(glow, (u8)alpha));
        }
    }
    (void)radius;
    (void)palette;
}

/* ------------------------------------------------------------------ */
/* Shared text helpers                                                 */
/* ------------------------------------------------------------------ */

i32 axau_text_center_y(gui_rect_t rect, u8 scale)
{
    i32 line = axui_line_height(scale);
    return rect.y + (rect.h - line) / 2;
}

void axau_label(gui_surface_t *surface, gui_rect_t rect, const char *text,
                u32 colour, u8 scale)
{
    if (!surface || !text) return;
    axui_draw_text(surface, rect.x, axau_text_center_y(rect, scale), text,
                   axui_style(colour, scale));
}

void axau_label_right(gui_surface_t *surface, gui_rect_t rect, const char *text,
                      u32 colour, u8 scale)
{
    if (!surface || !text) return;
    i32 width = axui_measure_text(text, scale);
    axui_draw_text(surface, rect.x + rect.w - width,
                   axau_text_center_y(rect, scale), text,
                   axui_style(colour, scale));
}

void axau_icon_glyph(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                     const char *glyph, u32 colour)
{
    if (!surface || !glyph) return;
    axui_draw_text_center(surface,
                          gui_rect(cx - size / 2, cy - size / 2, size, size),
                          glyph, axui_style(colour, 1));
}

/* Placeholder for the eventual rounded clip.  The rasteriser has no scissor
 * stack yet, so callers that need clipping inside a rounded panel pass an
 * inset rectangle; this function records the intent and keeps call sites
 * honest until the clip stack lands. */
void axau_rounded_clip_note(gui_surface_t *surface, gui_rect_t rect, i32 radius)
{
    (void)surface;
    (void)rect;
    (void)radius;
}
