/* AxiomOS - Aurora: system monitor.
 *
 * Displays a process table, aggregate CPU and memory graphs, per-core bars,
 * storage usage and a small service list.  The monitor pulls samples from a
 * caller supplied provider so that it can be driven either by real kernel
 * counters or by synthetic data during a UI demo.
 */

#include "gui.h"

#define AXSM_MAX_PROCESSES  64
#define AXSM_MAX_CORES      8
#define AXSM_MAX_DISKS      4
#define AXSM_MAX_SERVICES   12
#define AXSM_HISTORY        64
#define AXSM_TABSTRIP       36
#define AXSM_HEADER         28
#define AXSM_ROW            30
#define AXSM_SIDEBAR         0
#define AXSM_STATUSBAR      22

#define AXSM_C_BG       GUI_RGB(0x1B, 0x1D, 0x23)
#define AXSM_C_PANEL    GUI_RGB(0x23, 0x26, 0x2D)
#define AXSM_C_PANEL2   GUI_RGB(0x2A, 0x2D, 0x35)
#define AXSM_C_LINE     GUI_RGB(0x36, 0x3A, 0x44)
#define AXSM_C_TEXT     GUI_RGB(0xD4, 0xD8, 0xE2)
#define AXSM_C_DIM      GUI_RGB(0x86, 0x8C, 0x9A)
#define AXSM_C_ACCENT   GUI_RGB(0x59, 0xA8, 0xFF)
#define AXSM_C_GREEN    GUI_RGB(0x5C, 0xC0, 0x86)
#define AXSM_C_YELLOW   GUI_RGB(0xE0, 0xB8, 0x58)
#define AXSM_C_RED      GUI_RGB(0xE0, 0x64, 0x5C)
#define AXSM_C_SEL      GUI_RGB(0x2C, 0x38, 0x50)

typedef struct axsyw_process
{
    u32  pid;
    u32  ppid;
    char name[24];
    u8   state;         /* 0 running, 1 sleeping, 2 stopped, 3 zombie */
    u8   priority;
    u32  cpu_permille;
    u64  memory_kb;
    u32  threads;
    u32  handles;
    u64  started_tick;
} axsyw_process_t;

typedef struct axsyw_disk
{
    char label[16];
    char mount[16];
    u64  total_mb;
    u64  used_mb;
    u32  read_kbps;
    u32  write_kbps;
} axsyw_disk_t;

typedef struct axsyw_service
{
    char name[20];
    u8   state;
    u32  restarts;
} axsyw_service_t;

typedef struct axsyw_stats
{
    u32 cpu_percent;
    u32 cpu_history[AXSM_MAX_CORES][AXSM_HISTORY];
    u32 core_percent[AXSM_MAX_CORES];
    u16 core_count;
    u32 memory_total_mb;
    u32 memory_used_mb;
    u32 memory_history[AXSM_HISTORY];
    u32 memory_pressure;
    u32 uptime_seconds;
    u32 interrupts;
    u32 context_switches;
    u32 syscalls;
    u32 page_faults;
    u32 net_rx_kbps;
    u32 net_tx_kbps;
} axsyw_stats_t;

typedef struct axsyw_monitor
{
    axsyw_process_t processes[AXSM_MAX_PROCESSES];
    u16            process_count;
    axsyw_disk_t    disks[AXSM_MAX_DISKS];
    u16            disk_count;
    axsyw_service_t services[AXSM_MAX_SERVICES];
    u16            service_count;
    axsyw_stats_t   stats;
    u8             tab;         /* 0 processes, 1 performance, 2 storage, 3 services */
    u16            selected;
    u16            sort_column; /* 0 name, 1 pid, 2 cpu, 3 memory */
    bool_t         sort_ascending;
    i32            scroll;
    char           search[24];
    u16            search_length;
    u32            samples;
    u32            tick;
} axsyw_monitor_t;

static void axsyw_zero(void *ptr, u32 size);
static u32  axsyw_scale_percent(u32 percent);
static void axsyw_label(char *out, const char *text);
static u32  axsyw_format_int(char *out, u64 value);
static void axsyw_push_history(u32 *history, u32 value);
static void axsyw_add_process(axsyw_monitor_t *mon, u32 pid, const char *name,
                             u32 cpu, u64 memory_mb, u8 state);
static void axsyw_add_disk(axsyw_monitor_t *mon, const char *label,
                          const char *mount, u64 total, u64 used);
static void axsyw_add_service(axsyw_monitor_t *mon, const char *name, u8 state,
                            u32 restarts);
