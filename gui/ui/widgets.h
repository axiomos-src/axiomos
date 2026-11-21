/* AxiomOS UI - widget state layer.
 *
 * The first-generation widgets in gui.h are immediate-mode: every call paints
 * and returns.  This layer adds the state a real form needs - focus, hover,
 * press, caret position, selection range, scroll offset, animation progress -
 * while keeping painting as a pure function of that state.
 *
 * The convention is the same for every control:
 *
 *   - `*_init` fills a caller-owned state block with defaults;
 *   - `*_paint` draws from the state and the theme, and never mutates state
 *     except for animation easing;
 *   - `*_event` processes a pointer or key event and returns a bitmask of
 *     widget events, so the caller decides what a click means.
 *
 * Returning events rather than invoking callbacks keeps the shell free of
 * per-widget function pointers, which matters when every byte of BSS is
 * accounted for.
 */
#ifndef AXIOM_UI_WIDGETS_H
#define AXIOM_UI_WIDGETS_H

#include "ui.h"

/* ------------------------------------------------------------------ */
/* Events and flags                                                    */
/* ------------------------------------------------------------------ */

#define AXW_NONE        0x0000u
#define AXW_HOVER       0x0001u
#define AXW_PRESS       0x0002u
#define AXW_CLICK       0x0004u
#define AXW_RELEASE     0x0008u
#define AXW_TOGGLE      0x0010u
#define AXW_FOCUS       0x0020u
#define AXW_BLUR        0x0040u
#define AXW_CHANGED     0x0080u
#define AXW_CANCELLED   0x0100u
#define AXW_SCROLL      0x0200u
#define AXW_SELECT      0x0400u
#define AXW_OPEN        0x0800u
#define AXW_CLOSE       0x1000u
#define AXW_KEY         0x2000u

/* Visual states, resolved from the interaction state and the theme. */
typedef enum axw_state {
    AXW_STATE_NORMAL = 0,
    AXW_STATE_HOVER,
    AXW_STATE_PRESSED,
    AXW_STATE_FOCUSED,
    AXW_STATE_DISABLED,
    AXW_STATE_CHECKED,
    AXW_STATE_INDETERMINATE,
} axw_state_t;

/* ------------------------------------------------------------------ */
/* Button                                                              */
/* ------------------------------------------------------------------ */

typedef struct axw_button {
    gui_rect_t  rect;
    const char *label;
    const char *glyph;        /* optional single character icon */
    bool_t      enabled;
    bool_t      hover;
    bool_t      pressed;
    bool_t      focused;
    bool_t      primary;      /* filled with the accent */
    bool_t      ghost;        /* text only */
    bool_t      danger;
    i32         radius;
} axw_button_t;

void axw_button_init(axw_button_t *button, gui_rect_t rect, const char *label);
void axw_button_paint(gui_surface_t *surface, const axw_button_t *button);
u32  axw_button_event(axw_button_t *button, i32 x, i32 y, u8 buttons, bool_t pressed);
void axw_button_set_primary(axw_button_t *button, bool_t primary);
void axw_button_set_enabled(axw_button_t *button, bool_t enabled);

/* Icon button: a square target with a single glyph, used by the title bar. */
typedef struct axw_icon_button {
    gui_rect_t rect;
    const char *glyph;
    u32        hover_fill;
    u32        glyph_colour;
    bool_t     hover;
    bool_t     pressed;
    bool_t     close_style;
} axw_icon_button_t;

void axw_icon_button_init(axw_icon_button_t *button, gui_rect_t rect, const char *glyph);
void axw_icon_button_paint(gui_surface_t *surface, const axw_icon_button_t *button);
u32  axw_icon_button_event(axw_icon_button_t *button, i32 x, i32 y, u8 buttons,
                           bool_t pressed);

/* ------------------------------------------------------------------ */
/* Checkbox, radio, switch                                             */
/* ------------------------------------------------------------------ */

