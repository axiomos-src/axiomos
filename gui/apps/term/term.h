/* AxiomOS - terminal emulator subsystem.
 *
 * The shell's terminal is a real VT-style emulator rather than a text buffer:
 * it parses the same escape sequences a Linux program emits, keeps an
 * alternate screen, a scrollback ring, 16/256/true-colour SGR attributes,
 * DEC line-drawing, origin mode, tab stops and a selection model.  The
 * parser is a byte-fed state machine, so it works equally for output from the
 * kernel log, from the built-in shell and from a future user-space process.
 *
 * Cell storage is straight ARGB rather than a palette index, because the
 * compositor blends in that space anyway and true-colour programs (the
 * kernel's own `palette` command among them) would otherwise be quantised
 * twice.
 *
 * Sizing: the grid is a static array.  A statically sized terminal keeps the
 * kernel heap out of the hot path and the bound is generous enough for a 120x48
 * window at scale 1.
 */
#ifndef AXIOM_TERM_H
#define AXIOM_TERM_H

#include <axiom/types.h>
#include <axiom/lib.h>
#include "ui.h"

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define TERM_MAX_COLS       120
#define TERM_MAX_ROWS       48
#define TERM_DEFAULT_COLS   100
#define TERM_DEFAULT_ROWS   32
#define TERM_SCROLLBACK     160
#define TERM_MAX_PARAMS     16
#define TERM_MAX_OSC        256
#define TERM_TAB_EVERY      8
#define TERM_MAX_TABS       32
#define TERM_BLINK_MS       530

/* ------------------------------------------------------------------ */
/* Cell attributes                                                     */
/* ------------------------------------------------------------------ */

#define TERM_A_BOLD       0x01
#define TERM_A_DIM        0x02
#define TERM_A_ITALIC     0x04
#define TERM_A_UNDERLINE  0x08
#define TERM_A_BLINK      0x10
#define TERM_A_REVERSE    0x20
#define TERM_A_HIDDEN     0x40
#define TERM_A_STRIKE     0x80
#define TERM_A_WIDE_HEAD  0x01      /* same bit, only meaningful in `wide` */
#define TERM_A_WIDE_TAIL  0x02

/* One character cell.  `ch` is a Unicode code point; zero means "no glyph",
 * which the painter treats as a solid background block. */
typedef struct term_cell {
    u32 fg;
    u32 bg;
    u16 ch;
    u8  flags;
    u8  wide;      /* 0 = narrow, 1 = head of a double-width glyph, 2 = tail */
} term_cell_t;

/* ------------------------------------------------------------------ */
/* Cursor and modes                                                    */
/* ------------------------------------------------------------------ */

typedef struct term_cursor {
    i32 x;
    i32 y;
    u32 fg;
    u32 bg;
    u8  flags;
    bool_t visible;
    bool_t pending_wrap;      /* the "last column" deferred-wrap state */
} term_cursor_t;

#define TERM_MODE_CURSOR_KEYS   0x0001    /* DECCKM: application cursor keys */
#define TERM_MODE_ORIGIN        0x0002    /* DECOM: origin-relative addressing */
#define TERM_MODE_AUTOWRAP      0x0004    /* DECAWM */
#define TERM_MODE_INSERT        0x0008    /* IRM */
#define TERM_MODE_NEWLINE       0x0010    /* LNM */
#define TERM_MODE_ALT_SCREEN    0x0020    /* alternate buffer active */
#define TERM_MODE_BRACKET_PASTE 0x0040
#define TERM_MODE_MOUSE         0x0080
#define TERM_MODE_MOUSE_SGR     0x0100
#define TERM_MODE_FOCUS_EVENTS  0x0200
#define TERM_MODE_APP_KEYPAD    0x0400
#define TERM_MODE_REVERSE_VIDEO 0x0800    /* DECSCNM */
#define TERM_MODE_CURSOR_BLINK  0x1000

/* ------------------------------------------------------------------ */
/* Parser states                                                       */
/* ------------------------------------------------------------------ */

