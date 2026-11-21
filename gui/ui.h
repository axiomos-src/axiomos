/* AxiomOS - extended UI toolkit interface.
 *
 * The first-generation GUI (gui/gui.h) has a small immediate-mode widget set
 * that is enough for the splash screen and a handful of windows.  This header
 * describes the second layer: precise geometry, anti-aliased rasterisation,
 * colour management, text layout, a small layout engine and the theme/metrics
 * tables that the terminal and the newer applications are built on.
 *
 * Naming: everything here is prefixed axui_ so that the older gui_* symbols
 * keep their meaning and both layers can be linked into the same kernel.
 */
#ifndef AXIOM_UI_H
#define AXIOM_UI_H

#include <axiom/types.h>
#include "gui.h"

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

typedef struct axui_point {
    i32 x;
    i32 y;
} axui_point_t;

typedef struct axui_size {
    i32 w;
    i32 h;
} axui_size_t;

typedef struct axui_edge {
    i32 left;
    i32 top;
    i32 right;
    i32 bottom;
} axui_edge_t;

axui_point_t   axui_point(i32 x, i32 y);
axui_size_t    axui_size(i32 w, i32 h);
axui_edge_t    axui_edge(i32 all);
axui_edge_t    axui_edge_trbl(i32 top, i32 right, i32 bottom, i32 left);
gui_rect_t     axui_rect_inset(gui_rect_t rect, axui_edge_t edge);
gui_rect_t     axui_rect_inset_px(gui_rect_t rect, i32 px);
gui_rect_t     axui_rect_union(gui_rect_t a, gui_rect_t b);
bool_t         axui_rect_empty(gui_rect_t rect);
bool_t         axui_rect_overlaps(gui_rect_t a, gui_rect_t b);
gui_rect_t     axui_rect_center_in(gui_rect_t outer, i32 w, i32 h);
i32            axui_rect_area(gui_rect_t rect);
gui_rect_t     axui_rect_split_left(gui_rect_t *rect, i32 amount);
gui_rect_t     axui_rect_split_right(gui_rect_t *rect, i32 amount);
gui_rect_t     axui_rect_split_top(gui_rect_t *rect, i32 amount);
gui_rect_t     axui_rect_split_bottom(gui_rect_t *rect, i32 amount);
i32            axui_clamp_i32(i32 v, i32 lo, i32 hi);
i32            axui_min_i32(i32 a, i32 b);
i32            axui_max_i32(i32 a, i32 b);

/* ------------------------------------------------------------------ */
/* Damage tracking                                                     */
/* ------------------------------------------------------------------ */

#define AXUI_DAMAGE_MAX 32

typedef struct axui_damage {
    gui_rect_t rects[AXUI_DAMAGE_MAX];
    u32        count;
    bool_t     full;
} axui_damage_t;

void       axui_damage_reset(axui_damage_t *damage);
void       axui_damage_add(axui_damage_t *damage, gui_rect_t rect);
void       axui_damage_add_px(axui_damage_t *damage, i32 x, i32 y, i32 w, i32 h);
void       axui_damage_merge(axui_damage_t *dst, const axui_damage_t *src);
bool_t     axui_damage_empty(const axui_damage_t *damage);
gui_rect_t axui_damage_bounds(const axui_damage_t *damage);

/* ------------------------------------------------------------------ */
/* Colour                                                              */
/* ------------------------------------------------------------------ */

typedef struct axui_hsv {
    i32 h;      /* 0..359 */
    i32 s;      /* 0..100 */
    i32 v;      /* 0..100 */
} axui_hsv_t;

u32        axui_rgba(u8 r, u8 g, u8 b, u8 a);
u32        axui_colour_alpha(u32 colour, u8 alpha);
u32        axui_colour_opacity(u32 colour, i32 percent);
u8         axui_alpha_of(u32 colour);
u32        axui_blend(u32 dst, u32 src);
u32        axui_blend_alpha(u32 dst, u32 src, u8 alpha);
u32        axui_lerp_colour(u32 a, u32 b, i32 t);      /* t in 0..100 */
u32        axui_lighten(u32 colour, i32 percent);
u32        axui_darken(u32 colour, i32 percent);
u32        axui_saturate(u32 colour, i32 percent);
axui_hsv_t axui_rgb_to_hsv(u32 colour);
u32        axui_hsv_to_rgb(axui_hsv_t hsv);
i32        axui_luma(u32 colour);
u32        axui_contrast_text(u32 background);
bool_t     axui_is_dark(u32 colour);