typedef struct axw_checkbox {
    gui_rect_t  rect;
    const char *label;
    bool_t      checked;
    bool_t      indeterminate;
    bool_t      hover;
    bool_t      pressed;
    bool_t      focused;
    bool_t      enabled;
} axw_checkbox_t;

void axw_checkbox_init(axw_checkbox_t *box, gui_rect_t rect, const char *label,
                       bool_t checked);
void axw_checkbox_paint(gui_surface_t *surface, const axw_checkbox_t *box);
u32  axw_checkbox_event(axw_checkbox_t *box, i32 x, i32 y, u8 buttons, bool_t pressed);

typedef struct axw_radio {
    gui_rect_t  rect;
    const char *label;
    bool_t      selected;
    bool_t      hover;
    bool_t      focused;
    bool_t      enabled;
} axw_radio_t;

void axw_radio_init(axw_radio_t *radio, gui_rect_t rect, const char *label,
                    bool_t selected);
void axw_radio_paint(gui_surface_t *surface, const axw_radio_t *radio);
u32  axw_radio_event(axw_radio_t *radio, i32 x, i32 y, u8 buttons, bool_t pressed);

/* A radio group paints and dispatches a row of options at once. */
typedef struct axw_radio_group {
    axw_radio_t items[8];
    i32         count;
    i32         selected;
    bool_t      horizontal;
    i32         gap;
} axw_radio_group_t;

void axw_radio_group_init(axw_radio_group_t *group, gui_rect_t rect, i32 count,
                          bool_t horizontal, i32 gap);
void axw_radio_group_label(axw_radio_group_t *group, i32 index, const char *label);
void axw_radio_group_paint(gui_surface_t *surface, const axw_radio_group_t *group);
u32  axw_radio_group_event(axw_radio_group_t *group, i32 x, i32 y, u8 buttons,
                           bool_t pressed);

typedef struct axw_switch {
    gui_rect_t  rect;
    const char *label;
    bool_t      on;
    bool_t      hover;
    bool_t      focused;
    bool_t      enabled;
    i32         anim;        /* 0..100, eased towards on/off */
} axw_switch_t;

void axw_switch_init(axw_switch_t *toggle, gui_rect_t rect, const char *label,
                     bool_t on);
void axw_switch_paint(gui_surface_t *surface, const axw_switch_t *toggle);
u32  axw_switch_event(axw_switch_t *toggle, i32 x, i32 y, u8 buttons, bool_t pressed);
void axw_switch_animate(axw_switch_t *toggle, i32 step);

/* ------------------------------------------------------------------ */
/* Slider and progress                                                 */
/* ------------------------------------------------------------------ */

typedef struct axw_slider {
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
} axw_slider_t;

void axw_slider_init(axw_slider_t *slider, gui_rect_t rect, i32 minimum,
                     i32 maximum, i32 value);
void axw_slider_paint(gui_surface_t *surface, const axw_slider_t *slider);
u32  axw_slider_event(axw_slider_t *slider, i32 x, i32 y, u8 buttons, bool_t pressed);
void axw_slider_set_value(axw_slider_t *slider, i32 value);

typedef struct axw_progress {
    gui_rect_t rect;
    i32        percent;
    i32        displayed;     /* eased towards percent */
    bool_t     indeterminate;
    bool_t     striped;
    u32        fill;
    u32        track;
    i32        phase;         /* stripes animation offset */
} axw_progress_t;

void axw_progress_init(axw_progress_t *bar, gui_rect_t rect, i32 percent);
void axw_progress_paint(gui_surface_t *surface, const axw_progress_t *bar);
void axw_progress_set(axw_progress_t *bar, i32 percent);
void axw_progress_animate(axw_progress_t *bar, i32 step);

/* A spinner is the indeterminate cousin: an arc that rotates. */
typedef struct axw_spinner {
    i32    cx;
    i32    cy;
    i32    radius;
    i32    angle;
    u32    colour;
    bool_t running;
} axw_spinner_t;