static void axsyw_sample(axsyw_monitor_t *mon, u32 cpu, u32 memory_used_mb);
static void axsyw_sort(axsyw_monitor_t *mon);
static bool_t axsyw_matches(const axsyw_monitor_t *mon, const axsyw_process_t *p);
static void axsyw_paint_tabs(gui_surface_t *surface, const axsyw_monitor_t *mon,
                            gui_rect_t strip);
static void axsyw_paint_processes(gui_surface_t *surface, axsyw_monitor_t *mon,
                                 gui_rect_t area);
static void axsyw_paint_performance(gui_surface_t *surface,
                                   const axsyw_monitor_t *mon, gui_rect_t area);
static void axsyw_paint_storage(gui_surface_t *surface, const axsyw_monitor_t *mon,
                               gui_rect_t area);
static void axsyw_paint_services(gui_surface_t *surface,
                                const axsyw_monitor_t *mon, gui_rect_t area);
static void axsyw_paint_status(gui_surface_t *surface, const axsyw_monitor_t *mon,
                              gui_rect_t bar);
static void axsyw_paint_graph(gui_surface_t *surface, gui_rect_t rect,
                             const u32 *history, u32 maximum, u32 colour);
static void axsyw_paint_core_bars(gui_surface_t *surface,
                                 const axsyw_monitor_t *mon, gui_rect_t rect);
static void axsyw_paint_donut(gui_surface_t *surface, i32 cx, i32 cy,
                             i32 radius, u32 percent, u32 colour);
static u32 axsyw_state_colour(u8 state);
static const char *axsyw_state_label(u8 state);

static void axsyw_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axsyw_scale_percent(u32 percent)
{
    if (percent > 100)
        return 100;
    return percent;
}

static void axsyw_label(char *out, const char *text)
{
    u16 i = 0;
    while (text[i] != 0 && i < 23)
    {
        out[i] = text[i];
        i++;
    }
    out[i] = 0;
}

static u32 axsyw_format_int(char *out, u64 value)
{
    u32 digits = 1;
    u64 tmp = value;
    u32 i;
    while (tmp >= 10)
    {
        tmp /= 10;
        digits++;
    }
    for (i = 0; i < digits; i++)
    {
        out[digits - i - 1] = (char)('0' + (value % 10));
        value /= 10;
    }
    out[digits] = 0;
    return digits;
}

static void axsyw_push_history(u32 *history, u32 value)
{
    u32 i;
    for (i = AXSM_HISTORY - 1; i > 0; i--)
        history[i] = history[i - 1];
    history[0] = value;
}

static void axsyw_add_process(axsyw_monitor_t *mon, u32 pid, const char *name,
                             u32 cpu, u64 memory_mb, u8 state)
{
    axsyw_process_t *p;
    if (mon->process_count >= AXSM_MAX_PROCESSES)
        return;
    p = &mon->processes[mon->process_count];
    axsyw_zero(p, (u32)sizeof(*p));
    p->pid = pid;
    p->ppid = pid > 1 ? 1 : 0;
    axsyw_label(p->name, name);
    p->cpu_permille = axsyw_scale_percent(cpu);
    p->memory_kb = memory_mb * 1024;
    p->threads = 1 + (pid % 4);
    p->handles = 8 + (pid % 40);
    p->state = state;
    p->priority = (u8)(pid % 8);
    mon->process_count++;
}

static void axsyw_add_disk(axsyw_monitor_t *mon, const char *label,
                          const char *mount, u64 total, u64 used)
{
    axsyw_disk_t *d;
    u16 i;
    if (mon->disk_count >= AXSM_MAX_DISKS)
        return;
    d = &mon->disks[mon->disk_count];
    for (i = 0; i < 15 && label[i] != 0; i++)
        d->label[i] = label[i];
    d->label[i] = 0;
    for (i = 0; i < 15 && mount[i] != 0; i++)
        d->mount[i] = mount[i];
    d->mount[i] = 0;
    d->total_mb = total;
    d->used_mb = used;
    d->read_kbps = 12 + (u32)mon->disk_count * 7;
    d->write_kbps = 8 + (u32)mon->disk_count * 5;
    mon->disk_count++;
}

static void axsyw_add_service(axsyw_monitor_t *mon, const char *name, u8 state,
                             u32 restarts)
{
    axsyw_service_t *s;
    u16 i;
    if (mon->service_count >= AXSM_MAX_SERVICES)
        return;
    s = &mon->services[mon->service_count];
    for (i = 0; i < 19 && name[i] != 0; i++)
        s->name[i] = name[i];
    s->name[i] = 0;
    s->state = state;
    s->restarts = restarts;
    mon->service_count++;
}

