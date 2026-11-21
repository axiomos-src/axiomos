/*
 * AxiomOS - window manager, theme and compositor.
 *
 * Windows are stacked in a simple z-order array; the last entry is the topmost
 * window.  Dragging is handled here as well, because it is a property of the
 * window frame rather than of any particular application.
 */

#include <gui.h>

/* ------------------------------------------------------------------ theme */

static const gui_theme_t g_theme = {
    .desktop_top      = GUI_RGB(0x0B, 0x12, 0x2A),
    .desktop_bottom   = GUI_RGB(0x1B, 0x10, 0x38),
    .window_bg        = GUI_RGB(0x14, 0x1A, 0x2E),
    .window_bg_alt    = GUI_RGB(0x1B, 0x23, 0x3B),
    .title_bg         = GUI_RGB(0x16, 0x1D, 0x33),
    .title_bg_active  = GUI_RGB(0x23, 0x2F, 0x52),
    .title_text       = GUI_RGB(0xF2, 0xF5, 0xFF),
    .title_text_inactive = GUI_RGB(0x8A, 0x94, 0xB0),
    .border           = GUI_RGB(0x25, 0x2E, 0x48),
    .border_active    = GUI_RGB(0x57, 0xC8, 0xFF),
    .accent           = GUI_RGB(0x4C, 0xC2, 0xFF),
    .accent_dim       = GUI_RGB(0x2A, 0x6E, 0x9E),
    .text             = GUI_RGB(0xE6, 0xEC, 0xFF),
    .text_dim         = GUI_RGB(0x93, 0x9E, 0xBC),
    .text_inverse     = GUI_RGB(0x08, 0x0C, 0x18),
    .taskbar          = GUI_RGB(0x10, 0x15, 0x26),
    .taskbar_hover    = GUI_RGB(0x22, 0x2B, 0x45),
    .success          = GUI_RGB(0x5A, 0xD6, 0x9A),
    .warning          = GUI_RGB(0xF2, 0xC1, 0x4E),
    .error            = GUI_RGB(0xFF, 0x6B, 0x6B),
    .selection        = GUI_RGB(0x2B, 0x5C, 0x8A),
};

const gui_theme_t *gui_theme(void)
{
    return &g_theme;
}

/* -------------------------------------------------------------- wallpaper */

void gui_wallpaper_paint(gui_surface_t *surface)
{
    const gui_theme_t *theme = gui_theme();
    gui_vgradient(surface, gui_rect(0, 0, surface->width, surface->height),
                  theme->desktop_top, theme->desktop_bottom);

    /* A soft radial glow behind the logo, approximated with rectangles. */
    i32 cx = surface->width / 4;
    i32 cy = surface->height / 3;
    for (i32 radius = 220; radius > 0; radius -= 4) {
        u32 alpha = (u32)(12 - radius / 24);
        if (alpha == 0)
            continue;
        gui_blend_rect(surface, gui_rect(cx - radius, cy - radius / 2,
                                         radius * 2, radius),
                       GUI_ARGB((u8)alpha, 0x3A, 0x7A, 0xC8));
    }

    /* Fine grid, one line every 32 pixels, very low contrast. */
    for (i32 x = 0; x < surface->width; x += 32)
        gui_vline(surface, x, 0, surface->height, GUI_ARGB(0x0E, 0x8F, 0xB6, 0xFF));
    for (i32 y = 0; y < surface->height; y += 32)
        gui_hline(surface, 0, surface->width, y, GUI_ARGB(0x0E, 0x8F, 0xB6, 0xFF));

    /* Watermark: the product name, low contrast, bottom right. */
    i32 width = gui_text_width("AXIOM OS", 4);
    i32 x = surface->width - width - 48;
    i32 y = surface->height - 190;
    gui_text_shadow(surface, x, y, "AXIOM OS", GUI_ARGB(0x22, 0xB6, 0xE6, 0xFF),
                    GUI_ARGB(0x10, 0x00, 0x00, 0x00), 4);

    /* Horizontal accent line under the watermark. */
    gui_hline(surface, x, x + width, y + 74, GUI_ARGB(0x33, 0x57, 0xC8, 0xFF));
}

/* ---------------------------------------------------------------- windows */

static gui_window_t *g_windows[GUI_WINDOW_MAX];
static i32 g_window_count = 0;
static u32 g_next_id = 1;
static gui_window_t *g_dragging = 0;
static i32 g_drag_offset_x = 0;
static i32 g_drag_offset_y = 0;

