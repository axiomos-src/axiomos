/*
 * AxiomOS - Aurora design system.
 *
 * Aurora is AxiomOS's own desktop design language: a calm, layered surface
 * system with sampled materials, soft depth and a small set of motion rules.
 * It is not a port of another platform's toolkit; the geometry and palette are
 * defined here, and every surface in the shell is built from these primitives.
 *
 * Tokens:
 *
 *   - 8 px spacing grid, 4 px for dense rows, 48 px standard row height;
 *   - corner radii: 4 control, 8 card, 12 flyout, 28 window, clipped at paint
 *     time rather than at window creation;
 *   - two stroke layers: a hairline border and a 1 px top highlight on
 *     elevated surfaces;
 *   - motion: 150 ms for state changes, 250 ms for flyouts, 350 ms for page
 *     transitions, one decelerate curve, and an opt-out for reduced motion;
 *   - materials sample the wallpaper instead of using a flat fill, so a panel
 *     stays legible over a bright or dark region without a second theme.
 *
 * Controls are immediate-mode: state lives in the caller's struct, painting is
 * a pure function of that state, and hit testing shares the layout helper.
 * That is the same contract the existing widget layer uses, so the two can
 * coexist while the shell migrates.
 */
#ifndef AXIOM_AURORA_H
#define AXIOM_AURORA_H

#include <axiom/types.h>
#include "gui.h"
#include "ui.h"

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

#define AXAU_GRID            8
#define AXAU_GRID_HALF       4
#define AXAU_ROW_HEIGHT      48
#define AXAU_ROW_COMPACT     32
#define AXAU_CONTROL_HEIGHT  32
#define AXAU_BUTTON_HEIGHT   32
#define AXAU_INPUT_HEIGHT    32
#define AXAU_RADIUS_CONTROL  4
#define AXAU_RADIUS_CARD     8
#define AXAU_RADIUS_FLYOUT   12
#define AXAU_RADIUS_WINDOW   28
#define AXAU_STROKE          1
#define AXAU_FOCUS_STROKE    2
#define AXAU_ICON_SIZE       16
#define AXAU_ICON_LARGE      24
#define AXAU_MAX_CONTROLS    128
#define AXAU_MAX_TABS        8
#define AXAU_MAX_NAV_ITEMS   16
#define AXAU_MAX_DIALOG_BTNS 3

/* Palette slots.  The dark and light tables below fill these in, and the
 * high-contrast table replaces them wholesale. */
typedef enum axau_colour_role {
    AXAU_COLOUR_TEXT = 0,
    AXAU_COLOUR_TEXT_SECONDARY,
    AXAU_COLOUR_TEXT_TERTIARY,
    AXAU_COLOUR_TEXT_DISABLED,
    AXAU_COLOUR_ACCENT,
    AXAU_COLOUR_ACCENT_HOVER,
    AXAU_COLOUR_ACCENT_PRESSED,
    AXAU_COLOUR_ACCENT_TEXT,
    AXAU_COLOUR_SURFACE,
    AXAU_COLOUR_SURFACE_ALT,
    AXAU_COLOUR_SURFACE_ELEVATED,
    AXAU_COLOUR_CONTROL,
    AXAU_COLOUR_CONTROL_HOVER,
    AXAU_COLOUR_CONTROL_PRESSED,
    AXAU_COLOUR_CONTROL_DISABLED,
    AXAU_COLOUR_STROKE,
    AXAU_COLOUR_STROKE_STRONG,
    AXAU_COLOUR_DIVIDER,
    AXAU_COLOUR_SHADOW,
    AXAU_COLOUR_SUCCESS,
    AXAU_COLOUR_WARNING,
    AXAU_COLOUR_ERROR,
    AXAU_COLOUR_FOCUS,
    AXAU_COLOUR_COUNT
} axau_colour_role_t;

typedef enum axau_theme_kind {
    AXAU_THEME_LIGHT = 0,
    AXAU_THEME_DARK,
    AXAU_THEME_HIGH_CONTRAST,
    AXAU_THEME_CUSTOM
} axau_theme_kind_t;

typedef struct axau_palette {
    axau_theme_kind_t kind;
    u32 colours[AXAU_COLOUR_COUNT];
    u32 accent;
    char name[24];
} axau_palette_t;

/* ------------------------------------------------------------------ */
/* Density, typography, metrics                                        */
/* ------------------------------------------------------------------ */