static void axsyw_sample(axsyw_monitor_t *mon, u32 cpu, u32 memory_used_mb)
{
    u16 core;
    mon->samples++;
    mon->stats.cpu_percent = axsyw_scale_percent(cpu);
    axsyw_push_history(mon->stats.memory_history, memory_used_mb);
    mon->stats.memory_used_mb = memory_used_mb;
    mon->stats.memory_pressure =
        (u32)((u64)mon->stats.memory_used_mb * 100 /
              (mon->stats.memory_total_mb > 0 ? mon->stats.memory_total_mb : 1));
    for (core = 0; core < mon->stats.core_count; core++)
    {
        u32 value = (u32)((cpu + core * 13) % 100);
        mon->stats.core_percent[core] = axsyw_scale_percent(value);
        axsyw_push_history(mon->stats.cpu_history[core], value);
    }
    mon->stats.interrupts += 1024 + cpu * 8;
    mon->stats.context_switches += 256 + cpu * 3;
    mon->stats.syscalls += 512;
    mon->stats.page_faults += 12;
    mon->stats.net_rx_kbps = 40 + cpu;
    mon->stats.net_tx_kbps = 18 + cpu / 2;
    mon->stats.uptime_seconds += 1;
}

static bool_t axsyw_matches(const axsyw_monitor_t *mon, const axsyw_process_t *p)
{
    u16 i;
    u16 j;
    if (mon->search_length == 0)
        return true;
    if (p->name[0] == 0)
        return false;
    for (i = 0; i + mon->search_length <= 24; i++)
    {
        bool_t hit = true;
        if (p->name[i] == 0)
            break;
        for (j = 0; j < mon->search_length; j++)
        {
            char a = p->name[i + j];
            char b = mon->search[i];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + 32);
            if (a == 0 || a != b)
            {
                hit = false;
                break;
            }
        }
        if (hit)
            return true;
    }
    return false;
}

static void axsyw_sort(axsyw_monitor_t *mon)
{
    u16 i;
    u16 j;
    for (i = 1; i < mon->process_count; i++)
    {
        axsyw_process_t key = mon->processes[i];
        j = i;
        while (j > 0)
        {
            axsyw_process_t *prev = &mon->processes[j - 1];
            bool_t swap = false;
            switch (mon->sort_column)
            {
            case 0:
                swap = (bool_t)(key.name[0] < prev->name[0]);
                if (mon->sort_ascending)
                    swap = (bool_t)(key.name[0] > prev->name[0]);
                break;
            case 1:
                swap = (bool_t)(mon->sort_ascending ? key.pid < prev->pid
                                                    : key.pid > prev->pid);
                break;
            case 2:
                swap = (bool_t)(mon->sort_ascending ? key.cpu_permille < prev->cpu_permille
                                                    : key.cpu_permille > prev->cpu_permille);
                break;
            default:
                swap = (bool_t)(mon->sort_ascending ? key.memory_kb < prev->memory_kb
                                                    : key.memory_kb > prev->memory_kb);
                break;
            }
            if (!swap)
                break;
            mon->processes[j] = mon->processes[j - 1];
            j--;
        }
        mon->processes[j] = key;
    }
}

/* ------------------------------------------------------------- rendering */

static u32 axsyw_state_colour(u8 state)
{
    switch (state)
    {
    case 0: return AXSM_C_GREEN;
    case 1: return AXSM_C_DIM;
    case 2: return AXSM_C_YELLOW;
    default: return AXSM_C_RED;
    }
}

static const char *axsyw_state_label(u8 state)
{
    switch (state)
    {
    case 0: return "Running";
    case 1: return "Sleeping";
    case 2: return "Stopped";
    default: return "Zombie";
    }
}

static void axsyw_paint_graph(gui_surface_t *surface, gui_rect_t rect,
                             const u32 *history, u32 maximum, u32 colour)
{
    u32 i;
    i32 x;
    i32 last_y = rect.y + rect.h;
    if (maximum == 0)
        maximum = 100;
    gui_fill_rect(surface, rect, AXSM_C_PANEL);
    gui_rect_outline(surface, rect, AXSM_C_LINE);
    for (i = 1; i < 4; i++)
        gui_hline(surface, rect.x, rect.x + rect.w,
                  rect.y + rect.h * i / 4, AXSM_C_LINE);
    for (i = 0; i < AXSM_HISTORY; i++)
    {
        i32 y = rect.y + rect.h - (i32)(history[i] * (u32)rect.h / maximum);
        if (history[i] > maximum)
            y = rect.y;
        x = rect.x + rect.w - (i32)i * rect.w / AXSM_HISTORY;
        if (x < rect.x)
            break;
        gui_vline(surface, x, y, rect.y + rect.h, colour);
        if (i > 0)
            gui_line(surface, x, y, x + rect.w / AXSM_HISTORY, last_y, colour);
        last_y = y;
    }
    gui_fill_rect(surface, gui_rect(rect.x, rect.y, rect.w, 1), AXSM_C_LINE);
}

