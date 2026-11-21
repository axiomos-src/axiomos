/* AxiomOS UI - theme registry and metrics.
 *
 * A theme owns twenty-one colours; every widget derives the rest of its
 * palette from those (hover tints, disabled greys, hairlines).  That keeps the
 * theme struct small enough to live in the kernel image while still allowing
 * four complete looks, including a high-contrast variant for accessibility.
 *
 * Metrics are shared by all themes: spacing steps, corner radii, control
 * heights and the timing constants the input layer uses for double clicks and
 * caret blinking.  Keeping timing here means the settings page can expose it
 * without reaching into the widget implementations.
 */

#include <axiom/lib.h>
#include "ui.h"

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

static const axui_metrics_t ax_metrics = {
    4,          /* spacing_1 */
    8,          /* spacing_2 */
    12,         /* spacing_3 */
    16,         /* spacing_4 */
    4,          /* radius_small */
    8,          /* radius_medium */
    14,         /* radius_large */
    1,          /* border_width */
    30,         /* titlebar_height */
    40,         /* taskbar_height */
    10,         /* scrollbar_width */
    24,         /* scrollbar_min_thumb */
    24,         /* row_height */
    2,          /* caret_width */
    500,        /* caret_blink_ms */
    450,        /* tooltip_delay_ms */
    380,        /* double_click_ms */
    160,        /* animation_ms */
};

const axui_metrics_t *axui_metrics(void)
{
    return &ax_metrics;
}

/* ------------------------------------------------------------------ */
/* Theme tables                                                        */
/* ------------------------------------------------------------------ */

/* "Axiom dark": the default.  Cool neutrals with a single blue accent; the
 * contrast ratios are all above 4.5:1 for body text. */
static const gui_theme_t ax_theme_dark = {
    0xFF0B0D12,   /* desktop_top */
    0xFF141822,   /* desktop_bottom */
    0xFF161A23,   /* window_bg */
    0xFF1B2029,   /* window_bg_alt */
    0xFF1E2430,   /* title_bg */
    0xFF232A38,   /* title_bg_active */
    0xFFCDD3E0,   /* title_text */
    0xFF7C8598,   /* title_text_inactive */
    0xFF2A3140,   /* border */
    0xFF3D7EFF,   /* border_active */
    0xFF3D7EFF,   /* accent */
    0xFF2B5CC4,   /* accent_dim */
    0xFFE4E8F1,   /* text */
    0xFF98A2B3,   /* text_dim */
    0xFF0B0D12,   /* text_inverse */
    0xFF11141C,   /* taskbar */
    0xFF232A38,   /* taskbar_hover */
    0xFF30A46C,   /* success */
    0xFFFFC53D,   /* warning */
    0xFFE5484D,   /* error */
    0xFF2A4A86,   /* selection */
};

/* "Axiom light": paper background, ink text, the same accent so the shell's
 * identity does not change with the theme. */
static const gui_theme_t ax_theme_light = {
    0xFFDCE3EC,   /* desktop_top */
    0xFFC9D3DF,   /* desktop_bottom */
    0xFFF7F8FA,   /* window_bg */
    0xFFEDF0F4,   /* window_bg_alt */
    0xFFE4E9F0,   /* title_bg */
    0xFFD7DEE8,   /* title_bg_active */
    0xFF1A1F28,   /* title_text */
    0xFF6B7484,   /* title_text_inactive */
    0xFFC3CCD8,   /* border */
    0xFF2F6FE0,   /* border_active */
    0xFF2563EB,   /* accent */
    0xFF1D4ED8,   /* accent_dim */
    0xFF161A22,   /* text */
    0xFF5A6373,   /* text_dim */
    0xFFF7F8FA,   /* text_inverse */
    0xFFE8EDF4,   /* taskbar */
    0xFFD7DEE8,   /* taskbar_hover */
    0xFF177A4C,   /* success */
    0xFF9A6400,   /* warning */
    0xFFB32B33,   /* error */
    0xFFBBD0F5,   /* selection */
};

/* "Solarized": the classic low-contrast palette, kept for people who stare at
 * a terminal all day. */
