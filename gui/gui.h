/*
 * AxiomOS GUI - public interface.
 *
 * The window system is a software compositor.  Every window owns an ARGB
 * surface; the compositor blends them, in z-order, into a back buffer which is
 * then converted to the firmware's framebuffer pixel format once per frame.
 *
 * Coordinate system: 32 bit ARGB colours (0xAARRGGBB), origin top left.
 */
#ifndef AXIOM_GUI_H
#define AXIOM_GUI_H

#include <axiom/types.h>
#include <axiom/kernel.h>

/* ---------------------------------------------------------------- colours */

#define GUI_ARGB(a, r, g, b) \
    (((u32)(a) << 24) | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
#define GUI_RGB(r, g, b) GUI_ARGB(0xFF, (r), (g), (b))
#define GUI_ALPHA(c)     (((c) >> 24) & 0xFF)
#define GUI_R(c)         (((c) >> 16) & 0xFF)
#define GUI_G(c)         (((c) >> 8) & 0xFF)
#define GUI_B(c)         ((c) & 0xFF)

/* ------------------------------------------------------------------ rects */

typedef struct gui_rect {
    i32 x, y;
    i32 w, h;
} gui_rect_t;

static inline gui_rect_t gui_rect(i32 x, i32 y, i32 w, i32 h)
{
    gui_rect_t rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    return rect;
}

static inline bool_t gui_rect_contains(gui_rect_t r, i32 x, i32 y)
{
    return (bool_t)(x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h);
}

static inline gui_rect_t gui_rect_intersect(gui_rect_t a, gui_rect_t b)
{
    i32 x0 = a.x > b.x ? a.x : b.x;
    i32 y0 = a.y > b.y ? a.y : b.y;
    i32 x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    i32 y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return gui_rect(0, 0, 0, 0);
    return gui_rect(x0, y0, x1 - x0, y1 - y0);
}

/* --------------------------------------------------------------- surfaces */

typedef struct gui_surface {
    u32   *pixels;
    i32    width;
    i32    height;
    i32    stride;        /* in pixels */
    bool_t owned;        /* pixels were allocated by the surface layer */
} gui_surface_t;

gui_surface_t *gui_surface_new(i32 width, i32 height);
void           gui_surface_free(gui_surface_t *surface);
void           gui_surface_wrap(gui_surface_t *surface, u32 *pixels,
                                i32 width, i32 height, i32 stride);
void           gui_fill(gui_surface_t *surface, u32 colour);
void           gui_fill_rect(gui_surface_t *surface, gui_rect_t rect, u32 colour);
void           gui_blend_rect(gui_surface_t *surface, gui_rect_t rect, u32 colour);
void           gui_fill_round_rect(gui_surface_t *surface, gui_rect_t rect,
                                   i32 radius, u32 colour);
void           gui_vgradient(gui_surface_t *surface, gui_rect_t rect,
                             u32 top, u32 bottom);
void           gui_hline(gui_surface_t *surface, i32 x0, i32 x1, i32 y, u32 colour);
void           gui_vline(gui_surface_t *surface, i32 x, i32 y0, i32 y1, u32 colour);
void           gui_line(gui_surface_t *surface, i32 x0, i32 y0, i32 x1, i32 y1,
                        u32 colour);
void           gui_rect_outline(gui_surface_t *surface, gui_rect_t rect,
                                u32 colour);
void           gui_blit(gui_surface_t *dst, i32 dx, i32 dy,
                        const gui_surface_t *src, gui_rect_t src_rect);
void           gui_blit_alpha(gui_surface_t *dst, i32 dx, i32 dy,
                              const gui_surface_t *src, u8 alpha);
void           gui_blit_surface(gui_surface_t *dst, i32 dx, i32 dy,
                                const gui_surface_t *src);
void           gui_shadow(gui_surface_t *dst, gui_rect_t rect, i32 spread,
                          u8 strength);

/* ------------------------------------------------------------------- fonts */

extern const u8 gui_font8x16[95][16];
extern const u8 gui_font_width;
extern const u8 gui_font_height;
extern const u8 gui_font_first;
extern const u8 gui_font_last;

i32  gui_text_width(const char *text, u8 scale);
void gui_text(gui_surface_t *surface, i32 x, i32 y, const char *text,
              u32 colour, u8 scale);
void gui_text_shadow(gui_surface_t *surface, i32 x, i32 y, const char *text,
                     u32 colour, u32 shadow, u8 scale);
void gui_text_clipped(gui_surface_t *surface, gui_rect_t clip, i32 x, i32 y,
                      const char *text, u32 colour, u8 scale);

/* ------------------------------------------------------------------ theme */

typedef struct gui_theme {
    u32 desktop_top;
    u32 desktop_bottom;
    u32 window_bg;
    u32 window_bg_alt;
    u32 title_bg;
    u32 title_bg_active;
    u32 title_text;
    u32 title_text_inactive;
    u32 border;
    u32 border_active;
    u32 accent;
    u32 accent_dim;
    u32 text;
    u32 text_dim;
    u32 text_inverse;
    u32 taskbar;
    u32 taskbar_hover;
    u32 success;
    u32 warning;
    u32 error;
    u32 selection;
} gui_theme_t;

const gui_theme_t *gui_theme(void);

/* ---------------------------------------------------------------- windows */

typedef struct gui_window gui_window_t;

typedef void (*gui_paint_fn)(gui_window_t *window);
typedef void (*gui_key_fn)(gui_window_t *window, char key, u8 scancode);
typedef void (*gui_mouse_fn)(gui_window_t *window, i32 x, i32 y, u8 buttons,
                             bool_t pressed);
typedef void (*gui_resize_fn)(gui_window_t *window);

struct gui_window {
    u32             id;
    char            title[48];
    gui_rect_t      frame;        /* outer rectangle in desktop coordinates */
    bool_t          visible;
    bool_t          focused;
    bool_t          minimised;
    bool_t          resizable;
    bool_t          dirty;
    gui_surface_t  *content;      /* client area surface */
    void           *state;        /* application private data */
    gui_paint_fn    on_paint;
    gui_key_fn      on_key;
    gui_mouse_fn    on_mouse;
    gui_resize_fn   on_resize;
    void           *user;
};

#define GUI_WINDOW_MAX 10
#define GUI_TITLE_HEIGHT 26
#define GUI_BORDER 1

void           gui_wm_init(void);
gui_window_t  *gui_window_create(const char *title, i32 x, i32 y, i32 w, i32 h,
                                 gui_paint_fn paint);
void           gui_window_close(gui_window_t *window);
void           gui_window_focus(gui_window_t *window);
void           gui_window_move(gui_window_t *window, i32 x, i32 y);
void           gui_window_resize(gui_window_t *window, i32 w, i32 h);
void           gui_window_invalidate(gui_window_t *window);
gui_window_t  *gui_window_focused(void);
gui_window_t  *gui_window_at(i32 x, i32 y);
gui_window_t  *gui_window_by_id(u32 id);
gui_window_t  **gui_window_list(i32 *count);
i32            gui_window_count(void);

/* ------------------------------------------------------------- compositor */

bool_t gui_compositor_init(u32 framebuffer, u32 width, u32 height, u32 stride,
                           u32 bpp, u32 red_mask, u32 green_mask,
                           u32 blue_mask);
void   gui_compositor_frame(void);
void   gui_compositor_mark_full(void);
void   gui_input_poll(void);
i32    gui_cursor_position_x(void);
i32    gui_cursor_position_y(void);
bool_t hvci_active(void);
void   gui_desktop_size(i32 *width, i32 *height);
i32    gui_taskbar_height(void);

/* --------------------------------------------------------------- widgets */

typedef struct gui_button {
    gui_rect_t rect;
    const char *label;
    bool_t     hover;
    bool_t     pressed;
    bool_t     primary;
} gui_button_t;

void gui_widget_button(gui_surface_t *surface, gui_theme_t *theme,
                       gui_button_t *button);
bool_t gui_widget_button_event(gui_button_t *button, i32 x, i32 y,
                               u8 buttons, bool_t pressed);
void gui_widget_progress(gui_surface_t *surface, gui_rect_t rect, i32 percent,
                         u32 fill, u32 track);
void gui_widget_checkbox(gui_surface_t *surface, i32 x, i32 y, bool_t checked,
                         const char *label, u32 text_colour, u32 accent);
void gui_widget_slider(gui_surface_t *surface, gui_rect_t rect, i32 value,
                       i32 maximum, u32 accent, u32 track);
void gui_widget_scrollbar(gui_surface_t *surface, gui_rect_t rect, i32 offset,
                          i32 total, i32 visible, u32 track, u32 thumb);
void gui_widget_panel(gui_surface_t *surface, gui_rect_t rect, u32 fill,
                      u32 border, i32 radius);
void gui_widget_badge(gui_surface_t *surface, i32 x, i32 y, const char *text,
                      u32 fill, u32 text_colour);

/* -------------------------------------------------------------- wallpaper */

void gui_wallpaper_paint(gui_surface_t *surface);

/* ------------------------------------------------- applications (windows) */

void gui_app_launcher_open(void);     /* start menu / app grid */
void gui_app_terminal_open(void);
void gui_app_files_open(void);
void gui_app_settings_open(void);
void gui_app_about_open(void);
void gui_app_calculator_open(void);
void gui_app_editor_open(void);
void gui_app_taskmanager_open(void);
void gui_app_hvci_open(void);

void gui_apps_init(void);
void gui_taskbar_paint(gui_surface_t *surface);
bool_t gui_taskbar_click(i32 x, i32 y, i32 screen_height);
void gui_taskbar_hover(i32 x, i32 y);

/* --------------------------------------------------------- boot / lifecycle */

void gui_boot_splash(const char *stage, i32 percent);
void gui_cursor_paint(gui_surface_t *surface, i32 x, i32 y);

#endif /* AXIOM_GUI_H */