void axw_spinner_init(axw_spinner_t *spinner, i32 cx, i32 cy, i32 radius);
void axw_spinner_paint(gui_surface_t *surface, const axw_spinner_t *spinner);
void axw_spinner_advance(axw_spinner_t *spinner, i32 degrees);

/* ------------------------------------------------------------------ */
/* Text field                                                          */
/* ------------------------------------------------------------------ */

#define AXW_TEXT_MAX 128

typedef struct axw_text_field {
    gui_rect_t  rect;
    char        text[AXW_TEXT_MAX];
    i32         length;
    i32         cursor;          /* byte offset */
    i32         selection;       /* anchor, -1 when there is no selection */
    i32         scroll;          /* horizontal scroll in pixels */
    const char *placeholder;
    bool_t      focused;
    bool_t      hover;
    bool_t      enabled;
    bool_t      password;
    bool_t      readonly;
    i32         max_length;
    u32         changed_at;      /* frame of the last edit, for the caret */
} axw_text_field_t;

void axw_text_field_init(axw_text_field_t *field, gui_rect_t rect,
                         const char *placeholder);
void axw_text_field_paint(gui_surface_t *surface, axw_text_field_t *field);
u32  axw_text_field_event(axw_text_field_t *field, i32 x, i32 y, u8 buttons,
                          bool_t pressed);
u32  axw_text_field_key(axw_text_field_t *field, char key, u8 scancode);
void axw_text_field_set(axw_text_field_t *field, const char *text);
const char *axw_text_field_get(const axw_text_field_t *field);
bool_t axw_text_field_has_selection(const axw_text_field_t *field);
i32  axw_text_field_selection_start(const axw_text_field_t *field);
i32  axw_text_field_selection_end(const axw_text_field_t *field);

/* Multi-line text area: the same editing rules, with wrapping and a
 * vertical scroll offset. */
typedef struct axw_text_area {
    gui_rect_t rect;
    char      *text;
    i32        capacity;
    i32        length;
    i32        cursor;
    i32        scroll;
    i32        line_height;
    bool_t     focused;
    bool_t     enabled;
    bool_t     wrap;
} axw_text_area_t;

void axw_text_area_init(axw_text_area_t *area, gui_rect_t rect, char *buffer,
                        i32 capacity);
void axw_text_area_paint(gui_surface_t *surface, axw_text_area_t *area);
u32  axw_text_area_key(axw_text_area_t *area, char key, u8 scancode);

/* ------------------------------------------------------------------ */
/* List box and table                                                  */
/* ------------------------------------------------------------------ */

typedef struct axw_list_item {
    const char *label;
    const char *detail;
    const char *glyph;
    bool_t      disabled;
    bool_t      checked;
} axw_list_item_t;

#define AXW_LIST_MAX 32

typedef struct axw_list {
    gui_rect_t      rect;
    axw_list_item_t items[AXW_LIST_MAX];
    i32             count;
    i32             selected;
    i32             hover;
    i32             row_height;
    i32             scroll;
    bool_t          focused;
    bool_t          show_detail;
} axw_list_t;

void axw_list_init(axw_list_t *list, gui_rect_t rect, i32 row_height);
i32  axw_list_add(axw_list_t *list, const char *label, const char *detail,
                  const char *glyph);
void axw_list_paint(gui_surface_t *surface, axw_list_t *list);
u32  axw_list_event(axw_list_t *list, i32 x, i32 y, u8 buttons, bool_t pressed);
u32  axw_list_key(axw_list_t *list, char key, u8 scancode);
void axw_list_scroll_to(axw_list_t *list, i32 index);
void axw_list_set_detail(axw_list_t *list, bool_t show);

#define AXW_TABLE_COLS 6

