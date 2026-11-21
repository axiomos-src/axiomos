/*
 * AxiomOS - System monitor page interface.
 */

#ifndef AXIOM_APP_MONITOR_VIEW_H
#define AXIOM_APP_MONITOR_VIEW_H

#include <axiom/types.h>
#include <gui.h>
#include "ui.h"
#include "widgets.h"

#define MON_SERIES_MAX 4
#define MON_SAMPLES 96

typedef struct mon_series {
    const char *name;
    char        samples[MON_SAMPLES];
    i32         head;
    i32         count;
    i32         current;
    i32         peak;
} mon_series_t;

typedef struct monitor_view {
    gui_rect_t   bounds;
    gui_rect_t   cards[MON_SERIES_MAX];
    mon_series_t cpu;
    mon_series_t memory;
    mon_series_t disk;
    mon_series_t network;
    gui_rect_t   graph;
    i32          graph_mode;
    bool_t       history;
    axw_table_t  processes;
} monitor_view_t;

void monitor_init(monitor_view_t *view, gui_rect_t bounds);
void monitor_sample(monitor_view_t *view, i32 cpu, i32 memory, i32 disk, i32 network);
i32  monitor_add_process(monitor_view_t *view, i32 row, i32 pid, const char *name,
                         const char *state, i32 cpu);
void monitor_seed_demo(monitor_view_t *view, i32 samples);
void monitor_paint(gui_surface_t *surface, monitor_view_t *view);
u32  monitor_event(monitor_view_t *view, i32 x, i32 y, u8 buttons, bool_t pressed);
u32  monitor_key(monitor_view_t *view, char key, u8 scancode);

#endif /* AXIOM_APP_MONITOR_VIEW_H */
