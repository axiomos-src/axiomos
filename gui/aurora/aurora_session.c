/* AxiomOS - Aurora session controller.
 *
 * The session owns the top level compositing pass for the desktop: it holds the
 * back buffer, the damage list, the pointer state, the boot stage machine and
 * the overlay arming flags.  Applications draw into the session surface; the
 * session decides what to present and in which order.
 *
 * This module is intentionally free of application specific types so that the
 * kernel can bring the session up before any app module is linked in.
 */

#include "gui.h"

#define AXSE_MAX_LAYERS     16
#define AXSE_MAX_DAMAGE     32
#define AXSE_STAGE_COUNT    6

enum axse_stage
{
    AXSE_STAGE_FIRMWARE = 0,
    AXSE_STAGE_KERNEL,
    AXSE_STAGE_SERVICES,
    AXSE_STAGE_SESSION,
    AXSE_STAGE_DESKTOP,
    AXSE_STAGE_READY
};

enum axse_layer_kind
{
    AXSE_LAYER_WALLPAPER = 0,
    AXSE_LAYER_WINDOW,
    AXSE_LAYER_OVERLAY,
    AXSE_LAYER_CURSOR
};

typedef struct axse_layer
{
    gui_rect_t bounds;
    u8         kind;
    u8         opacity;
    u8         z;
    bool_t     visible;
    bool_t     interactive;
    char       name[24];
} axse_layer_t;

typedef struct axse_damage
{
    gui_rect_t rect;
    u8         layer;
    u32        stamp;
} axse_damage_t;

typedef struct axse_session
{
    gui_surface_t  back_buffer;
    gui_surface_t *screen;
    axse_layer_t   layers[AXSE_MAX_LAYERS];
    u32            layer_count;
    axse_damage_t  damage[AXSE_MAX_DAMAGE];
    u32            damage_count;
    u32            damage_stamp;
    bool_t         full_redraw;
    u8             stage;
    u32            stage_ticks;
    u32            frames;
    u32            presented;
    i32            pointer_x;
    i32            pointer_y;
    bool_t         pointer_down;
    u8             focus_layer;
    bool_t         locked;
    bool_t         task_view;
    bool_t         launcher;
    bool_t         searching;
    char           status[64];
    u32            uptime_ticks;
} axse_session_t;

static void  axse_zero(void *ptr, u32 size);
static void  axse_copy(char *dst, const char *src);
static u32   axse_add_layer(axse_session_t *session, const char *name, u8 kind,
                            gui_rect_t bounds);
static void  axse_damage_all(axse_session_t *session);
static void  axse_damage(axse_session_t *session, u8 layer, gui_rect_t rect);
static void  axse_order(axse_session_t *session);
static void  axse_present(axse_session_t *session);
static const char *axse_stage_name(u8 stage);
static void  axse_paint_stage(gui_surface_t *surface, const axse_session_t *session);
static void  axse_paint_cursor(gui_surface_t *surface, const axse_session_t *session);
static void  axse_paint_chrome(gui_surface_t *surface, const axse_session_t *session);
static bool_t axse_hit(const axse_session_t *session, i32 x, i32 y, u32 *layer);
static u32   axse_layers_visible(const axse_session_t *session);

static void axse_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axse_copy(char *dst, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i < 23)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static u32 axse_add_layer(axse_session_t *session, const char *name, u8 kind,
                          gui_rect_t bounds)
{
    axse_layer_t *layer;
    if (session->layer_count >= AXSE_MAX_LAYERS)
        return 0xFFFFFFFFu;
    layer = &session->layers[session->layer_count];
    axse_zero(layer, (u32)sizeof(*layer));
    layer->kind = kind;
    layer->bounds = bounds;
    layer->opacity = 0xFF;
    layer->z = (u8)session->layer_count;
    layer->visible = true;
    layer->interactive = (bool_t)(kind != AXSE_LAYER_WALLPAPER);
    axse_copy(layer->name, name);
    session->layer_count++;
    axse_damage(session, (u8)(session->layer_count - 1), bounds);
    return session->layer_count - 1;
}