typedef struct axw_table {
    gui_rect_t  rect;
    const char *headers[AXW_TABLE_COLS];
    i32         column_width[AXW_TABLE_COLS];
    i32         columns;
    const char *cells[AXW_TABLE_COLS][AXW_LIST_MAX];
    i32         rows;
    i32         selected;
    i32         row_height;
    i32         scroll;
    bool_t      striped;
    bool_t      focused;
} axw_table_t;

void axw_table_init(axw_table_t *table, gui_rect_t rect, i32 columns, i32 row_height);
void axw_table_header(axw_table_t *table, i32 column, const char *text, i32 width);
u32  axw_table_key(axw_table_t *table, u8 scancode);
void axw_table_cell(axw_table_t *table, i32 row, i32 column, const char *text);
void axw_table_paint(gui_surface_t *surface, axw_table_t *table);
u32  axw_table_event(axw_table_t *table, i32 x, i32 y, u8 buttons, bool_t pressed);

/* ------------------------------------------------------------------ */
/* Tabs, menu, toolbar, status bar                                     */
/* ------------------------------------------------------------------ */

typedef struct axw_tabs {
    gui_rect_t  rect;
    const char *labels[8];
    i32         count;
    i32         selected;
    i32         hover;
    i32         tab_width;
    bool_t      underline;
} axw_tabs_t;

void axw_tabs_init(axw_tabs_t *tabs, gui_rect_t rect, i32 tab_width);
i32  axw_tabs_add(axw_tabs_t *tabs, const char *label);
void axw_tabs_paint(gui_surface_t *surface, const axw_tabs_t *tabs);
u32  axw_tabs_event(axw_tabs_t *tabs, i32 x, i32 y, u8 buttons, bool_t pressed);
gui_rect_t axw_tabs_content(const axw_tabs_t *tabs);

typedef struct axw_menu_item {
    const char *label;
    const char *shortcut;
    const char *glyph;
    bool_t      separator;
    bool_t      checked;
    bool_t      enabled;
} axw_menu_item_t;

typedef struct axw_menu {
    gui_rect_t     rect;
    axw_menu_item_t items[16];
    i32            count;
    i32            hover;
    i32            highlighted;
    bool_t         open;
    i32            item_height;
} axw_menu_t;

void axw_menu_init(axw_menu_t *menu, gui_rect_t rect, i32 item_height);
i32  axw_menu_add(axw_menu_t *menu, const char *label, const char *shortcut,
                  const char *glyph);
i32  axw_menu_separator(axw_menu_t *menu);
void axw_menu_paint(gui_surface_t *surface, const axw_menu_t *menu);
u32  axw_menu_event(axw_menu_t *menu, i32 x, i32 y, u8 buttons, bool_t pressed);

typedef struct axw_toast {
    gui_rect_t  rect;
    const char *text;
    i32         level;         /* 0 info, 1 ok, 2 warn, 3 error */
    i32         life;          /* frames remaining */
    i32         total;
} axw_toast_t;

void axw_toast_init(axw_toast_t *toast, gui_rect_t rect, const char *text, i32 level);
void axw_toast_paint(gui_surface_t *surface, const axw_toast_t *toast);
bool_t axw_toast_tick(axw_toast_t *toast);

/* Tooltip attached to a hovered rect. */
void axw_tooltip_paint(gui_surface_t *surface, gui_rect_t anchor, const char *text);
gui_rect_t axw_tooltip_measure(gui_rect_t anchor, const char *text);

/* ------------------------------------------------------------------ */
/* Scrollbar                                                           */
/* ------------------------------------------------------------------ */

typedef struct axw_scrollbar {
    gui_rect_t rect;
    i32        offset;
    i32        total;
    i32        visible;
    bool_t     vertical;
    bool_t     dragging;
    bool_t     hover;
    i32        drag_origin;
    i32        drag_offset;
} axw_scrollbar_t;