/* WCAG contrast in hundredths (403 means 4.03:1) and the repair helper that
 * nudges a foreground until it reaches the 4.5:1 threshold. */
i32        axui_contrast_ratio(u32 a, u32 b);
u32        axui_ensure_contrast(u32 fg, u32 bg);

/* Parsing and formatting: "#RRGGBB", "#AARRGGBB", "rgb(r,g,b)", "rgba(...)"
 * and a table of CSS colour names.  axui_parse_colour returns 0 on failure. */
u32         axui_parse_colour(const char *text);
const char *axui_format_colour(u32 colour, char *buffer, usize cap);
const char *axui_colour_name(u32 colour);

/* Derived colours used by the widget painters. */
u32        axui_tint(u32 base, u32 background, i32 weight);
u32        axui_fade(u32 colour, i32 percent);
u32        axui_shift(u32 colour, i32 delta);
u32        axui_disabled(u32 colour);
u32        axui_on_colour(u32 background);
u32        axui_hairline(u32 background);
u32        axui_shadow_colour(u32 base, i32 strength);
u32        axui_status_colour(i32 status);
const char *axui_status_name(i32 status);
u32        axui_alpha_backdrop(i32 x, i32 y, i32 cell);
u32        axui_palette_step(u32 accent, i32 step);
u32        axui_chart_colour(i32 index);
u32        axui_ramp(const u32 *stops, i32 count, i32 t);
u32        axui_average(const u32 *colours, i32 count);

/* ------------------------------------------------------------------ */
/* Rasterisation                                                       */
/* ------------------------------------------------------------------ */

void axui_fill_triangle(gui_surface_t *surface, axui_point_t a, axui_point_t b,
                        axui_point_t c, u32 colour);
void axui_line_aa(gui_surface_t *surface, i32 x0, i32 y0, i32 x1, i32 y1,
                  u32 colour, u8 weight);
void axui_circle(gui_surface_t *surface, i32 cx, i32 cy, i32 radius, u32 colour);
void axui_circle_outline(gui_surface_t *surface, i32 cx, i32 cy, i32 radius,
                         i32 thickness, u32 colour);
void axui_ellipse(gui_surface_t *surface, i32 cx, i32 cy, i32 rx, i32 ry,
                  u32 colour);
void axui_arc(gui_surface_t *surface, i32 cx, i32 cy, i32 radius,
              i32 start_deg, i32 sweep_deg, i32 thickness, u32 colour);
void axui_quadratic(gui_surface_t *surface, axui_point_t p0, axui_point_t p1,
                    axui_point_t p2, u32 colour, i32 steps);
void axui_cubic(gui_surface_t *surface, axui_point_t p0, axui_point_t p1,
                axui_point_t p2, axui_point_t p3, u32 colour, i32 steps);
void axui_round_rect_aa(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                        u32 colour);
void axui_round_rect_border(gui_surface_t *surface, gui_rect_t rect, i32 radius,
                            i32 thickness, u32 colour);
void axui_gradient_linear(gui_surface_t *surface, gui_rect_t rect, i32 angle_deg,
                          u32 from, u32 to);
void axui_gradient_radial(gui_surface_t *surface, i32 cx, i32 cy, i32 radius,
                          u32 inner, u32 outer);
void axui_checkerboard(gui_surface_t *surface, gui_rect_t rect, i32 cell,
                       u32 a, u32 b);
void axui_grid_pattern(gui_surface_t *surface, gui_rect_t rect, i32 step,
                       u32 colour);
void axui_vignette(gui_surface_t *surface, u8 strength);
void axui_scanlines(gui_surface_t *surface, u8 strength);

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

