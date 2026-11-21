/*
 * AxiomOS - desktop controller.
 *
 * This is the piece that turns the individual Aurora surfaces into a session:
 * it owns the wallpaper, the workspace of windows, the task view, the search
 * panel and the gesture recogniser, and it decides which of them receives an
 * event.  Everything below it is a paint routine that takes a rect; everything
 * above it is the boot path calling axad_frame.
 *
 * Routing order is explicit and fixed: overlays first, then the focused window,
 * then the desktop itself.  A search panel that let clicks through to a window
 * underneath would be a bug that only shows up in front of a user.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"
#include "aurora_apps.h"

#define AXAD_MAX_WALLPAPERS 4

typedef enum axad_wallpaper {
    AXAD_WALLPAPER_DUSK = 0,
    AXAD_WALLPAPER_FOREST,
    AXAD_WALLPAPER_PAPER,
    AXAD_WALLPAPER_NIGHT
} axad_wallpaper_t;

typedef struct axad_desktop {
    gui_rect_t        bounds;
    gui_rect_t        work_area;      /* bounds minus the shell bar */
    axau_workspace_t  workspace;
    axau_taskview_t   taskview;
    axau_search_panel_t search;
    axad_wallpaper_t  wallpaper;
    i32               hover_window;
    i32               drag_window;
    i32               drag_offset_x;
    i32               drag_offset_y;
    axau_frame_region_t drag_region;
    bool_t            dragging;
    bool_t            resizing;
    gui_rect_t        resize_start;
    i32               resize_pointer_x;
    i32               resize_pointer_y;
    i32               frame_ticks;
    bool_t            overlay_open;
    i32               overlay_kind;    /* 1 task view, 2 search */
} axad_desktop_t;

/* ------------------------------------------------------------------ */
/* Wallpaper                                                           */
/* ------------------------------------------------------------------ */

/* Wallpapers are generated, not sampled: two vertical gradients and a soft
 * horizon band.  A generated wallpaper follows the theme, and the alternative —
 * a PNG decoder and a bitmap asset — is a subsystem this shell does not have. */