typedef enum axau_density {
    AXAU_DENSITY_COMFORTABLE = 0,
    AXAU_DENSITY_COMPACT,
    AXAU_DENSITY_TOUCH
} axau_density_t;

typedef struct axau_type_scale {
    u8 caption;       /* 12 sp -> scale 1 in the bitmap face */
    u8 body;          /* 14 sp -> scale 1 or 2 depending on DPI */
    u8 body_strong;
    u8 subtitle;      /* 20 sp */
    u8 title;         /* 28 sp */
    u8 title_large;   /* 40 sp */
    u8 display;       /* 68 sp */
} axau_type_scale_t;

typedef struct axau_metrics {
    i32 dpi;
    i32 scale_percent;      /* 100, 125, 150, 175, 200 */
    axau_density_t density;
    axau_type_scale_t type;
    i32 row_height;
    i32 control_height;
    i32 radius_control;
    i32 radius_card;
    i32 radius_flyout;
    i32 stroke;
} axau_metrics_t;

void axau_metrics_init(axau_metrics_t *metrics, i32 dpi);
void axau_metrics_set_density(axau_metrics_t *metrics, axau_density_t density);
i32  axau_dp(const axau_metrics_t *metrics, i32 value);
i32  axau_dp_inverse(const axau_metrics_t *metrics, i32 pixels);

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

void axau_palette_light(axau_palette_t *palette, u32 accent);
void axau_palette_dark(axau_palette_t *palette, u32 accent);
void axau_palette_high_contrast(axau_palette_t *palette);
u32  axau_colour(const axau_palette_t *palette, axau_colour_role_t role);
u32  axau_with_accent(const axau_palette_t *palette, u32 accent);
const char *axau_theme_name(axau_theme_kind_t kind);

/* ------------------------------------------------------------------ */
/* Motion                                                              */
/* ------------------------------------------------------------------ */

typedef enum axau_curve {
    AXAU_CURVE_DECELERATE = 0,
    AXAU_CURVE_ACCELERATE,
    AXAU_CURVE_EASE,
    AXAU_CURVE_OVERSHOOT,
    AXAU_CURVE_LINEAR
} axau_curve_t;

typedef struct axau_anim {
    i32 from;
    i32 to;
    i32 value;      /* 0..1000 */
    u32 duration_ms;
    u32 elapsed_ms;
    axau_curve_t curve;
    bool_t running;
    bool_t loop;
} axau_anim_t;

void axau_anim_init(axau_anim_t *anim, i32 value);
void axau_anim_to(axau_anim_t *anim, i32 target, u32 duration_ms, axau_curve_t curve);
void axau_anim_tick(axau_anim_t *anim, u32 dt_ms);
void axau_anim_finish(axau_anim_t *anim);
bool_t axau_anim_active(const axau_anim_t *anim);
i32  axau_anim_value(const axau_anim_t *anim);
i32  axau_curve_apply(axau_curve_t curve, i32 t);   /* t 0..1000 */
i32  axau_ease_out(i32 t);
i32  axau_ease_in_out(i32 t);
i32  axau_ease_overshoot(i32 t);

/* ------------------------------------------------------------------ */
/* Materials                                                           */
/* ------------------------------------------------------------------ */

typedef struct axau_material {
    u32  tint;
    u32  fallback;
    i32  opacity;          /* 0..100 */
    i32  blur;             /* logical radius, 0 = none */
    i32  noise;            /* 0..100 */
    i32  radius;
    bool_t hairline;
    bool_t inner_highlight;
    bool_t shadow;
} axau_material_t;

void axau_material_mica(axau_material_t *material, const axau_palette_t *palette,
                        i32 radius);
void axau_material_acrylic(axau_material_t *material, const axau_palette_t *palette,
                           i32 radius);
void axau_material_card(axau_material_t *material, const axau_palette_t *palette,
                        i32 radius);
void axau_material_control(axau_material_t *material, const axau_palette_t *palette);
void axau_paint_material(gui_surface_t *surface, gui_rect_t rect,
                         const axau_material_t *material,
                         const axau_palette_t *palette);
void axau_paint_island(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                       const axau_palette_t *palette);
void axau_paint_divider(gui_surface_t *surface, gui_rect_t rect,
                        const axau_palette_t *palette);
void axau_paint_focus(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                      const axau_palette_t *palette);
void axau_paint_noise(gui_surface_t *surface, gui_rect_t rect, u32 seed,
                      i32 strength, const axau_palette_t *palette);