void gui_wm_init(void)
{
    g_window_count = 0;
    g_next_id = 1;
    for (i32 i = 0; i < GUI_WINDOW_MAX; i++)
        g_windows[i] = 0;
}

i32 gui_window_count(void)
{
    return g_window_count;
}

gui_window_t **gui_window_list(i32 *count)
{
    if (count)
        *count = g_window_count;
    return g_windows;
}

gui_window_t *gui_window_by_id(u32 id)
{
    for (i32 i = 0; i < g_window_count; i++)
        if (g_windows[i]->id == id)
            return g_windows[i];
    return 0;
}

gui_window_t *gui_window_create(const char *title, i32 x, i32 y, i32 w, i32 h,
                                gui_paint_fn paint)
{
    if (g_window_count >= GUI_WINDOW_MAX)
        return 0;

    gui_window_t *window = (gui_window_t *)kzalloc(sizeof(gui_window_t));
    if (!window)
        return 0;

    window->id = g_next_id++;
    window->frame = gui_rect(x, y, w, h);
    window->visible = true;
    window->resizable = true;
    window->dirty = true;
    window->on_paint = paint;

    i32 client_w = w - 2;
    i32 client_h = h - GUI_TITLE_HEIGHT - 1;
    if (client_w < 8)
        client_w = 8;
    if (client_h < 8)
        client_h = 8;
    window->content = gui_surface_new(client_w, client_h);
    if (!window->content) {
        kfree(window);
        return 0;
    }
    gui_fill(window->content, gui_theme()->window_bg);

    for (i32 i = 0; i < (i32)(sizeof(window->title) - 1) && title[i]; i++)
        window->title[i] = title[i];

    g_windows[g_window_count++] = window;
    gui_window_focus(window);
    return window;
}

void gui_window_close(gui_window_t *window)
{
    if (!window)
        return;
    for (i32 i = 0; i < g_window_count; i++) {
        if (g_windows[i] != window)
            continue;
        for (i32 j = i; j < g_window_count - 1; j++)
            g_windows[j] = g_windows[j + 1];
        g_window_count--;
        break;
    }
    if (g_dragging == window)
        g_dragging = 0;
    if (window->content)
        gui_surface_free(window->content);
    kfree(window);
    if (g_window_count > 0)
        gui_window_focus(g_windows[g_window_count - 1]);
}

void gui_window_focus(gui_window_t *window)
{
    if (!window)
        return;
    for (i32 i = 0; i < g_window_count; i++) {
        g_windows[i]->focused = false;
        if (g_windows[i] == window) {
            for (i32 j = i; j < g_window_count - 1; j++)
                g_windows[j] = g_windows[j + 1];
            g_windows[g_window_count - 1] = window;
            break;
        }
    }
    window->focused = true;
    window->minimised = false;
}

gui_window_t *gui_window_focused(void)
{
    if (g_window_count == 0)
        return 0;
    return g_windows[g_window_count - 1];
}

gui_window_t *gui_window_at(i32 x, i32 y)
{
    for (i32 i = g_window_count - 1; i >= 0; i--) {
        gui_window_t *window = g_windows[i];
        if (!window->visible || window->minimised)
            continue;
        if (gui_rect_contains(window->frame, x, y))
            return window;
    }
    return 0;
}

void gui_window_move(gui_window_t *window, i32 x, i32 y)
{
    if (!window)
        return;
    window->frame.x = x;
    window->frame.y = y;
    window->dirty = true;
}

void gui_window_resize(gui_window_t *window, i32 w, i32 h)
{
    if (!window || w < 200 || h < 120)
        return;
    window->frame.w = w;
    window->frame.h = h;
    gui_surface_free(window->content);
    window->content = gui_surface_new(w - 2, h - GUI_TITLE_HEIGHT - 1);
    if (window->content)
        gui_fill(window->content, gui_theme()->window_bg);
    if (window->on_resize)
        window->on_resize(window);
    window->dirty = true;
}

void gui_window_invalidate(gui_window_t *window)
{
    if (window)
        window->dirty = true;
}

/* ------------------------------------------------------------- compositor */