typedef struct axui_text_style {
    u32 colour;
    u8  scale;
    bool_t bold;        /* draw the glyph offset by one pixel */
    bool_t underline;
    u32 underline_colour;
} axui_text_style_t;

axui_text_style_t axui_style(u32 colour, u8 scale);
i32  axui_char_width(u8 scale);
i32  axui_line_height(u8 scale);
i32  axui_measure_text(const char *text, u8 scale);
i32  axui_measure_text_bold(const char *text, u8 scale);
void axui_draw_text(gui_surface_t *surface, i32 x, i32 y, const char *text,
                    axui_text_style_t style);
void axui_draw_text_clip(gui_surface_t *surface, gui_rect_t clip, i32 x, i32 y,
                         const char *text, axui_text_style_t style);
i32  axui_draw_text_wrapped(gui_surface_t *surface, gui_rect_t box, const char *text,
                            axui_text_style_t style);
void axui_draw_text_ellipsis(gui_surface_t *surface, gui_rect_t box, const char *text,
                             axui_text_style_t style);
void axui_draw_text_right(gui_surface_t *surface, gui_rect_t box, const char *text,
                          axui_text_style_t style);
void axui_draw_text_center(gui_surface_t *surface, gui_rect_t box, const char *text,
                           axui_text_style_t style);
void axui_wrap_lines(const char *text, i32 width_px, u8 scale,
                     i32 (*out_starts)[2], i32 *out_count, i32 max_lines);
void axui_draw_text_selection(gui_surface_t *surface, gui_rect_t line_rect,
                              i32 x0, i32 x1, u32 colour);

/* UTF-8 helpers.  The built-in face covers printable ASCII only, so the
 * decoders exist mainly to keep measurement and caret maths byte-correct. */
i32        axui_utf8_decode(const char *text, u32 *out_code);
i32        axui_utf8_cells(const char *text);
usize      axui_utf8_length(const char *text);

/* Glyph access, including the synthesised box- and block-drawing set that the
 * terminal needs (0xB0..0xDF). */
u8         axui_glyph_row(u8 ch, i32 row);
bool_t     axui_glyph_synthetic(u8 ch, i32 row, u8 *bits);

/* Single-character advance and single-line drawing. */
i32        axui_advance(char ch, u8 scale);
i32        axui_draw_line(gui_surface_t *surface, i32 x, i32 y, const char *text,
                          axui_text_style_t style);
void       axui_draw_caret(gui_surface_t *surface, i32 x, i32 y, i32 height,
                           u8 scale, u32 colour);
void       axui_draw_caret_at(gui_surface_t *surface, gui_rect_t box, i32 column,
                              u8 scale, u32 colour, bool_t visible);
i32        axui_draw_paragraph(gui_surface_t *surface, gui_rect_t box, const char *text,
                               axui_text_style_t style, bool_t justify);

/* Measurement helpers used by layout and the text field. */
i32        axui_columns_for_width(i32 width_px, u8 scale);
i32        axui_rows_for_height(i32 height_px, u8 scale);
i32        axui_text_baseline(gui_rect_t box, u8 scale);
i32        axui_text_right_origin(gui_rect_t box, const char *text, u8 scale);
i32        axui_text_center_origin(gui_rect_t box, const char *text, u8 scale);
usize      axui_text_truncate(char *buffer, usize cap, i32 width_px, u8 scale);
i32        axui_column_of_offset(const char *text, usize offset, u8 scale);
usize      axui_offset_at_column(const char *text, i32 column, u8 scale);

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

typedef struct axui_box_spec {
    i32 width;          /* -1 means "fill parent" */
    i32 height;         /* -1 means "fill parent" */
    i32 grow;           /* flex weight, 0 = fixed */
    i32 basis;          /* preferred main-axis size */
    i32 min_width;
    i32 min_height;
    axui_edge_t margin;
    axui_edge_t padding;
} axui_box_spec_t;