static void axse_damage(axse_session_t *session, u8 layer, gui_rect_t rect)
{
    axse_damage_t *slot;
    if (session->damage_count >= AXSE_MAX_DAMAGE)
    {
        session->full_redraw = true;
        session->damage_count = 0;
        return;
    }
    slot = &session->damage[session->damage_count];
    slot->rect = rect;
    slot->layer = layer;
    slot->stamp = session->damage_stamp++;
    session->damage_count++;
}

static void axse_damage_all(axse_session_t *session)
{
    session->full_redraw = true;
    session->damage_count = 0;
}

static void axse_order(axse_session_t *session)
{
    u32 i;
    u32 j;
    for (i = 1; i < session->layer_count; i++)
    {
        axse_layer_t key = session->layers[i];
        j = i;
        while (j > 0 && session->layers[j - 1].z > key.z)
        {
            session->layers[j] = session->layers[j - 1];
            j--;
        }
        session->layers[j] = key;
    }
}

static const char *axse_stage_name(u8 stage)
{
    switch (stage)
    {
    case AXSE_STAGE_FIRMWARE: return "firmware";
    case AXSE_STAGE_KERNEL: return "kernel";
    case AXSE_STAGE_SERVICES: return "services";
    case AXSE_STAGE_SESSION: return "session";
    case AXSE_STAGE_DESKTOP: return "desktop";
    default: return "ready";
    }
}

static void axse_paint_stage(gui_surface_t *surface, const axse_session_t *session)
{
    gui_fill_rect(surface, gui_rect(0, 0, session->back_buffer.width,
                                    session->back_buffer.height),
                  GUI_RGB(0x10, 0x12, 0x18));
    gui_text(surface, 24, 24, "AxiomOS", GUI_RGB(0xE8, 0xEA, 0xF2), 3);
    gui_text(surface, 24, 68, axse_stage_name(session->stage),
             GUI_RGB(0x8A, 0x90, 0x9E), 1);
    gui_fill_rect(surface, gui_rect(24, 100, 320, 6), GUI_RGB(0x2A, 0x2E, 0x38));
    gui_fill_rect(surface, gui_rect(24, 100,
                                    (session->stage + 1) * 320 / AXSE_STAGE_COUNT, 6),
                  GUI_RGB(0x59, 0xA8, 0xFF));
    gui_text(surface, 24, 124, session->status, GUI_RGB(0x6C, 0x72, 0x80), 1);
}

static void axse_paint_cursor(gui_surface_t *surface, const axse_session_t *session)
{
    i32 x = session->pointer_x;
    i32 y = session->pointer_y;
    i32 i;
    gui_fill_rect(surface, gui_rect(x, y, 2, 12), GUI_RGB(0xF2, 0xF4, 0xFA));
    for (i = 0; i < 6; i++)
    {
        gui_fill_rect(surface, gui_rect(x + 2 + i, y + i, 1, 12 - i * 2),
                      GUI_RGB(0x20, 0x24, 0x2C));
        gui_fill_rect(surface, gui_rect(x + 1 + i, y + i, 1, 12 - i * 2),
                      GUI_RGB(0xF2, 0xF4, 0xFA));
    }
}

static void axse_paint_chrome(gui_surface_t *surface, const axse_session_t *session)
{
    gui_rect_t bar = gui_rect(0, session->back_buffer.height - 40,
                              session->back_buffer.width, 40);
    gui_blend_rect(surface, bar, GUI_ARGB(0xCC, 0x18, 0x1A, 0x22));
    gui_text(surface, 16, bar.y + 12, "Axiom", GUI_RGB(0xD8, 0xDC, 0xE6), 1);
    gui_text(surface, 80, bar.y + 12, session->task_view ? "task view" : "",
             GUI_RGB(0x8A, 0x90, 0x9E), 1);
    gui_text(surface, bar.x + bar.w - 120, bar.y + 12, axse_stage_name(session->stage),
             GUI_RGB(0x8A, 0x90, 0x9E), 1);
}