static u32   *g_framebuffer = 0;
static i32    g_screen_w = 0;
static i32    g_screen_h = 0;
static u32    g_fb_stride = 0;
static u32    g_fb_rmask = 0;
static u32    g_fb_gmask = 0;
static u32    g_fb_bmask = 0;
static gui_surface_t g_back;
static gui_surface_t g_wallpaper;
static bool_t g_have_wallpaper = false;
static bool_t g_present_full = true;

static i32    g_cursor_x = 512;
static i32    g_cursor_y = 384;
static u8     g_mouse_buttons = 0;
static u8     g_prev_buttons = 0;

void gui_desktop_size(i32 *width, i32 *height)
{
    if (width)
        *width = g_screen_w;
    if (height)
        *height = g_screen_h;
}

i32 gui_taskbar_height(void)
{
    return 40;
}

void gui_compositor_mark_full(void)
{
    g_present_full = true;
}

static u32 pack_colour(u32 argb)
{
    u32 r = GUI_R(argb), g = GUI_G(argb), b = GUI_B(argb);
    u32 value = 0;
    if (g_fb_rmask) {
        u32 shift = 0;
        u32 mask = g_fb_rmask;
        while (!(mask & 1)) {
            mask >>= 1;
            shift++;
        }
        u32 bits = 0;
        while (mask & 1) {
            bits++;
            mask >>= 1;
        }
        u32 max = (1u << bits) - 1u;
        u32 scaled = bits >= 8 ? (r >> (bits - 8)) : ((r * max) / 255);
        value |= scaled << shift;
    }
    if (g_fb_gmask) {
        u32 shift = 0;
        u32 mask = g_fb_gmask;
        while (!(mask & 1)) {
            mask >>= 1;
            shift++;
        }
        u32 bits = 0;
        while (mask & 1) {
            bits++;
            mask >>= 1;
        }
        u32 max = (1u << bits) - 1u;
        u32 scaled = bits >= 8 ? (g >> (bits - 8)) : ((g * max) / 255);
        value |= scaled << shift;
    }
    if (g_fb_bmask) {
        u32 shift = 0;
        u32 mask = g_fb_bmask;
        while (!(mask & 1)) {
            mask >>= 1;
            shift++;
        }
        u32 bits = 0;
        while (mask & 1) {
            bits++;
            mask >>= 1;
        }
        u32 max = (1u << bits) - 1u;
        u32 scaled = bits >= 8 ? (b >> (bits - 8)) : ((b * max) / 255);
        value |= scaled << shift;
    }
    return value;
}

/* Detect the common layout to avoid a per pixel shift/mask walk. */
static bool_t g_fast_rgbx = false;
static bool_t g_fast_bgrx = false;

static void present_surface(void)
{
    u64 row_bytes = (u64)g_screen_w * 4u;
    for (i32 y = 0; y < g_screen_h; y++) {
        u32 *dst = (u32 *)(g_framebuffer + (u64)y * g_fb_stride);
        const u32 *src = g_back.pixels + (u64)y * g_back.stride;
        if (g_fast_rgbx) {
            for (i32 x = 0; x < g_screen_w; x++)
                dst[x] = src[x] & 0x00FFFFFFu;
        } else if (g_fast_bgrx) {
            for (i32 x = 0; x < g_screen_w; x++) {
                u32 pixel = src[x];
                dst[x] = (u32)((pixel & 0x00FF0000u) >> 16)
                       | (pixel & 0x0000FF00u)
                       | (u32)((pixel & 0x000000FFu) << 16);
            }
        } else {
            for (i32 x = 0; x < g_screen_w; x++)
                dst[x] = pack_colour(src[x]);
        }
    }
    (void)row_bytes;
}