axui_box_spec_t axui_box_fixed(i32 w, i32 h);
axui_box_spec_t axui_box_fill(void);
axui_box_spec_t axui_box_grow(i32 weight);
axui_box_spec_t axui_box_sized(i32 w, i32 h, i32 grow);
axui_box_spec_t axui_box_padded(axui_box_spec_t spec, axui_edge_t padding);
axui_box_spec_t axui_box_margined(axui_box_spec_t spec, axui_edge_t margin);
axui_box_spec_t axui_box_min(axui_box_spec_t spec, i32 min_w, i32 min_h);

typedef struct axui_layout_row {
    gui_rect_t   area;
    i32          cursor;
    i32          gap;
    axui_edge_t  padding;
    bool_t       vertical;
} axui_layout_t;

void       axui_layout_init(axui_layout_t *layout, gui_rect_t area, bool_t vertical,
                            i32 gap);
void       axui_layout_padding(axui_layout_t *layout, axui_edge_t padding);
gui_rect_t axui_layout_next(axui_layout_t *layout, const axui_box_spec_t *spec,
                            i32 total_children, i32 *out_grow_total);
gui_rect_t axui_layout_place(axui_layout_t *layout, i32 w, i32 h);
gui_rect_t axui_layout_place_grow(axui_layout_t *layout, const axui_box_spec_t *spec,
                                  i32 weight, i32 total_weight,
                                  i32 remaining_main, i32 cross_size);
i32        axui_layout_gap_total(i32 count, i32 gap);

/* Alignment, centring and stretching. */
gui_rect_t axui_layout_align(gui_rect_t container, i32 w, i32 h, i32 align_x,
                             i32 align_y, axui_edge_t margin);
gui_rect_t axui_layout_center(gui_rect_t container, i32 w, i32 h);
gui_rect_t axui_layout_stretch(gui_rect_t container, axui_edge_t margin);
gui_rect_t axui_rect_fit_aspect(gui_rect_t area, i32 ratio_w, i32 ratio_h);
i32        axui_flow_row(gui_rect_t area, const i32 *widths, i32 count, i32 item_h,
                         i32 gap, gui_rect_t *out);
i32        axui_stack_rows(gui_rect_t area, i32 row_height, i32 gap, i32 count,
                           gui_rect_t *out);
void       axui_dock_two(gui_rect_t area, i32 sidebar_w, i32 gap,
                         gui_rect_t *sidebar, gui_rect_t *content);

/* Grid of equally sized cells; `count` children are laid out row-major. */
typedef struct axui_grid {
    gui_rect_t  area;
    i32         columns;
    i32         rows;
    i32         gap;
    axui_edge_t padding;
} axui_grid_t;

axui_grid_t axui_grid_make(gui_rect_t area, i32 columns, i32 rows, i32 gap);
gui_rect_t  axui_grid_cell(const axui_grid_t *grid, i32 column, i32 row);
bool_t      axui_grid_position(const axui_grid_t *grid, i32 index, i32 *out_column,
                               i32 *out_row);

/* Split panes: equal or percentage weighted. */
typedef struct axui_panes {
    gui_rect_t area;
    i32        gap;
    i32        count;
    bool_t     vertical;
} axui_panes_t;

axui_panes_t axui_panes_make(gui_rect_t area, i32 count, bool_t vertical, i32 gap);
gui_rect_t   axui_panes_rect(const axui_panes_t *panes, i32 index);
gui_rect_t   axui_panes_weighted(const axui_panes_t *panes, i32 index,
                                 const i32 *weights, i32 weight_count);

/* Scrollable viewport shared by the list, table and terminal widgets. */
typedef struct axui_viewport {
    gui_rect_t visible;
    i32        content_w;
    i32        content_h;
    i32        offset_x;
    i32        offset_y;
} axui_viewport_t;

axui_viewport_t axui_viewport_make(gui_rect_t visible, i32 content_w, i32 content_h);
i32             axui_viewport_max_offset_y(const axui_viewport_t *viewport);
i32             axui_viewport_max_offset_x(const axui_viewport_t *viewport);
gui_rect_t      axui_viewport_rect(const axui_viewport_t *viewport, gui_rect_t content);
i32             axui_viewport_scroll(axui_viewport_t *viewport, i32 dx, i32 dy);
i32             axui_viewport_scroll_to(axui_viewport_t *viewport, i32 y,
                                        i32 row_height);