void axw_scrollbar_init(axw_scrollbar_t *bar, gui_rect_t rect, bool_t vertical);
void axw_scrollbar_paint(gui_surface_t *surface, const axw_scrollbar_t *bar);
u32  axw_scrollbar_event(axw_scrollbar_t *bar, i32 x, i32 y, u8 buttons, bool_t pressed);
void axw_scrollbar_config(axw_scrollbar_t *bar, i32 total, i32 visible, i32 offset);

/* ------------------------------------------------------------------ */
/* Panel, card, badge, divider                                         */
/* ------------------------------------------------------------------ */

void axw_panel_paint(gui_surface_t *surface, gui_rect_t rect, i32 radius);
void axw_card_paint(gui_surface_t *surface, gui_rect_t rect, i32 radius);
void axw_divider_paint(gui_surface_t *surface, gui_rect_t rect);
void axw_badge_paint(gui_surface_t *surface, gui_rect_t rect, const char *text,
                     u32 fill, u32 text_colour);
void axw_chip_paint(gui_surface_t *surface, gui_rect_t rect, const char *text,
                    const char *glyph);
void axw_section_header_paint(gui_surface_t *surface, gui_rect_t rect,
                              const char *title, const char *subtitle);
void axw_empty_state_paint(gui_surface_t *surface, gui_rect_t rect,
                           const char *glyph, const char *title,
                           const char *detail);

/* ------------------------------------------------------------------ */
/* Focus manager                                                       */
/* ------------------------------------------------------------------ */

#define AXW_FOCUS_MAX 24

typedef struct axw_focus {
    void *items[AXW_FOCUS_MAX];
    i32   count;
    i32   index;
    bool_t wrap;
} axw_focus_t;

void axw_focus_init(axw_focus_t *focus);
void axw_focus_add(axw_focus_t *focus, void *item);
void axw_focus_clear(axw_focus_t *focus);
void *axw_focus_next(axw_focus_t *focus);
void *axw_focus_prev(axw_focus_t *focus);
void *axw_focus_current(const axw_focus_t *focus);
i32  axw_focus_find(const axw_focus_t *focus, const void *item);

/* ------------------------------------------------------------------ */
/* Hit testing                                                         */
/* ------------------------------------------------------------------ */

bool_t axw_hit(gui_rect_t rect, i32 x, i32 y);
i32    axw_hit_row(gui_rect_t rect, i32 y, i32 row_height, i32 scroll);
i32    axw_hit_column(gui_rect_t rect, i32 x, const i32 *widths, i32 count);
void   axw_focus_set(axw_focus_t *focus, void *item);

/* Editing shortcuts and selection export for the text field. */
u32    axw_text_field_control(axw_text_field_t *field, char code);
usize  axw_text_field_copy(const axw_text_field_t *field, char *buffer, usize cap);
void   axw_text_area_scroll(axw_text_area_t *area, i32 lines);
i32    axw_text_area_lines(const axw_text_area_t *area);

/* Visible-row and content helpers used by callers that own a scrollbar. */
i32  axw_list_visible_rows(const axw_list_t *list);
i32  axw_list_content_height(const axw_list_t *list);
void axw_list_scroll_to(axw_list_t *list, i32 index);
void axw_list_set_detail(axw_list_t *list, bool_t show);
u32  axw_list_key(axw_list_t *list, char key, u8 scancode);

void axw_table_header(axw_table_t *table, i32 column, const char *text, i32 width);
u32  axw_table_key(axw_table_t *table, u8 scancode);
void axw_table_cell(axw_table_t *table, i32 row, i32 column, const char *text);
gui_rect_t axw_tabs_content(const axw_tabs_t *tabs);
i32  axw_tabs_add(axw_tabs_t *tabs, const char *label);
i32  axw_menu_add(axw_menu_t *menu, const char *label, const char *shortcut,
                  const char *glyph);
i32  axw_menu_separator(axw_menu_t *menu);