bool_t gui_compositor_init(u32 framebuffer, u32 width, u32 height, u32 stride,
                           u32 bpp, u32 red_mask, u32 green_mask, u32 blue_mask)
{
    if (!framebuffer || width < 320 || height < 200 || bpp != 32)
        return false;

    g_framebuffer = (u32 *)(u64)framebuffer;
    g_screen_w = (i32)width;
    g_screen_h = (i32)height;
    g_fb_stride = stride ? stride : width;
    g_fb_rmask = red_mask;
    g_fb_gmask = green_mask;
    g_fb_bmask = blue_mask;

    g_fast_rgbx = (red_mask == 0x00FF0000u && green_mask == 0x0000FF00u &&
                   blue_mask == 0x000000FFu);
    g_fast_bgrx = (blue_mask == 0x00FF0000u && green_mask == 0x0000FF00u &&
                   red_mask == 0x000000FFu);

    gui_surface_wrap(&g_back, (u32 *)kmalloc((u64)width * height * 4u),
                     (i32)width, (i32)height, (i32)width);
    gui_surface_wrap(&g_wallpaper, (u32 *)kmalloc((u64)width * height * 4u),
                     (i32)width, (i32)height, (i32)width);
    if (!g_back.pixels || !g_wallpaper.pixels)
        return false;

    gui_fill(&g_back, GUI_RGB(0, 0, 0));
    g_have_wallpaper = false;

    g_cursor_x = g_screen_w / 2;
    g_cursor_y = g_screen_h / 2;
    printk("gui: compositor %ux%u, masks r=%x g=%x b=%x\n", width, height,
           red_mask, green_mask, blue_mask);
    return true;
}