static const gui_theme_t ax_theme_solarized = {
    0xFF002B36,   /* desktop_top */
    0xFF073642,   /* desktop_bottom */
    0xFFFDF6E3,   /* window_bg */
    0xFFEEE8D5,   /* window_bg_alt */
    0xFFE4DDC8,   /* title_bg */
    0xFFD8D1BC,   /* title_bg_active */
    0xFF073642,   /* title_text */
    0xFF586E75,   /* title_text_inactive */
    0xFFCDC6B2,   /* border */
    0xFF268BD2,   /* border_active */
    0xFF268BD2,   /* accent */
    0xFF1A6FA8,   /* accent_dim */
    0xFF002B36,   /* text */
    0xFF586E75,   /* text_dim */
    0xFFFDF6E3,   /* text_inverse */
    0xFFEEE8D5,   /* taskbar */
    0xFFE0D9C4,   /* taskbar_hover */
    0xFF859900,   /* success */
    0xFFB58900,   /* warning */
    0xFFDC322F,   /* error */
    0xFFB8D6EA,   /* selection */
};

/* "High contrast": pure black and white with a saturated focus ring.  Every
 * pair here reaches at least 7:1, the WCAG AAA threshold. */
static const gui_theme_t ax_theme_high_contrast = {
    0xFF000000,   /* desktop_top */
    0xFF000000,   /* desktop_bottom */
    0xFF0A0A0A,   /* window_bg */
    0xFF121212,   /* window_bg_alt */
    0xFF1A1A1A,   /* title_bg */
    0xFF242424,   /* title_bg_active */
    0xFFFFFFFF,   /* title_text */
    0xFFBDBDBD,   /* title_text_inactive */
    0xFFFFFFFF,   /* border */
    0xFFFFFF00,   /* border_active */
    0xFFFFFF00,   /* accent */
    0xFFB8B800,   /* accent_dim */
    0xFFFFFFFF,   /* text */
    0xFFD0D0D0,   /* text_dim */
    0xFF000000,   /* text_inverse */
    0xFF000000,   /* taskbar */
    0xFF303030,   /* taskbar_hover */
    0xFF00FF6A,   /* success */
    0xFFFFFF00,   /* warning */
    0xFFFF5A5F,   /* error */
    0xFF3C3C00,   /* selection */
};

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

#define AX_THEME_COUNT 4

static const gui_theme_t *const ax_themes[AX_THEME_COUNT] = {
    &ax_theme_dark,
    &ax_theme_light,
    &ax_theme_solarized,
    &ax_theme_high_contrast,
};

static const char *const ax_theme_names[AX_THEME_COUNT] = {
    "Axiom Dark",
    "Axiom Light",
    "Solarized",
    "High Contrast",
};

static const char *const ax_theme_ids[AX_THEME_COUNT] = {
    "dark",
    "light",
    "solarized",
    "high-contrast",
};

/* The current theme starts as the dark one; the settings application can
 * change it and the compositor picks the new pointer up on the next frame. */
static const gui_theme_t *ax_current = &ax_theme_dark;

i32 axui_theme_count(void)
{
    return AX_THEME_COUNT;
}

const char *axui_theme_name(i32 index)
{
    if (index < 0 || index >= AX_THEME_COUNT) {
        return "unknown";
    }
    return ax_theme_names[index];
}

const char *axui_theme_id(i32 index)
{
    if (index < 0 || index >= AX_THEME_COUNT) {
        return "unknown";
    }
    return ax_theme_ids[index];
}

const gui_theme_t *axui_theme_dark(void)
{
    return &ax_theme_dark;
}

const gui_theme_t *axui_theme_light(void)
{
    return &ax_theme_light;
}

const gui_theme_t *axui_theme_solarized(void)
{
    return &ax_theme_solarized;
}

const gui_theme_t *axui_theme_high_contrast(void)
{
    return &ax_theme_high_contrast;
}

const gui_theme_t *axui_theme_current(void)
{
    return ax_current;
}

void axui_theme_set(const gui_theme_t *theme)
{
    if (theme) {
        ax_current = theme;
    }
}