typedef enum term_parse_state {
    TERM_STATE_GROUND = 0,
    TERM_STATE_ESC,
    TERM_STATE_ESC_INTERMEDIATE,
    TERM_STATE_CSI_ENTRY,
    TERM_STATE_CSI_PARAM,
    TERM_STATE_CSI_INTERMEDIATE,
    TERM_STATE_CSI_IGNORE,
    TERM_STATE_OSC_STRING,
    TERM_STATE_DCS_STRING,
    TERM_STATE_SOS_STRING,
    TERM_STATE_STRING_IGNORE,
} term_parse_state_t;

typedef enum term_key {
    TERM_KEY_NONE = 0,
    TERM_KEY_UP,
    TERM_KEY_DOWN,
    TERM_KEY_LEFT,
    TERM_KEY_RIGHT,
    TERM_KEY_HOME,
    TERM_KEY_END,
    TERM_KEY_PAGE_UP,
    TERM_KEY_PAGE_DOWN,
    TERM_KEY_INSERT,
    TERM_KEY_DELETE,
    TERM_KEY_TAB,
    TERM_KEY_BACKTAB,
    TERM_KEY_ENTER,
    TERM_KEY_ESCAPE,
    TERM_KEY_BACKSPACE,
    TERM_KEY_F1,
    TERM_KEY_F2,
    TERM_KEY_F3,
    TERM_KEY_F4,
    TERM_KEY_F5,
    TERM_KEY_F6,
    TERM_KEY_F7,
    TERM_KEY_F8,
    TERM_KEY_F9,
    TERM_KEY_F10,
    TERM_KEY_F11,
    TERM_KEY_F12,
} term_key_t;

/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */

typedef struct term_selection {
    bool_t active;
    bool_t dragging;
    i32    anchor_x;
    i32    anchor_y;
    i32    cursor_x;
    i32    cursor_y;
    i32    scroll_offset;     /* scrollback lines included in the anchor */
} term_selection_t;

/* ------------------------------------------------------------------ */
/* Scrollback                                                          */
/* ------------------------------------------------------------------ */

typedef struct term_history {
    term_cell_t lines[TERM_SCROLLBACK][TERM_MAX_COLS];
    i32         offset;        /* lines scrolled back from the live screen */
    i32         head;          /* next slot to write */
    i32         count;         /* valid lines, 0..TERM_SCROLLBACK */
    i32         cols;          /* columns valid in each stored line */
} term_history_t;

/* ------------------------------------------------------------------ */
/* Terminal                                                            */
/* ------------------------------------------------------------------ */

typedef struct term {
    i32 cols;
    i32 rows;
    i32 saved_cols;
    i32 saved_rows;

    term_cell_t *cells;            /* points into main_grid or alt_grid */
    term_cell_t  main_grid[TERM_MAX_ROWS][TERM_MAX_COLS];
    term_cell_t  alt_grid[TERM_MAX_ROWS][TERM_MAX_COLS];

    term_cursor_t cursor;
    term_cursor_t saved_cursor_main;
    term_cursor_t saved_cursor_alt;

    u32  modes;
    i32  scroll_top;               /* inclusive, 0-based */
    i32  scroll_bottom;            /* inclusive, 0-based */

    i32  tab_stops[TERM_MAX_TABS];
    i32  tab_count;

    /* Parser */
    term_parse_state_t state;
    i32  csi_params[TERM_MAX_PARAMS];
    i32  csi_param_count;
    bool_t csi_private;            /* '?' prefix seen */
    u8   csi_intermediate;
    char osc_buffer[TERM_MAX_OSC];
    i32  osc_length;
    u32  utf8_code;                /* partial UTF-8 sequence in progress */
    u8   utf8_remaining;
    u32  utf8_min;
    u8   g0_charset;               /* 0 = ASCII, 1 = DEC special graphics */
    u8   g1_charset;
    u8   active_charset;           /* 0 = G0, 1 = G1 */

    /* Presentation */
    u32  default_fg;
    u32  default_bg;
    u32  cursor_colour;
    u32  palette[256];             /* OSC 4 overrides, 0 = use the default */
    u8   default_flags;
    i32  cell_w;
    i32  cell_h;
    u8   scale;
    i32  padding;

    /* Behaviour */
    bool_t shell_attached;         /* the built-in shell owns the prompt */
    bool_t local_echo;
    bool_t dirty;
    bool_t scrolled;               /* scrollback was used since last frame */
    bool_t blink_on;               /* cursor blink phase */
    bool_t title_changed;
    char   title[64];
    char   status[96];

    /* Counters for the status bar */
    u64  bytes_in;
    u64  lines_out;
    u64  escape_count;
    u64  bell_count;

    term_history_t   history;
    term_selection_t selection;

    /* The shell state, owned by shell.c; opaque to the other modules. */
    void *shell_state;

    /* The window this terminal paints into; owned by the app layer. */
    struct gui_window *window;
} term_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* One static terminal lives in the shell; init is idempotent and resets the
 * grid, modes and parser. */