/* Cursor: a classic arrow, drawn as a small alpha mask. */
static const u8 g_cursor_mask[16][8] = {
    { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xFF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xFF, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xF8, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xE0, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

void gui_cursor_paint(gui_surface_t *surface, i32 x, i32 y)
{
    for (i32 row = 0; row < 16; row++) {
        for (i32 column = 0; column < 8; column++) {
            if (!(g_cursor_mask[row][column / 8] & (0x80 >> (column % 8)))
                && column < 8)
                continue;
            i32 px = x + column;
            i32 py = y + row;
            if (px < 0 || py < 0 || px >= surface->width || py >= surface->height)
                continue;
            surface->pixels[(u64)py * surface->stride + px] =
                (column == 0) ? GUI_RGB(0xFF, 0xFF, 0xFF) : GUI_RGB(0x00, 0x00, 0x00);
        }
    }
}

/* Paint the frame (title bar, border, shadow) of one window. */
static void paint_window_frame(gui_surface_t *target, gui_window_t *window)
{
    const gui_theme_t *theme = gui_theme();
    gui_rect_t frame = window->frame;

    gui_shadow(target, frame, 10, 120);

    u32 border = window->focused ? theme->border_active : theme->border;
    gui_fill_rect(target, gui_rect(frame.x + 1, frame.y + 1, frame.w - 2,
                                   frame.h - 2), border);

    u32 title_bg = window->focused ? theme->title_bg_active : theme->title_bg;
    gui_fill_rect(target, gui_rect(frame.x + 1, frame.y + 1, frame.w - 2,
                                   GUI_TITLE_HEIGHT - 1), title_bg);

    /* Accent stripe on the left of the title bar. */
    gui_fill_rect(target, gui_rect(frame.x + 1, frame.y + 1, 4,
                                   GUI_TITLE_HEIGHT - 1),
                  window->focused ? theme->accent : theme->accent_dim);

    gui_text(target, frame.x + 12, frame.y + 6, window->title,
             window->focused ? theme->title_text : theme->title_text_inactive, 1);

    /* Window controls: close on the right, minimise next to it. */
    i32 button_size = 14;
    i32 close_x = frame.x + frame.w - button_size - 6;
    i32 y = frame.y + 6;
    gui_fill_round_rect(target, gui_rect(close_x, y, button_size, button_size),
                        3, GUI_RGB(0xE8, 0x55, 0x55));
    gui_text(target, close_x + 3, y + 1, "x", GUI_RGB(0xFF, 0xFF, 0xFF), 1);
    i32 min_x = close_x - button_size - 4;
    gui_fill_round_rect(target, gui_rect(min_x, y, button_size, button_size),
                        3, GUI_RGB(0x3A, 0x46, 0x66));
    gui_text(target, min_x + 3, y + 1, "-", GUI_RGB(0xE6, 0xEC, 0xFF), 1);

    /* Client area. */
    gui_blit_surface(target, frame.x + 1, frame.y + GUI_TITLE_HEIGHT,
                     window->content);
    if (window->focused)
        gui_hline(target, frame.x + 1, frame.x + frame.w - 2,
                  frame.y + GUI_TITLE_HEIGHT - 1, theme->accent);
}

bool_t gui_active(void);

static void dispatch_mouse(gui_surface_t *target)
{
    i32 mx = g_cursor_x;
    i32 my = g_cursor_y;
    u8 buttons = g_mouse_buttons;
    bool_t pressed = (bool_t)((buttons & 1) && !(g_prev_buttons & 1));
    bool_t released = (bool_t)((!(buttons & 1)) && (g_prev_buttons & 1));

    if (pressed) {
        gui_window_t *window = gui_window_at(mx, my);
        if (window) {
            gui_window_focus(window);
            i32 local_y = my - window->frame.y;
            if (local_y < GUI_TITLE_HEIGHT) {
                /* Title bar: close button, minimise button or drag. */
                i32 close_x = window->frame.x + window->frame.w - 20;
                i32 min_x = close_x - 18;
                if (mx >= close_x && mx < close_x + 14) {
                    gui_window_close(window);
                    return;
                }
                if (mx >= min_x && mx < min_x + 14) {
                    window->minimised = true;
                    gui_window_focus(g_windows[0]);
                    return;
                }
                g_dragging = window;
                g_drag_offset_x = mx - window->frame.x;
                g_drag_offset_y = my - window->frame.y;
            } else if (window->on_mouse) {
                window->on_mouse(window, mx - window->frame.x - 1,
                                 local_y - GUI_TITLE_HEIGHT, buttons, true);
            }
        } else if (!gui_taskbar_click(mx, my, g_screen_h)) {
            gui_app_launcher_open();
        }
    }

    if (g_dragging && (buttons & 1)) {
        i32 nx = mx - g_drag_offset_x;
        i32 ny = my - g_drag_offset_y;
        if (ny < 0)
            ny = 0;
        if (ny > g_screen_h - 60)
            ny = g_screen_h - 60;
        gui_window_move(g_dragging, nx, ny);
        g_present_full = true;
    }
    if (released) {
        g_dragging = 0;
        gui_window_t *window = gui_window_at(mx, my);
        if (window && window->on_mouse && my - window->frame.y >= GUI_TITLE_HEIGHT)
            window->on_mouse(window, mx - window->frame.x - 1,
                             my - window->frame.y - GUI_TITLE_HEIGHT, buttons, false);
    }

    if (buttons & 1)
        g_present_full = true;
    (void)target;
    g_prev_buttons = buttons;
}

static void dispatch_keys(void)
{
    u8 scancode;
    bool_t pressed;
    gui_window_t *focused = gui_window_focused();
    u8 modifiers = ps2_modifiers();
    bool_t shift = (bool_t)(modifiers & 1);
    bool_t caps = (bool_t)((modifiers & 4) != 0);

    while (ps2_get_key(&scancode, &pressed)) {
        if (!pressed)
            continue;
        char key = keyboard_translate(scancode, shift, caps);
        if (!key)
            continue;
        if (focused && focused->on_key)
            focused->on_key(focused, key, scancode);
        g_present_full = true;
    }
}

void gui_compositor_frame(void)
{
    if (!g_back.pixels)
        return;

    if (!g_have_wallpaper) {
        gui_wallpaper_paint(&g_wallpaper);
        g_have_wallpaper = true;
    }
    gui_blit_surface(&g_back, 0, 0, &g_wallpaper);

    for (i32 i = 0; i < gui_window_count(); i++) {
        gui_window_t *window = g_windows[i];
        if (!window->visible || window->minimised)
            continue;
        if (window->on_paint)
            window->on_paint(window);
        paint_window_frame(&g_back, window);
        window->dirty = false;
    }

    gui_taskbar_paint(&g_back);
    gui_cursor_paint(&g_back, g_cursor_x, g_cursor_y);
    present_surface();
    g_present_full = false;
}

/* Mouse position bookkeeping is driven by the PS/2 driver. */
void gui_compositor_mouse_update(void)
{
    i32 nx = mouse_position_x();
    i32 ny = mouse_position_y();
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if (nx >= g_screen_w)
        nx = g_screen_w - 1;
    if (ny >= g_screen_h)
        ny = g_screen_h - 1;
    if (nx != g_cursor_x || ny != g_cursor_y) {
        g_cursor_x = nx;
        g_cursor_y = ny;
        g_present_full = true;
    }
    g_mouse_buttons = mouse_buttons();
    (void)dispatch_mouse;
}

i32 gui_cursor_position_x(void)
{
    return g_cursor_x;
}

i32 gui_cursor_position_y(void)
{
    return g_cursor_y;
}

void gui_input_poll(void)
{
    dispatch_keys();
    gui_compositor_mouse_update();
    dispatch_mouse(0);
}