i32             axui_viewport_percent(const axui_viewport_t *viewport);

/* ------------------------------------------------------------------ */
/* Theme and metrics                                                   */
/* ------------------------------------------------------------------ */

typedef struct axui_metrics {
    i32 spacing_1;
    i32 spacing_2;
    i32 spacing_3;
    i32 spacing_4;
    i32 radius_small;
    i32 radius_medium;
    i32 radius_large;
    i32 border_width;
    i32 titlebar_height;
    i32 taskbar_height;
    i32 scrollbar_width;
    i32 scrollbar_min_thumb;
    i32 row_height;
    i32 caret_width;
    i32 caret_blink_ms;
    i32 tooltip_delay_ms;
    i32 double_click_ms;
    i32 animation_ms;
} axui_metrics_t;

const axui_metrics_t *axui_metrics(void);
const gui_theme_t    *axui_theme_dark(void);
const gui_theme_t    *axui_theme_light(void);
const gui_theme_t    *axui_theme_solarized(void);
const gui_theme_t    *axui_theme_high_contrast(void);
void                  axui_theme_set(const gui_theme_t *theme);
const gui_theme_t    *axui_theme_current(void);
i32                   axui_theme_count(void);
const char           *axui_theme_name(i32 index);
bool_t                axui_theme_select(const char *name);
void                  axui_theme_palette_dump(void (*sink)(const char *line));
i32                   axui_theme_index(void);
const char           *axui_theme_id(i32 index);

/* Accent override used by the settings application. */
void                  axui_set_accent(u32 accent);
void                  axui_clear_accent(void);
bool_t                axui_accent_overridden(void);

/* Derived widget colours: every control resolves its fill, hover and disabled
 * states through these so a theme change repaints consistently. */
u32                   axui_surface_raised(void);
u32                   axui_surface_sunken(void);
u32                   axui_surface_overlay(void);
u32                   axui_hover_tint(u32 base);
u32                   axui_press_tint(u32 base);
u32                   axui_focus_ring(void);
u32                   axui_disabled_text(void);
u32                   axui_disabled_fill(void);
u32                   axui_accent_text(void);
u32                   axui_title_gradient_mid(void);
u32                   axui_selection_colour(void);
u32                   axui_window_border(bool_t focused);
u32                   axui_desktop_top(void);
u32                   axui_desktop_bottom(void);
i32                   axui_shadow_strength(void);

/* Metrics and animation helpers. */
i32                   axui_control_height(void);
i32                   axui_spacing(i32 step);
i32                   axui_frames_for_ms(i32 ms, i32 frame_ms);
i32                   axui_ease_out(i32 t);
i32                   axui_ease_in_out(i32 t);
i32                   axui_approach(i32 current, i32 target, i32 step);


/* ------------------------------------------------------------------ */
/* Vector icons                                                        */
/* ------------------------------------------------------------------ */

i32         axui_icon_count(void);
const char *axui_icon_name(i32 index);
i32         axui_icon_find(const char *name);
void        axui_icon_draw(gui_surface_t *surface, i32 index, gui_rect_t rect,
                           u32 ink);
void        axui_icon_draw_named(gui_surface_t *surface, const char *name,
                                 gui_rect_t rect, u32 ink);
void        axui_icon_badge(gui_surface_t *surface, const char *name, gui_rect_t rect,
                            u32 ink, u32 badge_colour);
void        axui_icon_dump(gui_surface_t *surface, gui_rect_t rect, i32 per_row,
                           u32 ink);

/* ------------------------------------------------------------------ */
/* Animation                                                           */
/* ------------------------------------------------------------------ */

#define AXUI_EASE_LINEAR       0
#define AXUI_EASE_IN_QUAD      1
#define AXUI_EASE_OUT_QUAD     2
#define AXUI_EASE_IN_OUT_QUAD  3
#define AXUI_EASE_IN_CUBIC     4
#define AXUI_EASE_OUT_CUBIC    5
#define AXUI_EASE_STANDARD     6
#define AXUI_EASE_OUT_BACK     7
#define AXUI_EASE_OUT_ELASTIC  8
#define AXUI_EASE_OUT_BOUNCE   9