term_t *axterm_instance(void);
void    axterm_init(term_t *term);
void    axterm_reset(term_t *term);
void    axterm_resize(term_t *term, i32 cols, i32 rows);
void    axterm_destroy(term_t *term);

/* ------------------------------------------------------------------ */
/* Output path                                                         */
/* ------------------------------------------------------------------ */

void axterm_feed(term_t *term, u8 byte);
void axterm_write(term_t *term, const char *text);
void axterm_write_line(term_t *term, const char *text);
void axterm_printf(term_t *term, const char *fmt, ...);
void axterm_bell(term_t *term);

/* ------------------------------------------------------------------ */
/* Grid operations (used by the parser and the shell)                   */
/* ------------------------------------------------------------------ */

void axterm_clear_all(term_t *term);
void axterm_clear_line(term_t *term, i32 mode);
void axterm_clear_screen(term_t *term, i32 mode);
void axterm_erase_chars(term_t *term, i32 count);
void axterm_insert_lines(term_t *term, i32 count);
void axterm_delete_lines(term_t *term, i32 count);
void axterm_insert_chars(term_t *term, i32 count);
void axterm_delete_chars(term_t *term, i32 count);
void axterm_scroll_up(term_t *term, i32 count);
void axterm_scroll_down(term_t *term, i32 count);
void axterm_scroll_region_up(term_t *term, i32 count, bool_t save_history);
void axterm_scroll_region_down(term_t *term, i32 count);
void axterm_newline(term_t *term);
void axterm_carriage_return(term_t *term);
void axterm_backspace(term_t *term);
void axterm_tab(term_t *term, i32 count);
void axterm_reverse_index(term_t *term);
void axterm_save_cursor(term_t *term);
void axterm_restore_cursor(term_t *term);
void axterm_set_cursor(term_t *term, i32 x, i32 y);
void axterm_move_relative(term_t *term, i32 dx, i32 dy);
void axterm_put_cell(term_t *term, u32 ch, u8 flags);
void axterm_set_scroll_region(term_t *term, i32 top, i32 bottom);
void axterm_reset_scroll_region(term_t *term);
void axterm_switch_screen(term_t *term, bool_t alternate);
void axterm_clear_tabs(term_t *term, bool_t all);
void axterm_set_tab_stop(term_t *term);
void axterm_set_title(term_t *term, const char *text);
void axterm_set_status(term_t *term, const char *text);
void       axterm_set_colour(term_t *term, i32 index, u32 colour);
u32        axterm_palette_lookup(const term_t *term, i32 index);
u32        axterm_named(term_t *term, const char *name);
const char *axterm_attribute_name(const term_t *term);
i32        axterm_nearest_palette(u32 colour, const u32 *palette, i32 count);

/* ------------------------------------------------------------------ */
/* Attributes                                                          */
/* ------------------------------------------------------------------ */

u32      axterm_default_palette(i32 index);
u32      axterm_bright_palette(i32 index);
u32      axterm_xterm256(i32 index);
void     axterm_reset_attributes(term_t *term);
term_cell_t axterm_make_cell(term_t *term, u32 ch, u8 extra_flags);
u32      axterm_effective_fg(const term_t *term, const term_cell_t *cell);
u32      axterm_effective_bg(const term_t *term, const term_cell_t *cell);

/* ------------------------------------------------------------------ */
/* Scrollback                                                          */
/* ------------------------------------------------------------------ */

