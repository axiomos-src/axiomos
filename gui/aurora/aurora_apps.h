/*
 * AxiomOS - Aurora application layer shared types.
 *
 * The Aurora design system describes how a control looks and behaves; this
 * header describes what a *place* is: a window with chrome and state, a
 * workspace holding windows, and the descriptors that let the shell list
 * applications without knowing anything about their internals.
 *
 * Everything here is pure data plus functions that operate on the caller's
 * structs.  No allocation, no globals beyond the shell singleton, so a window
 * can live on an application's own stack and be painted by a shared routine.
 */

#ifndef AXIOM_AURORA_APPS_H
#define AXIOM_AURORA_APPS_H

#include <axiom/types.h>
#include "gui.h"
#include "aurora.h"

#define AXAU_MAX_WINDOWS      16
#define AXAU_MAX_WORKSPACES   4
#define AXAU_TITLEBAR_HEIGHT  36
#define AXAU_RESIZE_MARGIN     6
#define AXAU_SNAP_THRESHOLD   24

/* Window lifecycle.  A window is "opening" while its scale animation runs and
 * "closing" while it fades; the workspace keeps it in the list during both so
 * the animation has something to draw. */
typedef enum axau_window_state {
    AXAU_WINDOW_HIDDEN = 0,
    AXAU_WINDOW_OPENING,
    AXAU_WINDOW_NORMAL,
    AXAU_WINDOW_MINIMISED,
    AXAU_WINDOW_MAXIMISED,
    AXAU_WINDOW_CLOSING
} axau_window_state_t;

typedef enum axau_snap_edge {
    AXAU_SNAP_NONE = 0,
    AXAU_SNAP_LEFT,
    AXAU_SNAP_RIGHT,
    AXAU_SNAP_TOP,
    AXAU_SNAP_BOTTOM_LEFT,
    AXAU_SNAP_BOTTOM_RIGHT,
    AXAU_SNAP_QUARTER_LEFT,
    AXAU_SNAP_QUARTER_RIGHT
} axau_snap_edge_t;

/* Hit-test regions of the frame.  The client area is everything inside the
 * border; the chrome regions exist so the shell can start a drag or resize
 * without the application seeing the pointer. */
typedef enum axau_frame_region {
    AXAU_REGION_CLIENT = 0,
    AXAU_REGION_TITLEBAR,
    AXAU_REGION_CAPTION_BUTTONS,
    AXAU_REGION_BORDER_LEFT,
    AXAU_REGION_BORDER_RIGHT,
    AXAU_REGION_BORDER_TOP,
    AXAU_REGION_BOTTOM,
    AXAU_REGION_BOTTOM_LEFT,
    AXAU_REGION_BOTTOM_RIGHT,
    AXAU_REGION_OUTSIDE
} axau_frame_region_t;

typedef struct axau_window {
    gui_rect_t          frame;          /* outer frame, including border */
    gui_rect_t          restore;        /* frame to return to when un-maximising */
    gui_rect_t          content;        /* cached client rect for this frame */
    char                title[64];
    char                subtitle[80];
    const char         *glyph;          /* icon token drawn in the titlebar */
    u32                 accent;
    axau_window_state_t state;
    axau_snap_edge_t    snap;
    i32                 workspace;
    i32                 z_order;
    i32                 id;
    bool_t              resizable;
    bool_t              closable;
    bool_t              maximisable;
    bool_t              active;
    bool_t              dirty;
    i32                 open_progress;  /* 0..1000, drives the open animation */
    i32                 close_progress; /* 0..1000, drives the close animation */
    i32                 snap_preview;   /* 0..1000, snap assist visibility */
    i32                 drag_dx;
    i32                 drag_dy;
    bool_t              dragging;
    bool_t              resizing;
    axau_frame_region_t resize_region;
} axau_window_t;

typedef struct axau_workspace {
    axau_window_t windows[AXAU_MAX_WINDOWS];
    i32           count;
    i32           active;
    i32           current;      /* visible workspace index */
    i32           workspace_count;
    gui_rect_t    bounds;
    i32           next_id;
} axau_workspace_t;

/* An application descriptor is a name, an icon token, and an opaque tag that
 * the shell hands back when the tile is clicked. */
typedef struct axau_app_desc {
    char        name[40];
    char        subtitle[56];
    const char *glyph;
    const char *tag;
    u32         colour;
} axau_app_desc_t;

/* ------------------------------------------------------------------ */
/* Window API                                                          */
/* ------------------------------------------------------------------ */

void axau_window_init(axau_window_t *window, const char *title, gui_rect_t frame,
                      const char *glyph);