static void axse_present(axse_session_t *session)
{
    u32 i;
    if (session->screen == NULL_PTR)
    {
        session->damage_count = 0;
        session->full_redraw = false;
        return;
    }
    if (session->full_redraw)
    {
        gui_blit_surface(session->screen, 0, 0, &session->back_buffer);
        gui_blit_surface(session->screen, 0, 0, &session->back_buffer);
        session->presented++;
    }
    else
    {
        for (i = 0; i < session->damage_count; i++)
        {
            gui_rect_t rect = gui_rect_intersect(session->damage[i].rect,
                                                 gui_rect(0, 0,
                                                          session->back_buffer.width,
                                                          session->back_buffer.height));
            if (rect.w <= 0 || rect.h <= 0)
                continue;
            gui_blit(session->screen, rect.x, rect.y, &session->back_buffer, rect);
            session->presented++;
        }
    }
    session->damage_count = 0;
    session->full_redraw = false;
}

static u32 axse_layers_visible(const axse_session_t *session)
{
    u32 i;
    u32 count = 0;
    for (i = 0; i < session->layer_count; i++)
    {
        if (session->layers[i].visible)
            count++;
    }
    return count;
}

static bool_t axse_hit(const axse_session_t *session, i32 x, i32 y, u32 *layer)
{
    u32 i;
    for (i = session->layer_count; i > 0; i--)
    {
        const axse_layer_t *candidate = &session->layers[i - 1];
        if (!candidate->visible || !candidate->interactive)
            continue;
        if (gui_rect_contains(candidate->bounds, x, y))
        {
            *layer = i - 1;
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------- public API */

bool_t axse_init(axse_session_t *session, i32 width, i32 height)
{
    axse_zero(session, (u32)sizeof(*session));
    session->back_buffer.pixels = NULL_PTR;
    session->back_buffer.width = width;
    session->back_buffer.height = height;
    session->back_buffer.stride = width;
    session->layer_count = 0;
    session->stage = AXSE_STAGE_FIRMWARE;
    session->pointer_x = width / 2;
    session->pointer_y = height / 2;
    session->status[0] = 'A';
    session->status[1] = 'x';
    session->status[2] = 'i';
    session->status[3] = 'o';
    session->status[4] = 'm';
    session->status[5] = 'O';
    session->status[6] = 'S';
    session->status[7] = ' ';
    session->status[8] = 's';
    session->status[9] = 't';
    session->status[10] = 'a';
    session->status[11] = 'r';
    session->status[12] = 't';
    session->status[13] = 'i';
    session->status[14] = 'n';
    session->status[15] = 'g';
    session->status[16] = 0;
    session->focus_layer = 0xFF;
    axse_add_layer(session, "wallpaper", AXSE_LAYER_WALLPAPER,
                   gui_rect(0, 0, width, height - 40));
    axse_damage_all(session);
    return true;
}

void axse_attach_screen(axse_session_t *session, gui_surface_t *screen,
                        u32 *pixels)
{
    session->screen = screen;
    session->back_buffer.pixels = pixels;
    axse_damage_all(session);
}

bool_t axse_advance(axse_session_t *session)
{
    if (session->stage < AXSE_STAGE_READY)
    {
        session->stage++;
        session->stage_ticks = 0;
        return true;
    }
    return false;
}

void axse_set_stage(axse_session_t *session, u8 stage)
{
    if (stage > AXSE_STAGE_READY)
        stage = AXSE_STAGE_READY;
    session->stage = stage;
    session->stage_ticks = 0;
}

u8 axse_stage(const axse_session_t *session)
{
    return session->stage;
}

u32 axse_open_window_layer(axse_session_t *session, const char *name,
                           gui_rect_t bounds)
{
    return axse_add_layer(session, name, AXSE_LAYER_WINDOW, bounds);
}

void axse_close_layer(axse_session_t *session, u32 layer)
{
    u32 i;
    if (layer >= session->layer_count)
        return;
    session->layers[layer].visible = false;
    for (i = layer; i + 1 < session->layer_count; i++)
        session->layers[i] = session->layers[i + 1];
    session->layer_count--;
}

void axse_focus(axse_session_t *session, u32 layer)
{
    i32 i;
    i32 best_z = 0;
    if (layer >= session->layer_count)
        return;
    for (i = 0; i < (i32)session->layer_count; i++)
    {
        if (session->layers[i].z > best_z)
            best_z = session->layers[i].z;
    }
    session->layers[layer].z = (u8)(best_z + 1);
    if (session->layers[layer].z < best_z)
        session->layers[layer].z = (u8)best_z;
    session->focus_layer = (u8)layer;
    axse_order(session);
}

void axse_pointer_move(axse_session_t *session, i32 x, i32 y)
{
    session->pointer_x = x;
    session->pointer_y = y;
    axse_damage(session, 0xFF, gui_rect(x - 4, y - 4, 20, 24));
}

bool_t axse_pointer_down(axse_session_t *session, i32 x, i32 y)
{
    u32 layer = 0;
    session->pointer_down = true;
    session->pointer_x = x;
    session->pointer_y = y;
    if (!axse_hit(session, x, y, &layer))
        return false;
    axse_focus(session, layer);
    if (session->layers[layer].kind == AXSE_LAYER_OVERLAY)
        session->launcher = false;
    return true;
}

void axse_pointer_up(axse_session_t *session)
{
    session->pointer_down = false;
}

void axse_set_lock(axse_session_t *session, bool_t locked)
{
    session->locked = locked;
    axse_damage_all(session);
}

void axse_set_task_view(axse_session_t *session, bool_t enabled)
{
    session->task_view = enabled;
    axse_damage_all(session);
}

void axse_set_launcher(axse_session_t *session, bool_t enabled)
{
    session->launcher = enabled;
    axse_damage_all(session);
}

void axse_tick(axse_session_t *session)
{
    session->uptime_ticks++;
    session->stage_ticks++;
}

void axse_draw_begin(axse_session_t *session)
{
    if (session->back_buffer.pixels == NULL_PTR)
        return;
    if (session->stage < AXSE_STAGE_DESKTOP)
    {
        axse_paint_stage(&session->back_buffer, session);
        axse_paint_cursor(&session->back_buffer, session);
        axse_damage_all(session);
        return;
    }
    if (session->locked)
    {
        gui_fill_rect(&session->back_buffer,
                      gui_rect(0, 0, session->back_buffer.width,
                               session->back_buffer.height),
                      GUI_RGB(0x14, 0x16, 0x1C));
        gui_text(&session->back_buffer, 40, session->back_buffer.height / 2 - 40,
                 "Locked", GUI_RGB(0xD8, 0xDC, 0xE6), 3);
        gui_text(&session->back_buffer, 40, session->back_buffer.height / 2 + 4,
                 "Type to unlock", GUI_RGB(0x8A, 0x90, 0x9E), 1);
        axse_paint_cursor(&session->back_buffer, session);
        axse_damage_all(session);
        return;
    }
    axse_paint_stage(&session->back_buffer, session);
    axse_paint_chrome(&session->back_buffer, session);
    axse_paint_cursor(&session->back_buffer, session);
}

void axse_draw_end(axse_session_t *session)
{
    session->frames++;
    axse_present(session);
}

void axse_report(const axse_session_t *session)
{
    printk("axse: stage=%s layers=%u visible=%u frames=%u presented=%u\n",
           axse_stage_name(session->stage), (unsigned)session->layer_count,
           (unsigned)axse_layers_visible(session), (unsigned)session->frames,
           (unsigned)session->presented);
}

bool_t axse_selftest(void)
{
    static axse_session_t session;
    u32 layer;
    if (!axse_init(&session, 1024, 768))
        return false;
    if (session.layer_count != 1)
        return false;
    layer = axse_open_window_layer(&session, "test", gui_rect(100, 100, 400, 300));
    if (layer != 1)
        return false;
    if (!axse_advance(&session))
        return false;
    if (axse_stage(&session) != AXSE_STAGE_KERNEL)
        return false;
    axse_set_stage(&session, AXSE_STAGE_DESKTOP);
    if (!axse_pointer_down(&session, 200, 200))
        return false;
    axse_pointer_up(&session);
    if (session.focus_layer != 1)
        return false;
    axse_close_layer(&session, 1);
    if (session.layer_count != 1)
        return false;
    axse_set_lock(&session, true);
    axse_set_lock(&session, false);
    return true;
}