typedef struct axui_tween {
    i32    from;
    i32    to;
    i32    duration;
    i32    elapsed;
    i32    value;
    i32    ease;
    bool_t running;
} axui_tween_t;

typedef struct axui_rect_tween {
    axui_tween_t x;
    axui_tween_t y;
    axui_tween_t w;
    axui_tween_t h;
    i32          duration;
    i32          ease;
} axui_rect_tween_t;

typedef struct axui_spring {
    i32 value;
    i32 target;
    i32 velocity;
    i32 stiffness;
    i32 damping;
} axui_spring_t;

#define AXUI_TIMELINE_MAX 12

typedef void (*axui_stage_fn_t)(void *context, i32 progress);

typedef struct axui_timeline_stage {
    i32             duration;
    i32             elapsed;
    bool_t          started;
    bool_t          done;
    axui_stage_fn_t fn;
    void           *context;
} axui_timeline_stage_t;

typedef struct axui_timeline {
    axui_timeline_stage_t stages[AXUI_TIMELINE_MAX];
    i32                   count;
    i32                   current;
    bool_t                loop;
    bool_t                paused;
} axui_timeline_t;

typedef struct axui_clock {
    u64 last_tick;
    i32 last_delta;
    i32 frames;
    i32 dropped;
} axui_clock_t;

i32  axui_ease_linear(i32 t);
i32  axui_ease_in_quad(i32 t);
i32  axui_ease_out_quad(i32 t);
i32  axui_ease_in_out_quad(i32 t);
i32  axui_ease_in_cubic(i32 t);
i32  axui_ease_out_cubic(i32 t);
i32  axui_ease_in_out_cubic(i32 t);
i32  axui_ease_standard(i32 t);
i32  axui_ease_out_back(i32 t);
i32  axui_ease_out_elastic(i32 t);
i32  axui_ease_out_bounce(i32 t);
i32  axui_ease_steps(i32 t, i32 steps);
i32  axui_ease_by_id(i32 id, i32 t);
const char *axui_ease_name(i32 id);

void  axui_tween_init(axui_tween_t *tween, i32 from, i32 to, i32 duration_ms,
                      i32 ease);
void  axui_tween_to(axui_tween_t *tween, i32 target, i32 duration_ms);
i32   axui_tween_advance(axui_tween_t *tween, i32 dt);
i32   axui_tween_value(const axui_tween_t *tween);
bool_t axui_tween_done(const axui_tween_t *tween);
void  axui_tween_finish(axui_tween_t *tween);
i32   axui_tween_progress(const axui_tween_t *tween);

void       axui_rect_tween_init(axui_rect_tween_t *tween, gui_rect_t from,
                                i32 duration_ms, i32 ease);
void       axui_rect_tween_to(axui_rect_tween_t *tween, gui_rect_t to,
                              i32 duration_ms);
gui_rect_t axui_rect_tween_advance(axui_rect_tween_t *tween, i32 dt);
u32        axui_colour_tween(u32 from, u32 to, i32 progress);
u32        axui_fade(u32 colour, i32 progress);

void   axui_spring_init(axui_spring_t *spring, i32 value, i32 stiffness,
                        i32 damping);
void   axui_spring_to(axui_spring_t *spring, i32 target);
i32    axui_spring_step(axui_spring_t *spring, i32 frames);
bool_t axui_spring_settled(const axui_spring_t *spring);

void  axui_timeline_init(axui_timeline_t *timeline, bool_t loop);
i32   axui_timeline_add(axui_timeline_t *timeline, i32 duration_ms,
                        axui_stage_fn_t fn, void *context);
i32   axui_timeline_advance(axui_timeline_t *timeline, i32 dt);
bool_t axui_timeline_done(const axui_timeline_t *timeline);
void  axui_timeline_reset(axui_timeline_t *timeline);

void  axui_clock_init(axui_clock_t *clock);
i32   axui_clock_advance(axui_clock_t *clock, u64 ticks);
i32   axui_clock_fps(const axui_clock_t *clock);
#endif /* AXIOM_UI_H */