static void axsyw_paint_core_bars(gui_surface_t *surface,
                                 const axsyw_monitor_t *mon, gui_rect_t rect)
{
    u16 core;
    i32 bar_width;
    i32 x = rect.x + 8;
    if (mon->stats.core_count == 0)
        return;
    bar_width = (rect.w - 16) / mon->stats.core_count - 4;
    for (core = 0; core < mon->stats.core_count; core++)
    {
        i32 height = rect.h - 24;
        i32 fill = (i32)(height * mon->stats.core_percent[core] / 100);
        gui_rect_t track = gui_rect(x, rect.y + 4, bar_width, height);
        gui_fill_rect(surface, track, AXSM_C_PANEL2);
        gui_fill_rect(surface, gui_rect(x, track.y + height - fill, bar_width, fill),
                      AXSM_C_ACCENT);
        {
            char label[8];
            label[0] = 'C';
            label[1] = ' ';
            label[2] = (char)('0' + core % 10);
            label[3] = 0;
            gui_text(surface, x + 4, rect.y + rect.h - 18, label, AXSM_C_DIM, 1);
        }
        x += bar_width + 4;
    }
}

static void axsyw_paint_donut(gui_surface_t *surface, i32 cx, i32 cy,
                             i32 radius, u32 percent, u32 colour)
{
    i32 i;
    i32 j;
    for (i = -radius; i <= radius; i++)
    {
        for (j = -radius; j <= radius; j++)
        {
            i32 dist2 = i * i + j * j;
            i32 inner = (radius - 8) * (radius - 8);
            i32 outer = radius * radius;
            if (dist2 > outer || dist2 < inner)
                continue;
            {
                i32 angle;
                u32 limit;
                if (j == 0 && i == 0)
                    continue;
                angle = (i32)((i * 1000) / (radius > 0 ? radius : 1));
                limit = percent * 10;
                if ((u32)(angle + 1000) / 2 > limit)
                    continue;
                surface->pixels[(cy + j) * surface->stride + (cx + i)] = colour;
            }
        }
    }
    AXIOM_UNUSED_VAR(AXSM_C_RED);
}

static void axsyw_paint_tabs(gui_surface_t *surface, const axsyw_monitor_t *mon,
                            gui_rect_t strip)
{
    static const char *const labels[4] = { "Processes", "Performance", "Storage",
                                           "Services" };
    i32 x = strip.x + 12;
    u16 i;
    gui_fill_rect(surface, strip, AXSM_C_PANEL);
    for (i = 0; i < 4; i++)
    {
        i32 width = gui_text_width(labels[i], 1) + 28;
        gui_rect_t tab = gui_rect(x, strip.y + 6, width, strip.h - 12);
        if (i == mon->tab)
        {
            gui_fill_round_rect(surface, tab, 4, AXSM_C_SEL);
            gui_fill_rect(surface, gui_rect(x + 10, strip.y + strip.h - 3,
                                            width - 20, 2), AXSM_C_ACCENT);
        }
        gui_text(surface, x + 14, strip.y + 12, labels[i],
                 i == mon->tab ? AXSM_C_TEXT : AXSM_C_DIM, 1);
        x += width + 6;
    }
    gui_hline(surface, strip.x, strip.x + strip.w, strip.y + strip.h - 1,
              AXSM_C_LINE);
}