/* ------------------------------------------------------------------ */
/* Tree view                                                           */
/* ------------------------------------------------------------------ */

typedef struct axw_tree_row {
    const char *label;
    i32         depth;
    bool_t      expanded;
    bool_t      leaf;
} axw_tree_row_t;

typedef struct axw_tree {
    gui_rect_t     rect;
    axw_tree_row_t rows[AXW_LIST_MAX];
    i32            count;
    i32            selected;
    i32            row_height;
    i32            scroll;
} axw_tree_t;

void axw_tree_init(axw_tree_t *tree, gui_rect_t rect, i32 row_height);
i32  axw_tree_add(axw_tree_t *tree, const char *label, i32 depth, bool_t expanded,
                  bool_t leaf);
void axw_tree_paint(gui_surface_t *surface, axw_tree_t *tree);
u32  axw_tree_event(axw_tree_t *tree, i32 x, i32 y, u8 buttons, bool_t pressed);


/* ------------------------------------------------------------------ */
/* Registry, trace and enable mask                                     */
/* ------------------------------------------------------------------ */

#define AXW_REGISTRY_MAX 64
#define AXW_TRACE_MAX 48

typedef struct axw_registry_entry {
    i32         id;
    gui_rect_t  rect;
    u32         state;
    bool_t      enabled;
    bool_t      visible;
    const char *name;
} axw_registry_entry_t;

typedef struct axw_registry {
    axw_registry_entry_t entries[AXW_REGISTRY_MAX];
    i32                  count;
    u32                  hit_tests;
    u32                  misses;
} axw_registry_t;

typedef struct axw_trace_record {
    u32  frame;
    i32  id;
    u32  events;
    u8   from;
    u8   to;
} axw_trace_record_t;

typedef struct axw_trace {
    axw_trace_record_t records[AXW_TRACE_MAX];
    i32                head;
    i32                count;
    u32                frame;
} axw_trace_t;

#define AXW_DISABLED_WORDS 4

typedef struct axw_enable_mask {
    u32 words[AXW_DISABLED_WORDS];
} axw_enable_mask_t;

const char *axw_state_name(axw_state_t state);
void  axw_event_name(u32 events, char *buffer, usize cap);
void  axw_registry_init(axw_registry_t *registry);
i32   axw_registry_register(axw_registry_t *registry, i32 id, gui_rect_t rect,
                            const char *name);
axw_registry_entry_t *axw_registry_find(axw_registry_t *registry, i32 id);
void  axw_registry_move(axw_registry_t *registry, i32 id, gui_rect_t rect);
void  axw_registry_enable(axw_registry_t *registry, i32 id, bool_t enabled);
i32   axw_registry_hit(axw_registry_t *registry, i32 x, i32 y);
void  axw_registry_apply(axw_registry_t *registry, i32 id, u32 events);
i32   axw_registry_count(const axw_registry_t *registry);
void  axw_trace_init(axw_trace_t *trace);
void  axw_trace_record(axw_trace_t *trace, i32 id, u32 events, axw_state_t from,
                       axw_state_t to);
void  axw_trace_advance(axw_trace_t *trace, u32 frames);
const axw_trace_record_t *axw_trace_at(const axw_trace_t *trace, i32 index);
void  axw_trace_format(const axw_trace_record_t *record, char *buffer, usize cap);
bool_t axw_is_tab_key(char key, u8 scancode);
bool_t axw_is_shift_tab(char key, u8 scancode, bool_t shift);
i32   axw_nav_pick(const gui_rect_t *rects, i32 count, i32 current, i32 direction);
void  axw_mask_clear(axw_enable_mask_t *mask);
void  axw_mask_disable(axw_enable_mask_t *mask, i32 id);
void  axw_mask_enable(axw_enable_mask_t *mask, i32 id);
bool_t axw_mask_is_disabled(const axw_enable_mask_t *mask, i32 id);
#endif /* AXIOM_UI_WIDGETS_H */