/* Accepts either the display name or the short id, case insensitively. */
bool_t axui_theme_select(const char *name)
{
    if (!name) {
        return (bool_t)0;
    }
    for (i32 i = 0; i < AX_THEME_COUNT; i++) {
        if (ax_strcasecmp(name, ax_theme_ids[i]) == 0 ||
            ax_strcasecmp(name, ax_theme_names[i]) == 0) {
            ax_current = ax_themes[i];
            return (bool_t)1;
        }
    }
    return (bool_t)0;
}

/* Index of the current theme, or -1 when a caller installed its own table. */
i32 axui_theme_index(void)
{
    for (i32 i = 0; i < AX_THEME_COUNT; i++) {
        if (ax_themes[i] == ax_current) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Palette dump                                                        */
/* ------------------------------------------------------------------ */

/* Emit every colour of a theme through a line sink; used by the about page and
 * the HVCI diagnostics screen, which both render a palette strip. */
void axui_theme_palette_dump(void (*sink)(const char *line))
{
    if (!sink) {
        return;
    }
    struct entry {
        const char *key;
        u32         value;
    };
    const struct entry entries[] = {
        { "desktop_top", ax_current->desktop_top },
        { "desktop_bottom", ax_current->desktop_bottom },
        { "window_bg", ax_current->window_bg },
        { "window_bg_alt", ax_current->window_bg_alt },
        { "title_bg", ax_current->title_bg },
        { "title_bg_active", ax_current->title_bg_active },
        { "title_text", ax_current->title_text },
        { "title_text_inactive", ax_current->title_text_inactive },
        { "border", ax_current->border },
        { "border_active", ax_current->border_active },
        { "accent", ax_current->accent },
        { "accent_dim", ax_current->accent_dim },
        { "text", ax_current->text },
        { "text_dim", ax_current->text_dim },
        { "text_inverse", ax_current->text_inverse },
        { "taskbar", ax_current->taskbar },
        { "taskbar_hover", ax_current->taskbar_hover },
        { "success", ax_current->success },
        { "warning", ax_current->warning },
        { "error", ax_current->error },
        { "selection", ax_current->selection },
    };

    char line[64];
    for (usize i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        char hex[16];
        axui_format_colour(entries[i].value, hex, sizeof(hex));
        (void)ax_snprintf(line, sizeof(line), "%s = %s", entries[i].key, hex);
        sink(line);
    }
}

/* ------------------------------------------------------------------ */
/* Derived widget colours                                              */
/* ------------------------------------------------------------------ */

/* Surfaces layered on the window background: panels, cards and popups step
 * away from window_bg by one and two shades respectively. */
u32 axui_surface_raised(void)
{
    return ax_current->window_bg_alt;
}

u32 axui_surface_sunken(void)
{
    return axui_is_dark(ax_current->window_bg) ? axui_darken(ax_current->window_bg, 6)
                                              : axui_darken(ax_current->window_bg, 4);
}

u32 axui_surface_overlay(void)
{
    return axui_is_dark(ax_current->window_bg)
               ? axui_lighten(ax_current->window_bg, 8)
               : axui_darken(ax_current->window_bg, 2);
}

/* Hover tint for a button-like control: a small lift on dark themes, a small
 * darkening on light themes.  Directing the tint the same way as the theme's
 * own polarity keeps the control from inverting against its neighbours. */
u32 axui_hover_tint(u32 base)
{
    return axui_is_dark(base) ? axui_lighten(base, 6) : axui_darken(base, 5);
}

u32 axui_press_tint(u32 base)
{
    return axui_is_dark(base) ? axui_darken(base, 4) : axui_darken(base, 10);
}

/* Focus ring: the accent, softened so it does not overpower the control. */
u32 axui_focus_ring(void)
{
    return axui_colour_alpha(ax_current->accent, 190);
}

/* Disabled foreground and background for form controls. */
u32 axui_disabled_text(void)
{
    return axui_tint(ax_current->text, ax_current->window_bg, 45);
}

u32 axui_disabled_fill(void)
{
    return axui_tint(ax_current->window_bg_alt, ax_current->window_bg, 60);
}

/* Text on accent-coloured fills, contrast corrected so a custom accent in the
 * settings page cannot end up unreadable. */
u32 axui_accent_text(void)
{
    return axui_ensure_contrast(ax_current->text_inverse, ax_current->accent);
}

/* The middle stop of a title bar gradient; the compositor draws the bar with
 * three stops so the top highlight reads on light themes too. */
u32 axui_title_gradient_mid(void)
{
    return axui_lerp_colour(ax_current->title_bg_active,
                            axui_lighten(ax_current->title_bg_active, 10), 40);
}

/* Selection colour for text runs: pre-multiplied so the terminal can paint it
 * without a second blend pass. */
u32 axui_selection_colour(void)
{
    return axui_colour_alpha(ax_current->selection, 200);
}

/* Draw colour for the window border based on focus. */
u32 axui_window_border(bool_t focused)
{
    return focused ? ax_current->border_active : ax_current->border;
}

/* Desktop gradient stops, exposed so the wallpaper can reuse them. */
u32 axui_desktop_top(void)
{
    return ax_current->desktop_top;
}

u32 axui_desktop_bottom(void)
{
    return ax_current->desktop_bottom;
}

/* A per-theme shadow strength: dark themes need a deeper shadow to separate
 * windows from the desktop, light themes a softer one. */
i32 axui_shadow_strength(void)
{
    return axui_is_dark(ax_current->window_bg) ? 190 : 110;
}

/* ------------------------------------------------------------------ */
/* Accent overrides                                                    */
/* ------------------------------------------------------------------ */

/* The settings page can recolour the accent without rebuilding the table.
 * A local copy of the active theme is kept so the pointer stays valid. */
static gui_theme_t ax_override;
static bool_t      ax_override_active = (bool_t)0;

void axui_set_accent(u32 accent)
{
    if (!ax_override_active) {
        ax_override = *ax_current;
        ax_override_active = (bool_t)1;
    }
    ax_override.accent = accent;
    ax_override.border_active = accent;
    ax_override.accent_dim = axui_darken(accent, 25);
    ax_current = &ax_override;
}

void axui_clear_accent(void)
{
    if (ax_override_active) {
        ax_override_active = (bool_t)0;
        ax_current = &ax_theme_dark;
    }
}

bool_t axui_accent_overridden(void)
{
    return ax_override_active;
}

/* ------------------------------------------------------------------ */
/* Metrics helpers                                                     */
/* ------------------------------------------------------------------ */

/* Standard control height for the current metrics: text fields, buttons and
 * combo boxes all share it so rows line up without per-widget rules. */
i32 axui_control_height(void)
{
    return ax_metrics.row_height;
}

i32 axui_spacing(i32 step)
{
    switch (step) {
    case 1: return ax_metrics.spacing_1;
    case 2: return ax_metrics.spacing_2;
    case 3: return ax_metrics.spacing_3;
    case 4: return ax_metrics.spacing_4;
    default: return ax_metrics.spacing_2;
    }
}

/* Pixels per millisecond converted to compositor frames; the animation code
 * uses this to keep fades tied to wall-clock time rather than frame count. */
i32 axui_frames_for_ms(i32 ms, i32 frame_ms)
{
    if (frame_ms <= 0) {
        frame_ms = 16;
    }
    i32 frames = ms / frame_ms;
    return frames > 0 ? frames : 1;
}

/* Easing: cubic ease-out on a 0..100 progress value.  Written as a table-free
 * integer approximation so it stays cheap in the compositor loop. */
i32 axui_ease_out(i32 t)
{
    if (t <= 0) {
        return 0;
    }
    if (t >= 100) {
        return 100;
    }
    i32 inverse = 100 - t;
    return 100 - (inverse * inverse * inverse) / 10000;
}

i32 axui_ease_in_out(i32 t)
{
    if (t <= 0) {
        return 0;
    }
    if (t >= 100) {
        return 100;
    }
    /* Symmetric: accelerate to the midpoint, then decelerate. */
    if (t < 50) {
        return (t * t * 2) / 100;
    }
    i32 u = 100 - t;
    return 100 - (u * u * 2) / 100;
}

/* Step an animation value towards its target, never overshooting. */
i32 axui_approach(i32 current, i32 target, i32 step)
{
    if (step <= 0) {
        step = 1;
    }
    if (current < target) {
        current += step;
        return current > target ? target : current;
    }
    if (current > target) {
        current -= step;
        return current < target ? target : current;
    }
    return target;
}
