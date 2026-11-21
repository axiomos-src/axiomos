/* AxiomOS terminal - window application.
 *
 * This is the glue between the shell's window system and the emulator core:
 * it creates the window, forwards paint, keyboard and mouse events, keeps the
 * cursor blinking and refits the grid when the user resizes.  All of the
 * interesting work lives in the other files of this directory; this one is
 * deliberately thin so the terminal can also be driven head-less from a
 * diagnostic hook.
 */

#include <axiom/lib.h>
#include "term.h"

/* One terminal instance for the shell.  A terminal per window would need a
 * window-private state pointer, which the current window manager supports;
 * the single instance keeps the static grid affordable. */
static term_t g_terminal;

/* Set by axterm_open, cleared by the window's close callback. */
static bool_t  g_open;
static u32     g_frame_counter;

term_t *axterm_instance(void)
{
    return &g_terminal;
}

/* ------------------------------------------------------------------ */
/* Scancode mapping                                                    */
/* ------------------------------------------------------------------ */

/* Set 1 scancodes, which is what the kernel's keyboard driver reports. */
static term_key_t translate_scancode(u8 scancode)
{
    switch (scancode) {
    case 0x01: return TERM_KEY_ESCAPE;
    case 0x0E: return TERM_KEY_BACKSPACE;
    case 0x0F: return TERM_KEY_TAB;
    case 0x1C: return TERM_KEY_ENTER;
    case 0x47: return TERM_KEY_HOME;
    case 0x48: return TERM_KEY_UP;
    case 0x49: return TERM_KEY_PAGE_UP;
    case 0x4B: return TERM_KEY_LEFT;
    case 0x4D: return TERM_KEY_RIGHT;
    case 0x4F: return TERM_KEY_END;
    case 0x50: return TERM_KEY_DOWN;
    case 0x51: return TERM_KEY_PAGE_DOWN;
    case 0x52: return TERM_KEY_INSERT;
    case 0x53: return TERM_KEY_DELETE;
    case 0x3B: return TERM_KEY_F1;
    case 0x3C: return TERM_KEY_F2;
    case 0x3D: return TERM_KEY_F3;
    case 0x3E: return TERM_KEY_F4;
    case 0x3F: return TERM_KEY_F5;
    case 0x40: return TERM_KEY_F6;
    case 0x41: return TERM_KEY_F7;
    case 0x42: return TERM_KEY_F8;
    case 0x43: return TERM_KEY_F9;
    case 0x44: return TERM_KEY_F10;
    case 0x57: return TERM_KEY_F11;
    case 0x58: return TERM_KEY_F12;
    default: return TERM_KEY_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

static void term_window_paint(gui_window_t *window)
{
    term_t *term = (term_t *)window->state;
    if (!term || !window->content) {
        return;
    }

    gui_surface_t *surface = window->content;
    const gui_theme_t *theme = axui_theme_current();

    /* Status bar at the bottom, scrollbar on the right. */
    i32 status_h = 18;
    i32 scroll_w = 10;
    gui_rect_t text_area = gui_rect(0, 0, surface->width - scroll_w,
                                    surface->height - status_h);
    gui_rect_t status_area = gui_rect(0, surface->height - status_h,
                                      surface->width, status_h);
    gui_rect_t scroll_area = gui_rect(surface->width - scroll_w, 0, scroll_w,
                                      surface->height - status_h);

    /* The grid must match the text area; a resize event normally did this
     * already, but the first paint after creation needs it. */
    i32 cols = (text_area.w - term->padding * 2) / term->cell_w;
    i32 rows = (text_area.h - term->padding * 2) / term->cell_h;
    if (cols != term->cols || rows != term->rows) {
        axterm_resize(term, cols, rows);
    }

    axterm_paint(term, surface, text_area);
    axterm_paint_banner(term, surface, text_area);
    axterm_paint_scrollbar(term, surface, scroll_area);
    axterm_paint_status(term, surface, status_area);

    /* A one pixel separator between the text and the status bar. */
    for (i32 x = 0; x < surface->width; x++) {
        u32 *p = &surface->pixels[(usize)(status_area.y - 1) *
                                  (usize)surface->stride + (usize)x];
        *p = theme->border;
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void term_window_key(gui_window_t *window, char key, u8 scancode)
{
    term_t *term = (term_t *)window->state;
    if (!term) {
        return;
    }

    term_key_t special = translate_scancode(scancode);
    if (special != TERM_KEY_NONE && key == 0) {
        /* Ctrl and Alt are not delivered separately by the current driver;
         * the modifiers that matter (Ctrl+C, Ctrl+L) arrive as control codes
         * through the character path below. */
        axterm_input_key(term, special, (bool_t)0, (bool_t)0, (bool_t)0);
        gui_window_invalidate(window);
        return;
    }

    if (key == 0) {
        return;
    }

    /* Control characters are delivered as the low ASCII values. */
    if ((u8)key < 0x20 && key != '\n' && key != '\t' && key != '\b') {
        axterm_input_control(term, key);
    } else if (key == '\n') {
        axterm_input_key(term, TERM_KEY_ENTER, (bool_t)0, (bool_t)0, (bool_t)0);
    } else if (key == '\b') {
        axterm_input_key(term, TERM_KEY_BACKSPACE, (bool_t)0, (bool_t)0, (bool_t)0);
    } else if (key == '\t') {
        axterm_input_key(term, TERM_KEY_TAB, (bool_t)0, (bool_t)0, (bool_t)0);
    } else {
        axterm_input_char(term, key);
    }

    gui_window_invalidate(window);
}

static void term_window_mouse(gui_window_t *window, i32 x, i32 y, u8 buttons,
                              bool_t pressed)
{
    term_t *term = (term_t *)window->state;
    if (!term) {
        return;
    }
    i32 status_h = 18;
    if (y >= window->content->height - status_h) {
        return;
    }
    i32 col = (x - term->padding) / term->cell_w;
    i32 row = (y - term->padding) / term->cell_h;
    axterm_input_click(term, col, row, buttons, pressed, row);
    gui_window_invalidate(window);
}

static void term_window_resize(gui_window_t *window)
{
    term_t *term = (term_t *)window->state;
    if (!term || !window->content) {
        return;
    }
    axterm_on_resize(term, window->content->width - 10,
                     window->content->height - 18);
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

void axterm_frame(void)
{
    if (!g_open) {
        return;
    }
    g_frame_counter++;
    /* 16 ms per frame is the compositor's nominal rate. */
    axterm_blink_update(&g_terminal, g_frame_counter * 16);

    if (g_terminal.dirty && g_terminal.window) {
        gui_window_invalidate(g_terminal.window);
    }
}

/* ------------------------------------------------------------------ */
/* Open                                                                */
/* ------------------------------------------------------------------ */

void axterm_open(void)
{
    if (g_open && g_terminal.window) {
        gui_window_focus(g_terminal.window);
        return;
    }

    ax_memset(&g_terminal, 0, sizeof(g_terminal));
    axterm_init(&g_terminal);
    axterm_set_metrics(&g_terminal, 1);
    g_terminal.cols = 100;
    g_terminal.rows = 30;

    gui_window_t *window = gui_window_create("Terminal", 110, 80, 760, 520,
                                             term_window_paint);
    if (!window) {
        return;
    }

    g_terminal.window = window;
    window->state = &g_terminal;
    window->on_key = term_window_key;
    window->on_mouse = term_window_mouse;
    window->on_resize = term_window_resize;

    axterm_shell_init(&g_terminal);
    g_open = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Direct entry points (used by the diagnostic hook in the kernel)      */
/* ------------------------------------------------------------------ */

bool_t axterm_handle_key(char key, u8 scancode)
{
    if (!g_open) {
        return (bool_t)0;
    }
    term_window_key(g_terminal.window, key, scancode);
    return (bool_t)1;
}

bool_t axterm_handle_mouse(i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!g_open || !g_terminal.window) {
        return (bool_t)0;
    }
    term_window_mouse(g_terminal.window, x, y, buttons, pressed);
    return (bool_t)1;
}

/* Feed a line into the terminal as if it came from a program, which the
 * boot log uses to seed the screen with the kernel's own messages. */
void axterm_feed_line(const char *text)
{
    if (!text) {
        return;
    }
    axterm_write_line(&g_terminal, text);
    if (g_terminal.window) {
        gui_window_invalidate(g_terminal.window);
    }
}