static void axsyw_paint_processes(gui_surface_t *surface, axsyw_monitor_t *mon,
                                 gui_rect_t area)
{
    gui_rect_t header = gui_rect(area.x, area.y, area.w, AXSM_HEADER);
    u16 visible = (u16)((area.h - AXSM_HEADER) / AXSM_ROW);
    u16 count = 0;
    u16 i;
    i32 y = area.y + AXSM_HEADER;
    char number[24];
    gui_fill_rect(surface, area, AXSM_C_BG);
    gui_fill_rect(surface, header, AXSM_C_PANEL2);
    gui_text(surface, header.x + 12, header.y + 6, "Name", AXSM_C_DIM, 1);
    gui_text(surface, header.x + 200, header.y + 6, "PID", AXSM_C_DIM, 1);
    gui_text(surface, header.x + 264, header.y + 6, "CPU", AXSM_C_DIM, 1);
    gui_text(surface, header.x + 328, header.y + 6, "Memory", AXSM_C_DIM, 1);
    gui_text(surface, header.x + 424, header.y + 6, "State", AXSM_C_DIM, 1);
    gui_text(surface, header.x + 512, header.y + 6, "Threads", AXSM_C_DIM, 1);
    gui_hline(surface, area.x, area.x + area.w, header.y + AXSM_HEADER - 1,
              AXSM_C_LINE);
    for (i = 0; i < mon->process_count; i++)
    {
        axsyw_process_t *p = &mon->processes[i];
        u32 colour;
        i32 index;
        if (!axsyw_matches(mon, p))
            continue;
        if (count < (u16)mon->scroll)
        {
            count++;
            continue;
        }
        index = (i32)(count - (u16)mon->scroll);
        if (index >= (i32)visible)
            break;
        count++;
        colour = (i == mon->selected) ? AXSM_C_TEXT : AXSM_C_TEXT;
        if (i == mon->selected)
            gui_fill_rect(surface, gui_rect(area.x, y, area.w, AXSM_ROW), AXSM_C_SEL);
        gui_fill_rect(surface, gui_rect(area.x + 8, y + 9, 8, 12),
                      axsyw_state_colour(p->state));
        gui_text(surface, area.x + 24, y + 7, p->name, colour, 1);
        axsyw_format_int(number, p->pid);
        gui_text(surface, area.x + 200, y + 7, number, AXSM_C_DIM, 1);
        axsyw_format_int(number, p->cpu_permille);
        gui_text(surface, area.x + 264, y + 7, number, AXSM_C_DIM, 1);
        axsyw_format_int(number, p->memory_kb / 1024);
        gui_text(surface, area.x + 328, y + 7, number, AXSM_C_DIM, 1);
        gui_text(surface, area.x + 340, y + 7, "MB", AXSM_C_DIM, 1);
        gui_text(surface, area.x + 424, y + 7, axsyw_state_label(p->state),
                 axsyw_state_colour(p->state), 1);
        axsyw_format_int(number, p->threads);
        gui_text(surface, area.x + 512, y + 7, number, AXSM_C_DIM, 1);
        gui_hline(surface, area.x, area.x + area.w, y + AXSM_ROW - 1,
                  GUI_RGB(0x27, 0x2A, 0x32));
        y += AXSM_ROW;
    }
    if (count == 0)
        gui_text(surface, area.x + 16, area.y + AXSM_HEADER + 16,
                 "No matching processes", AXSM_C_DIM, 1);
}