void axau_window_set_subtitle(axau_window_t *window, const char *subtitle);
gui_rect_t axau_window_content_rect(const axau_window_t *window);
void axau_window_move(axau_window_t *window, i32 x, i32 y);
void axau_window_resize_to(axau_window_t *window, gui_rect_t frame,
                           gui_rect_t bounds);
void axau_window_maximise(axau_window_t *window, gui_rect_t bounds);
void axau_window_restore(axau_window_t *window);
void axau_window_snap(axau_window_t *window, axau_snap_edge_t edge,
                      gui_rect_t bounds);
void axau_window_begin_open(axau_window_t *window);
void axau_window_begin_close(axau_window_t *window);
bool_t axau_window_tick(axau_window_t *window);
i32 axau_window_scale(const axau_window_t *window);
i32 axau_window_alpha(const axau_window_t *window);
axau_frame_region_t axau_window_hit(const axau_window_t *window, i32 x, i32 y,
                                    bool_t *on_close, bool_t *on_max,
                                    bool_t *on_min);
void axau_window_paint(gui_surface_t *surface, axau_window_t *window,
                       const axau_palette_t *palette, const axau_metrics_t *metrics);
void axau_window_paint_frame_only(gui_surface_t *surface, axau_window_t *window,
                                  const axau_palette_t *palette);
void axau_window_paint_shadow(gui_surface_t *surface, const axau_window_t *window,
                              const axau_palette_t *palette);

/* ------------------------------------------------------------------ */
/* Workspace API                                                       */
/* ------------------------------------------------------------------ */

void axau_workspace_init(axau_workspace_t *workspace, gui_rect_t bounds);
i32  axau_workspace_add(axau_workspace_t *workspace, const char *title,
                        gui_rect_t frame, const char *glyph);
i32  axau_workspace_find(axau_workspace_t *workspace, i32 x, i32 y);
void axau_workspace_activate(axau_workspace_t *workspace, i32 index);
void axau_workspace_raise(axau_workspace_t *workspace, i32 index);
void axau_workspace_close(axau_workspace_t *workspace, i32 index);
void axau_workspace_tick(axau_workspace_t *workspace);
i32  axau_workspace_visible_count(const axau_workspace_t *workspace);
void axau_workspace_paint(gui_surface_t *surface, axau_workspace_t *workspace,
                          const axau_palette_t *palette, const axau_metrics_t *metrics);
bool_t axau_workspace_cycle(axau_workspace_t *workspace, i32 delta);
bool_t axau_workspace_snap_active(axau_workspace_t *workspace, axau_snap_edge_t edge);

/* ------------------------------------------------------------------ */
/* Task view and search                                                */
/* ------------------------------------------------------------------ */

typedef struct axau_taskview {
    gui_rect_t     rect;
    i32            hover;
    i32            selected;
    bool_t         open;
    i32            progress;    /* 0..1000 */
    bool_t         closing;
} axau_taskview_t;

void axau_taskview_init(axau_taskview_t *view, gui_rect_t rect);
void axau_taskview_open(axau_taskview_t *view);
void axau_taskview_close(axau_taskview_t *view);
bool_t axau_taskview_tick(axau_taskview_t *view);
void axau_taskview_paint(gui_surface_t *surface, axau_taskview_t *view,
                         axau_workspace_t *workspace,
                         const axau_palette_t *palette);
i32  axau_taskview_hit(axau_taskview_t *view, axau_workspace_t *workspace,
                       i32 x, i32 y);

typedef struct axau_search_item {
    char        title[48];
    char        detail[64];
    const char *glyph;
    const char *section;
} axau_search_item_t;

typedef struct axau_search_panel {
    gui_rect_t       rect;
    axau_search_item_t items[24];
    i32              item_count;
    i32              matches[24];
    i32              match_count;
    i32              hover;
    i32              selected;
    char             query[48];
    i32              caret;
    bool_t           open;
    i32              progress;
} axau_search_panel_t;

void axau_search_init(axau_search_panel_t *panel, gui_rect_t rect);
void axau_search_add(axau_search_panel_t *panel, const char *title,
                     const char *detail, const char *glyph, const char *section);
void axau_search_open(axau_search_panel_t *panel, const char *query);
void axau_search_close(axau_search_panel_t *panel);
i32  axau_search_perform(axau_search_panel_t *panel);
void axau_search_type(axau_search_panel_t *panel, char ch);
void axau_search_backspace(axau_search_panel_t *panel);
void axau_search_paint(gui_surface_t *surface, axau_search_panel_t *panel,
                       const axau_palette_t *palette, const axau_metrics_t *metrics);
i32  axau_search_hit(axau_search_panel_t *panel, i32 x, i32 y);
bool_t axau_search_tick(axau_search_panel_t *panel);

#endif /* AXIOM_AURORA_APPS_H */