void axau_paint_reveal(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                       i32 pointer_x, i32 pointer_y, const axau_palette_t *palette);

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

typedef enum axau_control_kind {
    AXAU_CONTROL_BUTTON = 1,
    AXAU_CONTROL_ACCENT_BUTTON,
    AXAU_CONTROL_CHECKBOX,
    AXAU_CONTROL_RADIO,
    AXAU_CONTROL_TOGGLE,
    AXAU_CONTROL_SLIDER,
    AXAU_CONTROL_PROGRESS,
    AXAU_CONTROL_RING,
    AXAU_CONTROL_CHIP,
    AXAU_CONTROL_TEXTBOX
} axau_control_kind_t;

typedef enum axau_control_state {
    AXAU_STATE_NORMAL = 0,
    AXAU_STATE_HOVER,
    AXAU_STATE_PRESSED,
    AXAU_STATE_FOCUSED,
    AXAU_STATE_DISABLED
} axau_control_state_t;

typedef struct axau_button {
    gui_rect_t rect;
    char       label[48];
    char       glyph[4];
    bool_t     accent;
    bool_t     subtle;
    bool_t     has_icon;
    bool_t     hover;
    bool_t     pressed;
    bool_t     focused;
    bool_t     enabled;
    i32        press_anim;
} axau_button_t;

typedef struct axau_checkbox {
    gui_rect_t rect;
    char       label[64];
    bool_t     checked;
    bool_t     indeterminate;
    bool_t     hover;
    bool_t     focused;
    bool_t     enabled;
    i32        check_anim;
} axau_checkbox_t;

typedef struct axau_radio {
    gui_rect_t rect;
    char       label[64];
    bool_t     selected;
    bool_t     hover;
    bool_t     focused;
    bool_t     enabled;
} axau_radio_t;

typedef struct axau_toggle {
    gui_rect_t rect;
    char       on_label[24];
    char       off_label[24];
    bool_t     on;
    bool_t     hover;
    bool_t     pressed;
    bool_t     focused;
    bool_t     enabled;
    i32        thumb_anim;
} axau_toggle_t;

typedef struct axau_slider {
    gui_rect_t rect;
    i32        value;
    i32        minimum;
    i32        maximum;
    i32        step;
    bool_t     dragging;
    bool_t     hover;
    bool_t     focused;
    bool_t     enabled;
    bool_t     show_value;
} axau_slider_t;

typedef struct axau_progress {
    gui_rect_t rect;
    i32        percent;         /* 0..100, negative = indeterminate */
    i32        phase;           /* animation phase 0..360 */
    bool_t     ring;
    bool_t     paused;
} axau_progress_t;

typedef struct axau_chip {
    gui_rect_t rect;
    char       label[24];
    bool_t     selected;
    bool_t     hover;
    bool_t     dismissible;
} axau_chip_t;

typedef struct axau_textbox {
    gui_rect_t rect;
    char       text[128];
    char       placeholder[48];
    u32        cursor;
    i32        scroll;
    bool_t     focused;
    bool_t     password;
    bool_t     multiline;
    bool_t     hover;
    bool_t     error;
} axau_textbox_t;

void axau_button_paint(gui_surface_t *surface, axau_button_t *button,
                       const axau_palette_t *palette, const axau_metrics_t *metrics);
bool_t axau_button_hit(axau_button_t *button, i32 x, i32 y);
bool_t axau_button_down(axau_button_t *button, i32 x, i32 y);
bool_t axau_button_up(axau_button_t *button, i32 x, i32 y);

void axau_checkbox_paint(gui_surface_t *surface, axau_checkbox_t *box,
                         const axau_palette_t *palette, const axau_metrics_t *metrics);
bool_t axau_checkbox_hit(axau_checkbox_t *box, i32 x, i32 y);
void axau_checkbox_toggle(axau_checkbox_t *box, i32 x, i32 y);

void axau_radio_paint(gui_surface_t *surface, axau_radio_t *radio,
                      const axau_palette_t *palette, const axau_metrics_t *metrics);
bool_t axau_radio_hit(axau_radio_t *radio, i32 x, i32 y);

void axau_toggle_paint(gui_surface_t *surface, axau_toggle_t *toggle,
                       const axau_palette_t *palette, const axau_metrics_t *metrics);
bool_t axau_toggle_hit(axau_toggle_t *toggle, i32 x, i32 y);
bool_t axau_toggle_click(axau_toggle_t *toggle, i32 x, i32 y);