static void axsyw_paint_performance(gui_surface_t *surface,
                                   const axsyw_monitor_t *mon, gui_rect_t area)
{
    gui_rect_t cpu_graph = gui_rect(area.x + 16, area.y + 44, area.w - 260, 140);
    gui_rect_t mem_graph = gui_rect(area.x + 16, area.y + 214, area.w - 260, 140);
    char label[48];
    gui_fill_rect(surface, area, AXSM_C_BG);
    gui_text(surface, area.x + 16, area.y + 12, "CPU", AXSM_C_TEXT, 2);
    axsyw_format_int(label, mon->stats.cpu_percent);
    {
        u16 length = (u16)axsyw_format_int(label, mon->stats.cpu_percent);
        label[length] = '%';
        label[length + 1] = 0;
    }
    gui_text(surface, area.x + 64, area.y + 16, label, AXSM_C_TEXT, 1);
    axsyw_paint_graph(surface, cpu_graph, mon->stats.cpu_history[0], 100,
                     AXSM_C_ACCENT);
    gui_text(surface, area.x + 16, area.y + 186, "Memory", AXSM_C_TEXT, 2);
    axsyw_format_int(label, mon->stats.memory_used_mb);
    {
        u16 length = (u16)axsyw_format_int(label, mon->stats.memory_used_mb);
        label[length] = ' ';
        label[length + 1] = 'M';
        label[length + 2] = 'B';
        label[length + 3] = ' ';
        label[length + 4] = '/';
        label[length + 5] = ' ';
        label[length + 6] = 0;
    }
    gui_text(surface, area.x + 92, area.y + 190, label, AXSM_C_DIM, 1);
    axsyw_paint_graph(surface, mem_graph, mon->stats.memory_history,
                     mon->stats.memory_total_mb > 0 ? mon->stats.memory_total_mb : 1,
                     AXSM_C_GREEN);
    axsyw_paint_core_bars(surface, mon,
                         gui_rect(area.x + area.w - 220, area.y + 44, 200, 300));
    {
        i32 cx = area.x + area.w - 120;
        i32 cy = area.y + 380;
        axsyw_paint_donut(surface, cx, cy, 52, mon->stats.memory_pressure,
                         AXSM_C_GREEN);
        axsyw_format_int(label, mon->stats.memory_pressure);
        {
            u16 length = (u16)axsyw_format_int(label, mon->stats.memory_pressure);
            label[length] = '%';
            label[length + 1] = 0;
        }
        gui_text(surface, cx - 12, cy - 6, label, AXSM_C_TEXT, 1);
        gui_text(surface, cx - 30, cy + 40, "memory load", AXSM_C_DIM, 1);
    }
    gui_text(surface, area.x + 16, area.y + 370, "Interrupts", AXSM_C_DIM, 1);
    axsyw_format_int(label, mon->stats.interrupts);
    gui_text(surface, area.x + 120, area.y + 370, label, AXSM_C_TEXT, 1);
    gui_text(surface, area.x + 16, area.y + 390, "Context switches", AXSM_C_DIM, 1);
    axsyw_format_int(label, mon->stats.context_switches);
    gui_text(surface, area.x + 160, area.y + 390, label, AXSM_C_TEXT, 1);
    gui_text(surface, area.x + 16, area.y + 410, "Syscalls", AXSM_C_DIM, 1);
    axsyw_format_int(label, mon->stats.syscalls);
    gui_text(surface, area.x + 100, area.y + 410, label, AXSM_C_TEXT, 1);
}

static void axsyw_paint_storage(gui_surface_t *surface, const axsyw_monitor_t *mon,
                               gui_rect_t area)
{
    u16 i;
    i32 y = area.y + 16;
    char number[24];
    gui_fill_rect(surface, area, AXSM_C_BG);
    gui_text(surface, area.x + 16, y, "Storage", AXSM_C_TEXT, 2);
    y += 32;
    for (i = 0; i < mon->disk_count; i++)
    {
        const axsyw_disk_t *disk = &mon->disks[i];
        i32 percent = (i32)(disk->used_mb * 100 /
                            (disk->total_mb > 0 ? disk->total_mb : 1));
        gui_rect_t bar = gui_rect(area.x + 16, y + 24, area.w - 240, 14);
        gui_text(surface, area.x + 16, y, disk->label, AXSM_C_TEXT, 1);
        gui_text(surface, area.x + 100, y, disk->mount, AXSM_C_DIM, 1);
        gui_fill_rect(surface, bar, AXSM_C_PANEL2);
        gui_fill_round_rect(surface,
                            gui_rect(bar.x, bar.y, bar.w * percent / 100, bar.h),
                            4, percent > 90 ? AXSM_C_RED : AXSM_C_ACCENT);
        axsyw_format_int(number, disk->used_mb);
        gui_text(surface, area.x + 16, y + 44, number, AXSM_C_DIM, 1);
        gui_text(surface, area.x + 16 + 60, y + 44, "MB used of", AXSM_C_DIM, 1);
        axsyw_format_int(number, disk->total_mb);
        gui_text(surface, area.x + 148, y + 44, number, AXSM_C_DIM, 1);
        gui_text(surface, area.x + 148 + 60, y + 44, "MB", AXSM_C_DIM, 1);
        axsyw_format_int(number, disk->read_kbps);
        gui_text(surface, area.x + area.w - 200, y, number, AXSM_C_GREEN, 1);
        gui_text(surface, area.x + area.w - 140, y, "KB/s read", AXSM_C_DIM, 1);
        axsyw_format_int(number, disk->write_kbps);
        gui_text(surface, area.x + area.w - 200, y + 20, number, AXSM_C_YELLOW, 1);
        gui_text(surface, area.x + area.w - 140, y + 20, "KB/s write", AXSM_C_DIM, 1);
        y += 76;
    }
    if (mon->disk_count == 0)
        gui_text(surface, area.x + 16, y, "No volumes mounted", AXSM_C_DIM, 1);
}