static void axad_paint_wallpaper(gui_surface_t *surface, axad_desktop_t *desktop,
                                 const axau_palette_t *palette, i32 accent)
{
    u32 top = 0;
    u32 bottom = 0;
    switch (desktop->wallpaper) {
    case AXAD_WALLPAPER_FOREST:
        top = axui_darken(accent, 55);
        bottom = axui_darken(accent, 20);
        break;
    case AXAD_WALLPAPER_PAPER:
        top = 0x00F4F1EA;
        bottom = 0x00D8D2C4;
        break;
    case AXAD_WALLPAPER_NIGHT:
        top = 0x000B1020;
        bottom = 0x00182030;
        break;
    case AXAD_WALLPAPER_DUSK:
    default:
        top = axui_darken(axau_colour(palette, AXAU_COLOUR_SURFACE), 30);
        bottom = axui_lighten(accent, 10);
        break;
    }
    gui_vgradient(surface, desktop->bounds, top, bottom);

    /* A single broad arc suggests a horizon without pretending to be a
     * photograph; it also gives the eye a place to rest behind the windows. */
    i32 horizon_y = desktop->bounds.y + desktop->bounds.h * 2 / 3;
    for (i32 i = 0; i < 5; i++) {
        axui_circle(surface, desktop->bounds.x + desktop->bounds.w / 2,
                    horizon_y + 220 - i * 12, 260 + i * 30,
                    axui_colour_opacity(accent, 4 + i));
    }

    if (desktop->wallpaper == AXAD_WALLPAPER_NIGHT) {
        /* Stars are placed on a deterministic grid: the pattern is stable across
         * frames, which a pseudo-random placement would not be. */
        for (i32 i = 0; i < 40; i++) {
            i32 x = desktop->bounds.x + ((i * 137) % desktop->bounds.w);
            i32 y = desktop->bounds.y + ((i * 89) % (desktop->bounds.h / 2));
            axui_circle(surface, x, y, (i % 3 == 0) ? 1 : 0,
                        axui_colour_opacity(0x00FFFFFF, 40 + (i % 5) * 10));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void axad_init(axad_desktop_t *desktop, gui_rect_t bounds, i32 bar_height)
{
    if (!desktop) return;
    ax_memset(desktop, 0, sizeof(*desktop));
    desktop->bounds = bounds;
    desktop->work_area = gui_rect(bounds.x, bounds.y, bounds.w,
                                  bounds.h - bar_height);
    axau_workspace_init(&desktop->workspace, desktop->work_area);
    axau_taskview_init(&desktop->taskview, desktop->work_area);
    axau_search_init(&desktop->search,
                     gui_rect(bounds.x + bounds.w / 2 - 320,
                              bounds.y + 72, 640, 420));
    desktop->hover_window = -1;
    desktop->drag_window = -1;
    desktop->wallpaper = AXAD_WALLPAPER_DUSK;
}

void axad_set_wallpaper(axad_desktop_t *desktop, axad_wallpaper_t wallpaper)
{
    if (!desktop) return;
    desktop->wallpaper = wallpaper;
}

i32 axad_open_window(axad_desktop_t *desktop, const char *title,
                     gui_rect_t frame, const char *glyph)
{
    if (!desktop) return -1;
    return axau_workspace_add(&desktop->workspace, title, frame, glyph);
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

void axad_frame(gui_surface_t *surface, axad_desktop_t *desktop,
                const axau_palette_t *palette, const axau_metrics_t *metrics,
                u32 accent)
{
    if (!surface || !desktop || !palette) return;
    axad_paint_wallpaper(surface, desktop, palette, (i32)accent);

    axau_workspace_paint(surface, &desktop->workspace, palette, metrics);

    if (desktop->taskview.open) {
        axau_taskview_paint(surface, &desktop->taskview, &desktop->workspace,
                            palette);
    }
    if (desktop->search.open) {
        axau_search_paint(surface, &desktop->search, palette, metrics);
    }

    /* Window count in the corner is diagnostic, not decoration: it gives a
     * visible cue that the workspace is tracking something. */
    if (desktop->frame_ticks % 600 == 0) {
        char status[48];
        ax_snprintf(status, sizeof(status), "%d windows / %d visible",
                    desktop->workspace.count,
                    axau_workspace_visible_count(&desktop->workspace));
        axui_draw_text(surface, desktop->bounds.x + 12,
                       desktop->bounds.y + desktop->bounds.h - 40, status,
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
    desktop->frame_ticks++;
}

void axad_tick(axad_desktop_t *desktop)
{
    if (!desktop) return;
    axau_workspace_tick(&desktop->workspace);
    axau_taskview_tick(&desktop->taskview);
    axau_search_tick(&desktop->search);
}

/* ------------------------------------------------------------------ */
/* Event routing                                                       */
/* ------------------------------------------------------------------ */

/* Pointer move is split into three concerns: overlay hover, window hover and
 * drag.  They are not exclusive — a drag keeps the window it started on even if
 * the pointer leaves its bounds. */
i32 axad_pointer_move(axad_desktop_t *desktop, i32 x, i32 y)
{
    if (!desktop) return 0;
    if (desktop->taskview.open) {
        i32 card_w, card_h, gap;
        (void)card_h;
        (void)gap;
        (void)card_w;
        desktop->taskview.hover = axau_taskview_hit(&desktop->taskview,
                                                    &desktop->workspace, x, y);
        return 1;
    }
    if (desktop->search.open) {
        desktop->search.hover = axau_search_hit(&desktop->search, x, y);
        return 1;
    }

    if (desktop->dragging && desktop->drag_window >= 0) {
        axau_window_t *window = &desktop->workspace.windows[desktop->drag_window];
        if (desktop->resizing) {
            i32 dx = x - desktop->resize_pointer_x;
            i32 dy = y - desktop->resize_pointer_y;
            gui_rect_t target = desktop->resize_start;
            switch (desktop->drag_region) {
            case AXAU_REGION_BORDER_LEFT:
                target.x += dx;
                target.w -= dx;
                break;
            case AXAU_REGION_BORDER_RIGHT:
                target.w += dx;
                break;
            case AXAU_REGION_BOTTOM:
                target.h += dy;
                break;
            case AXAU_REGION_BOTTOM_LEFT:
                target.x += dx;
                target.w -= dx;
                target.h += dy;
                break;
            case AXAU_REGION_BOTTOM_RIGHT:
                target.w += dx;
                target.h += dy;
                break;
            default:
                break;
            }
            axau_window_resize_to(window, target, desktop->work_area);
        } else {
            i32 nx = x - desktop->drag_offset_x;
            i32 ny = y - desktop->drag_offset_y;
            /* Keep a strip of the titlebar on screen: a window dragged entirely
             * off-screen can never be recovered with a pointer. */
            if (ny < desktop->work_area.y) ny = desktop->work_area.y;
            if (nx < desktop->work_area.x - window->frame.w + 80) {
                nx = desktop->work_area.x - window->frame.w + 80;
            }
            if (nx > desktop->work_area.x + desktop->work_area.w - 80) {
                nx = desktop->work_area.x + desktop->work_area.w - 80;
            }
            axau_window_move(window, nx, ny);
            /* Snap preview appears when the pointer nears an edge; the snap
             * itself happens on release so the user can still change their mind. */
            i32 edge = AXAU_SNAP_THRESHOLD;
            if (x <= desktop->work_area.x + edge) {
                window->snap_preview = 1000;
            } else if (x >= desktop->work_area.x + desktop->work_area.w - edge) {
                window->snap_preview = 1000;
            } else if (y <= desktop->work_area.y + edge) {
                window->snap_preview = 1000;
            } else {
                window->snap_preview = 0;
            }
        }
        return 1;
    }

    i32 index = axau_workspace_find(&desktop->workspace, x, y);
    desktop->hover_window = index;
    if (index >= 0) {
        axau_window_t *window = &desktop->workspace.windows[index];
        axau_frame_region_t region = axau_window_hit(window, x, y, 0, 0, 0);
        (void)region;
    }
    return 0;
}

/* Returns a small code describing what happened so the shell can update the
 * cursor shape: 1 drag started, 2 resize started, 3 window activated, 4 handled
 * by an overlay, 0 nothing. */
i32 axad_pointer_down(axad_desktop_t *desktop, i32 x, i32 y, u32 button)
{
    if (!desktop) return 0;
    if (desktop->taskview.open) {
        i32 index = axau_taskview_hit(&desktop->taskview, &desktop->workspace,
                                      x, y);
        if (index >= 0) {
            axau_taskview_close(&desktop->taskview);
            axau_workspace_activate(&desktop->workspace, index);
            desktop->overlay_open = (bool_t)0;
        } else {
            axau_taskview_close(&desktop->taskview);
            desktop->overlay_open = (bool_t)0;
        }
        return 4;
    }
    if (desktop->search.open) {
        if (!gui_rect_contains(desktop->search.rect, x, y)) {
            axau_search_close(&desktop->search);
            desktop->overlay_open = (bool_t)0;
            return 4;
        }
        i32 item = axau_search_hit(&desktop->search, x, y);
        if (item >= 0) {
            /* Opening a result is the shell's job: this controller only reports
             * that a result was chosen via the return code. */
            return 100 + item;
        }
        return 4;
    }

    i32 index = axau_workspace_find(&desktop->workspace, x, y);
    if (index < 0) return 0;
    axau_window_t *window = &desktop->workspace.windows[index];
    bool_t on_close = (bool_t)0;
    bool_t on_max = (bool_t)0;
    bool_t on_min = (bool_t)0;
    axau_frame_region_t region = axau_window_hit(window, x, y, &on_close,
                                                 &on_max, &on_min);
    if (on_close) {
        if (desktop->workspace.active == index) {
            axau_workspace_close(&desktop->workspace, index);
        }
        return 5;
    }
    if (on_max) {
        if (window->state == AXAU_WINDOW_MAXIMISED) {
            axau_window_restore(window);
        } else {
            axau_window_maximise(window, desktop->work_area);
        }
        return 6;
    }
    if (on_min) {
        window->state = AXAU_WINDOW_MINIMISED;
        return 7;
    }

    axau_workspace_activate(&desktop->workspace, index);
    desktop->drag_window = index;
    desktop->dragging = (bool_t)1;
    if (region >= AXAU_REGION_BORDER_LEFT && region <= AXAU_REGION_BOTTOM_RIGHT &&
        region != AXAU_REGION_TITLEBAR) {
        desktop->resizing = (bool_t)1;
        desktop->drag_region = region;
        desktop->resize_start = window->frame;
        desktop->resize_pointer_x = x;
        desktop->resize_pointer_y = y;
        return 2;
    }
    desktop->resizing = (bool_t)0;
    desktop->drag_offset_x = x - window->frame.x;
    desktop->drag_offset_y = y - window->frame.y;
    return 1 + index;
}

/* Release applies the snap that was previewed during the drag.  Doing it here,
 * rather than during the drag, is what lets a user abort by moving back to the
 * middle before letting go. */
i32 axad_pointer_up(axad_desktop_t *desktop, i32 x, i32 y)
{
    if (!desktop) return 0;
    i32 index = desktop->drag_window;
    desktop->dragging = (bool_t)0;
    desktop->resizing = (bool_t)0;
    desktop->drag_window = -1;
    if (index < 0 || index >= desktop->workspace.count) return 0;
    axau_window_t *window = &desktop->workspace.windows[index];
    if (window->snap_preview <= 0) {
        window->snap_preview = 0;
        return 1;
    }
    i32 edge = AXAU_SNAP_THRESHOLD;
    if (x <= desktop->work_area.x + edge) {
        axau_window_snap(window, AXAU_SNAP_LEFT, desktop->work_area);
    } else if (x >= desktop->work_area.x + desktop->work_area.w - edge) {
        axau_window_snap(window, AXAU_SNAP_RIGHT, desktop->work_area);
    } else if (y <= desktop->work_area.y + edge) {
        axau_window_snap(window, AXAU_SNAP_TOP, desktop->work_area);
    }
    window->snap_preview = 0;
    return 2;
}

/* Keyboard routing: overlays consume first; the shell's global shortcuts are the
 * caller's responsibility and must not be swallowed here. */
i32 axad_key(axad_desktop_t *desktop, i32 key, i32 modifiers)
{
    if (!desktop) return 0;
    (void)modifiers;
    if (desktop->search.open) {
        if (key == 0x1B) {
            axau_search_close(&desktop->search);
            desktop->overlay_open = (bool_t)0;
            return 1;
        }
        if (key == 0x08) {
            axau_search_backspace(&desktop->search);
            return 1;
        }
        if (key == 0x0D) {
            i32 selected = desktop->search.selected;
            axau_search_close(&desktop->search);
            desktop->overlay_open = (bool_t)0;
            return selected >= 0 ? 100 + selected : 1;
        }
        if (key >= 32 && key < 127) {
            axau_search_type(&desktop->search, (char)key);
            return 1;
        }
        return 1;
    }
    if (desktop->taskview.open) {
        if (key == 0x1B) {
            axau_taskview_close(&desktop->taskview);
            desktop->overlay_open = (bool_t)0;
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Overlay control                                                     */
/* ------------------------------------------------------------------ */

bool_t axad_open_taskview(axad_desktop_t *desktop)
{
    if (!desktop) return (bool_t)0;
    if (desktop->search.open) axau_search_close(&desktop->search);
    axau_taskview_open(&desktop->taskview);
    desktop->overlay_open = (bool_t)1;
    desktop->overlay_kind = 1;
    return (bool_t)1;
}

bool_t axad_open_search(axad_desktop_t *desktop, const char *query)
{
    if (!desktop) return (bool_t)0;
    if (desktop->taskview.open) axau_taskview_close(&desktop->taskview);
    axau_search_open(&desktop->search, query);
    desktop->overlay_open = (bool_t)1;
    desktop->overlay_kind = 2;
    return (bool_t)1;
}

bool_t axad_close_overlay(axad_desktop_t *desktop)
{
    if (!desktop) return (bool_t)0;
    if (desktop->taskview.open) axau_taskview_close(&desktop->taskview);
    if (desktop->search.open) axau_search_close(&desktop->search);
    desktop->overlay_open = (bool_t)0;
    desktop->overlay_kind = 0;
    return (bool_t)1;
}

bool_t axad_any_overlay(const axad_desktop_t *desktop)
{
    if (!desktop) return (bool_t)0;
    return (bool_t)(desktop->taskview.open || desktop->search.open);
}

i32 axad_window_count(const axad_desktop_t *desktop)
{
    return desktop ? desktop->workspace.count : 0;
}

void axad_dump(const axad_desktop_t *desktop)
{
    if (!desktop) return;
    printk("desktop: %d windows, overlay=%d, wallpaper=%d, ticks=%d\n",
           desktop->workspace.count, desktop->overlay_kind,
           (i32)desktop->wallpaper, desktop->frame_ticks);
    const axau_window_t *active = desktop->workspace.active >= 0
                                ? &desktop->workspace.windows[desktop->workspace.active]
                                : 0;
    if (active) {
        printk("desktop: active '%s' at %d,%d %dx%d\n", active->title,
               active->frame.x, active->frame.y, active->frame.w,
               active->frame.h);
    }
}