void axterm_history_init(term_t *term);
void axterm_history_push(term_t *term);
void axterm_history_push_line(term_t *term, const term_cell_t *cells, i32 cols);
usize axterm_line_text(const term_t *term, i32 y, char *buffer, usize cap);
i32   axterm_view_lines(const term_t *term);
const term_cell_t *axterm_view_line(const term_t *term, i32 row);
i32   axterm_view_top(const term_t *term);
i32   axterm_view_row_at(const term_t *term, i32 visible_row);
usize axterm_view_line_text(const term_t *term, i32 row, char *buffer, usize cap);
i32   axterm_search(const term_t *term, const char *needle, i32 from_row);
void  axterm_view_center(term_t *term, i32 row);
i32   axterm_view_percent(const term_t *term);
void axterm_back_tab(term_t *term, i32 count);
void axterm_scroll_view(term_t *term, i32 lines);
i32  axterm_history_count(const term_t *term);
term_cell_t *axterm_history_line(const term_t *term, i32 index);
void axterm_history_clear(term_t *term);
i32  axterm_history_offset(const term_t *term);

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

void axterm_input_char(term_t *term, char ch);
void axterm_input_key(term_t *term, term_key_t key, bool_t shift, bool_t ctrl,
                      bool_t alt);
void axterm_input_bytes(term_t *term, const char *bytes, usize count);
bool_t axterm_input_control(term_t *term, char ch);
void axterm_input_wheel(term_t *term, i32 delta);
void axterm_input_click(term_t *term, i32 x, i32 y, u8 buttons, bool_t pressed, i32 visible_row);
usize axterm_input_copy(term_t *term, char *buffer, usize cap);
const char *axterm_key_sequence(const term_t *term, term_key_t key, bool_t shift,
                                bool_t ctrl, bool_t alt, usize *out_length);

/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */

void  axterm_selection_begin(term_t *term, i32 x, i32 y);
void  axterm_selection_extend(term_t *term, i32 x, i32 y);
void  axterm_selection_end(term_t *term);
void  axterm_selection_clear(term_t *term);
usize axterm_selection_copy(term_t *term, char *buffer, usize cap);
bool_t axterm_selection_contains(const term_t *term, i32 x, i32 y);
void axterm_selection_all(term_t *term);
i32 axterm_selection_cell_count(const term_t *term);
void  axterm_selection_normalise(const term_t *term, i32 *x0, i32 *y0, i32 *x1,
                                 i32 *y1);

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

void axterm_paint(term_t *term, gui_surface_t *surface, gui_rect_t area);
void axterm_paint_status(term_t *term, gui_surface_t *surface, gui_rect_t area);
void axterm_paint_scrollbar(term_t *term, gui_surface_t *surface, gui_rect_t area);
void axterm_paint_banner(term_t *term, gui_surface_t *surface, gui_rect_t area);
void axterm_paint_palette_strip(term_t *term, gui_surface_t *surface, gui_rect_t area);
void axterm_blink_update(term_t *term, u32 elapsed_ms);
void axterm_draw_cell(term_t *term, gui_surface_t *surface, term_cell_t *cell,
                      i32 px, i32 py, bool_t selected);
void axterm_set_metrics(term_t *term, u8 scale);
i32  axterm_pixel_width(const term_t *term);
i32  axterm_pixel_height(const term_t *term);

/* ------------------------------------------------------------------ */
/* Shell                                                               */
/* ------------------------------------------------------------------ */

void axterm_shell_init(term_t *term);
void axterm_shell_prompt(term_t *term);
void axterm_shell_submit(term_t *term, const char *line);
void axterm_shell_insert(term_t *term, char ch);
void axterm_shell_backspace(term_t *term);
void axterm_shell_history_step(term_t *term, i32 direction);
void axterm_shell_complete(term_t *term);
void axterm_shell_submit_current(term_t *term);
void axterm_shell_clear_input(term_t *term);
void axterm_shell_retheme(term_t *term);
const char *axterm_shell_prompt_text(const term_t *term);
i32  axterm_shell_input_length(const term_t *term);
bool_t axterm_shell_busy(const term_t *term);

/* ------------------------------------------------------------------ */
/* Application entry                                                   */
/* ------------------------------------------------------------------ */

void axterm_open(void);          /* creates the shell window */
void axterm_frame(void);         /* called once per compositor frame */
bool_t axterm_handle_key(char key, u8 scancode);
bool_t axterm_handle_mouse(i32 x, i32 y, u8 buttons, bool_t pressed);
void axterm_on_resize(term_t *term, i32 width, i32 height);
void axterm_feed_line(const char *text);

#endif /* AXIOM_TERM_H */