static void axsyw_paint_services(gui_surface_t *surface,
                                const axsyw_monitor_t *mon, gui_rect_t area)
{
    u16 i;
    i32 y = area.y + 16;
    char number[24];
    gui_fill_rect(surface, area, AXSM_C_BG);
    gui_text(surface, area.x + 16, y, "Services", AXSM_C_TEXT, 2);
    y += 34;
    for (i = 0; i < mon->service_count; i++)
    {
        const axsyw_service_t *s = &mon->services[i];
        gui_fill_round_rect(surface, gui_rect(area.x + 16, y, area.w - 32, 34), 6,
                            AXSM_C_PANEL);
        gui_fill_rect(surface, gui_rect(area.x + 28, y + 11, 8, 8),
                      s->state == 0 ? AXSM_C_GREEN : AXSM_C_RED);
        gui_text(surface, area.x + 46, y + 8, s->name, AXSM_C_TEXT, 1);
        gui_text(surface, area.x + area.w - 200, y + 8,
                 s->state == 0 ? "running" : "stopped",
                 s->state == 0 ? AXSM_C_GREEN : AXSM_C_RED, 1);
        axsyw_format_int(number, s->restarts);
        gui_text(surface, area.x + area.w - 90, y + 8, number, AXSM_C_DIM, 1);
        gui_text(surface, area.x + area.w - 60, y + 8, "restarts", AXSM_C_DIM, 1);
        y += 40;
    }
    if (mon->service_count == 0)
        gui_text(surface, area.x + 16, y, "No services registered", AXSM_C_DIM, 1);
}

static void axsyw_paint_status(gui_surface_t *surface, const axsyw_monitor_t *mon,
                              gui_rect_t bar)
{
    char number[24];
    gui_fill_rect(surface, bar, AXSM_C_PANEL);
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y, AXSM_C_LINE);
    axsyw_format_int(number, mon->process_count);
    gui_text(surface, bar.x + 12, bar.y + 5, number, AXSM_C_DIM, 1);
    gui_text(surface, bar.x + 40, bar.y + 5, "processes", AXSM_C_DIM, 1);
    axsyw_format_int(number, mon->stats.uptime_seconds);
    gui_text(surface, bar.x + 140, bar.y + 5, number, AXSM_C_DIM, 1);
    gui_text(surface, bar.x + 168, bar.y + 5, "s uptime", AXSM_C_DIM, 1);
    gui_text(surface, bar.x + bar.w - 160, bar.y + 5, mon->search, AXSM_C_DIM, 1);
}

/* -------------------------------------------------------------- public API */

void axsyw_init(axsyw_monitor_t *mon)
{
    axsyw_zero(mon, (u32)sizeof(*mon));
    mon->stats.core_count = 4;
    mon->stats.memory_total_mb = 8192;
    mon->stats.memory_used_mb = 1024;
    mon->sort_column = 2;
    mon->sort_ascending = false;
    axsyw_add_process(mon, 1, "init", 1, 4, 0);
    axsyw_add_process(mon, 2, "kernel", 6, 32, 0);
    axsyw_add_process(mon, 8, "aurora-shell", 12, 96, 0);
    axsyw_add_process(mon, 12, "terminal", 3, 18, 1);
    axsyw_add_process(mon, 16, "filemanager", 2, 24, 1);
    axsyw_add_process(mon, 21, "browser", 9, 220, 0);
    axsyw_add_process(mon, 27, "sysmon", 4, 60, 0);
    axsyw_add_process(mon, 33, "hvci-agent", 2, 12, 1);
    axsyw_add_process(mon, 41, "ebpf-loader", 1, 9, 1);
    axsyw_add_process(mon, 57, "audio-mixer", 5, 40, 2);
    axsyw_add_process(mon, 63, "orphan", 0, 0, 3);
    axsyw_add_disk(mon, "NVMe 0", "/", 512000, 186000);
    axsyw_add_disk(mon, "NVMe 1", "/home", 256000, 61000);
    axsyw_add_disk(mon, "USB", "/media", 32000, 12000);
    axsyw_add_service(mon, "hvci-policy", 0, 0);
    axsyw_add_service(mon, "acpi-daemon", 0, 1);
    axsyw_add_service(mon, "ebpf-tracer", 1, 3);
    axsyw_add_service(mon, "input-router", 0, 0);
    axsyw_add_service(mon, "compositor", 0, 0);
    axsyw_sample(mon, 24, 1024);
    axsyw_sort(mon);
}

void axsyw_tick(axsyw_monitor_t *mon)
{
    mon->tick++;
    if (mon->tick % 20 != 0)
        return;
    {
        u32 cpu = 18 + (mon->tick % 37);
        u32 memory = 1024 + (mon->tick % 512);
        axsyw_sample(mon, cpu, memory);
        axsyw_sort(mon);
    }
}