void axau_slider_paint(gui_surface_t *surface, axau_slider_t *slider,
                       const axau_palette_t *palette, const axau_metrics_t *metrics);
bool_t axau_slider_hit(axau_slider_t *slider, i32 x, i32 y);
bool_t axau_slider_drag(axau_slider_t *slider, i32 x, i32 y);
void axau_slider_release(axau_slider_t *slider);
i32  axau_slider_from_x(const axau_slider_t *slider, i32 x);

void axau_progress_paint(gui_surface_t *surface, axau_progress_t *progress,
                         const axau_palette_t *palette);
void axau_progress_tick(axau_progress_t *progress, i32 dt_ms);

void axau_chip_paint(gui_surface_t *surface, axau_chip_t *chip,
                     const axau_palette_t *palette);
bool_t axau_chip_hit(axau_chip_t *chip, i32 x, i32 y);

void axau_textbox_paint(gui_surface_t *surface, axau_textbox_t *box,
                        const axau_palette_t *palette, const axau_metrics_t *metrics);
bool_t axau_textbox_key(axau_textbox_t *box, char key);
bool_t axau_textbox_hit(axau_textbox_t *box, i32 x, i32 y);
void axau_textbox_clear(axau_textbox_t *box);

/* ------------------------------------------------------------------ */
/* Navigation and containers                                           */
/* ------------------------------------------------------------------ */

typedef struct axau_nav_item {
    char       label[32];
    char       glyph[4];
    bool_t     selected;
    bool_t     hover;
    i32        selection_anim;
    i32        badge;
} axau_nav_item_t;

typedef struct axau_navigation {
    gui_rect_t     rect;
    axau_nav_item_t items[AXAU_MAX_NAV_ITEMS];
    i32            count;
    i32            selected;
    i32            hover;
    bool_t         compact;
    bool_t         open;
    i32            pane_width;
    i32            anim;
    char           header[40];
} axau_navigation_t;

typedef struct axau_tab {
    char       title[24];
    bool_t     selected;
    bool_t     hover;
    bool_t     closeable;
    i32        width;
} axau_tab_t;

typedef struct axau_tabbar {
    gui_rect_t rect;
    axau_tab_t tabs[AXAU_MAX_TABS];
    i32        count;
    i32        selected;
    i32        hover;
    i32        scroll;
    bool_t     overflow;
    i32        indicator_x;
} axau_tabbar_t;

typedef struct axau_command_bar {
    gui_rect_t rect;
    axau_button_t commands[8];
    i32           count;
    bool_t        overflow_open;
} axau_command_bar_t;

typedef struct axau_breadcrumb {
    gui_rect_t rect;
    char       parts[4][32];
    i32        count;
    i32        hover;
} axau_breadcrumb_t;

void axau_navigation_init(axau_navigation_t *nav, gui_rect_t rect, i32 pane_width);
void axau_navigation_add(axau_navigation_t *nav, const char *label, const char *glyph);
void axau_navigation_paint(gui_surface_t *surface, axau_navigation_t *nav,
                           const axau_palette_t *palette, const axau_metrics_t *metrics);
i32  axau_navigation_hit(axau_navigation_t *nav, i32 x, i32 y);

void axau_tabbar_init(axau_tabbar_t *bar, gui_rect_t rect);
void axau_tabbar_add(axau_tabbar_t *bar, const char *title, bool_t closeable);
void axau_tabbar_paint(gui_surface_t *surface, axau_tabbar_t *bar,
                       const axau_palette_t *palette, const axau_metrics_t *metrics);
i32  axau_tabbar_hit(axau_tabbar_t *bar, i32 x, i32 y, bool_t *close_hit);

void axau_command_bar_init(axau_command_bar_t *bar, gui_rect_t rect);
void axau_command_bar_add(axau_command_bar_t *bar, const char *label, const char *glyph,
                          bool_t accent);
void axau_command_bar_paint(gui_surface_t *surface, axau_command_bar_t *bar,
                            const axau_palette_t *palette, const axau_metrics_t *metrics);
i32  axau_command_bar_hit(axau_command_bar_t *bar, i32 x, i32 y);

void axau_breadcrumb_set(axau_breadcrumb_t *crumb, gui_rect_t rect, const char *path);
void axau_breadcrumb_paint(gui_surface_t *surface, axau_breadcrumb_t *crumb,
                           const axau_palette_t *palette);
i32  axau_breadcrumb_hit(axau_breadcrumb_t *crumb, i32 x, i32 y);