void axsyw_set_tab(axsyw_monitor_t *mon, u8 tab)
{
    if (tab > 3)
        tab = 3;
    mon->tab = tab;
    mon->scroll = 0;
}

void axsyw_select_row(axsyw_monitor_t *mon, i32 y, i32 area_y)
{
    i32 index = (y - area_y - AXSM_HEADER) / AXSM_ROW + mon->scroll;
    if (index < 0)
        index = 0;
    if (index >= (i32)mon->process_count)
        index = mon->process_count == 0 ? 0 : (i32)mon->process_count - 1;
    mon->selected = (u16)index;
}

void axsyw_scroll(axsyw_monitor_t *mon, i32 delta)
{
    mon->scroll += delta;
    if (mon->scroll < 0)
        mon->scroll = 0;
    if (mon->scroll >= (i32)mon->process_count)
        mon->scroll = mon->process_count == 0 ? 0 : (i32)mon->process_count - 1;
}

void axsyw_sort_by(axsyw_monitor_t *mon, u16 column)
{
    if (mon->sort_column == column)
        mon->sort_ascending = (bool_t)!mon->sort_ascending;
    else
    {
        mon->sort_column = column;
        mon->sort_ascending = false;
    }
    axsyw_sort(mon);
}

void axsyw_search_type(axsyw_monitor_t *mon, char c)
{
    if (mon->search_length + 1 >= 24)
        return;
    mon->search[mon->search_length] = c;
    mon->search_length++;
    mon->search[mon->search_length] = 0;
}

void axsyw_search_backspace(axsyw_monitor_t *mon)
{
    if (mon->search_length == 0)
        return;
    mon->search_length--;
    mon->search[mon->search_length] = 0;
}

void axsyw_kill_selected(axsyw_monitor_t *mon)
{
    u16 i;
    if (mon->selected >= mon->process_count)
        return;
    for (i = mon->selected; i + 1 < mon->process_count; i++)
        mon->processes[i] = mon->processes[i + 1];
    mon->process_count--;
    if (mon->selected >= mon->process_count)
        mon->selected = mon->process_count == 0 ? 0 : (u16)(mon->process_count - 1);
}

void axsyw_paint(axsyw_monitor_t *mon, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t strip = gui_rect(bounds.x, bounds.y, bounds.w, AXSM_TABSTRIP);
    gui_rect_t area = gui_rect(bounds.x, strip.y + strip.h, bounds.w,
                               bounds.h - AXSM_TABSTRIP - AXSM_STATUSBAR);
    gui_rect_t status = gui_rect(bounds.x, bounds.y + bounds.h - AXSM_STATUSBAR,
                                 bounds.w, AXSM_STATUSBAR);
    gui_fill_rect(surface, bounds, AXSM_C_BG);
    axsyw_paint_tabs(surface, mon, strip);
    switch (mon->tab)
    {
    case 0:
        axsyw_paint_processes(surface, mon, area);
        break;
    case 1:
        axsyw_paint_performance(surface, mon, area);
        break;
    case 2:
        axsyw_paint_storage(surface, mon, area);
        break;
    default:
        axsyw_paint_services(surface, mon, area);
        break;
    }
    axsyw_paint_status(surface, mon, status);
}

void axsyw_dump(const axsyw_monitor_t *mon)
{
    printk("axsm: %u processes, cpu=%u%%, mem=%u/%u MB, tab=%u\n",
           (unsigned)mon->process_count, (unsigned)mon->stats.cpu_percent,
           (unsigned)mon->stats.memory_used_mb,
           (unsigned)mon->stats.memory_total_mb, (unsigned)mon->tab);
}

bool_t axsyw_gui_selftest(void)
{
    axsyw_monitor_t mon;
    axsyw_init(&mon);
    if (mon.process_count != 11)
        return false;
    if (mon.disk_count != 3)
        return false;
    if (mon.service_count != 5)
        return false;
    axsyw_sort_by(&mon, 2);
    if (mon.sort_column != 2)
        return false;
    if (mon.processes[0].cpu_permille < mon.processes[1].cpu_permille)
        return false;
    axsyw_search_type(&mon, 'b');
    if (mon.search_length != 1)
        return false;
    axsyw_search_backspace(&mon);
    if (mon.search_length != 0)
        return false;
    axsyw_set_tab(&mon, 2);
    if (mon.tab != 2)
        return false;
    axsyw_kill_selected(&mon);
    if (mon.process_count != 10)
        return false;
    axsyw_tick(&mon);
    return true;
}