/* ------------------------------------------------------------------ */
/* Dialogs and flyouts                                                 */
/* ------------------------------------------------------------------ */

typedef struct axau_dialog {
    gui_rect_t      rect;
    char            title[48];
    char            body[256];
    char            buttons[AXAU_MAX_DIALOG_BTNS][24];
    i32             button_count;
    i32             default_button;
    i32             hover_button;
    bool_t          open;
    bool_t          modal;
    i32             anim;
    gui_rect_t      button_rects[AXAU_MAX_DIALOG_BTNS];
} axau_dialog_t;

typedef struct axau_flyout {
    gui_rect_t rect;
    i32        anchor_x;
    i32        anchor_y;
    bool_t     open;
    i32        anim;
    bool_t     above;
} axau_flyout_t;

typedef struct axau_tooltip {
    char       text[64];
    gui_rect_t anchor;
    u32        delay_ms;
    u32        elapsed_ms;
    bool_t     visible;
    i32        anim;
} axau_tooltip_t;

typedef struct axau_menu_item {
    char       label[40];
    char       shortcut[16];
    char       glyph[4];
    bool_t     enabled;
    bool_t     checked;
    bool_t     separator;
    bool_t     submenu;
} axau_menu_item_t;

typedef struct axau_menu {
    gui_rect_t     rect;
    axau_menu_item_t items[8];
    i32            count;
    i32            hover;
    i32            selected;
    bool_t         open;
    i32            anim;
} axau_menu_t;

void axau_dialog_open(axau_dialog_t *dialog, const char *title, const char *body,
                      const char *primary, const char *secondary);
void axau_dialog_paint(gui_surface_t *surface, axau_dialog_t *dialog,
                       const axau_palette_t *palette, const axau_metrics_t *metrics);
i32  axau_dialog_hit(axau_dialog_t *dialog, i32 x, i32 y);

void axau_flyout_open(axau_flyout_t *flyout, i32 anchor_x, i32 anchor_y,
                      i32 width, i32 height, i32 screen_w, i32 screen_h);
void axau_flyout_paint(gui_surface_t *surface, axau_flyout_t *flyout,
                       const axau_palette_t *palette);

void axau_tooltip_paint(gui_surface_t *surface, axau_tooltip_t *tooltip,
                        const axau_palette_t *palette);
void axau_tooltip_tick(axau_tooltip_t *tooltip, i32 dt_ms);

void axau_menu_init(axau_menu_t *menu, i32 x, i32 y);
void axau_menu_add(axau_menu_t *menu, const char *label, const char *shortcut,
                   const char *glyph);
void axau_menu_add_separator(axau_menu_t *menu);
void axau_menu_paint(gui_surface_t *surface, axau_menu_t *menu,
                     const axau_palette_t *palette);
i32  axau_menu_hit(axau_menu_t *menu, i32 x, i32 y);

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

i32  axau_text_center_y(gui_rect_t rect, u8 scale);
void axau_label(gui_surface_t *surface, gui_rect_t rect, const char *text,
                u32 colour, u8 scale);
void axau_label_right(gui_surface_t *surface, gui_rect_t rect, const char *text,
                      u32 colour, u8 scale);
void axau_icon_glyph(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                     const char *glyph, u32 colour);
void axau_rounded_clip_note(gui_surface_t *surface, gui_rect_t rect, i32 radius);

/* Global design-system state, initialised once by the shell. */
typedef struct axau_context {
    axau_palette_t palette;
    axau_metrics_t metrics;
    bool_t         reduced_motion;
    bool_t         high_contrast;
    bool_t         initialised;
    u32            frame;
    i32            pointer_x;
    i32            pointer_y;
} axau_context_t;

axau_context_t *axau_context(void);
void axau_init(i32 dpi, u32 accent, axau_theme_kind_t kind);
void axau_set_theme(axau_theme_kind_t kind, u32 accent);
void axau_set_reduced_motion(bool_t reduced);
void axau_begin_frame(i32 pointer_x, i32 pointer_y);
void axau_end_frame(void);

#endif /* AXIOM_AURORA_H */

/* Vector icon vocabulary, drawn procedurally at paint time. */
void axau_icon_paint(gui_surface_t *surface, const char *token, i32 cx, i32 cy,
                     i32 size, u32 colour);
void axau_icon_label(gui_surface_t *surface, gui_rect_t rect, const char *token,
                     const char *label, u32 colour);

