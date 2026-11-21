/* AxiomOS - Aurora: application store.
 *
 * A catalogue browser with categories, search, a detail pane and an install
 * queue.  Installation is simulated as a deterministic progress ramp so that
 * the UI can be exercised without a package manager; the state machine is the
 * same one a real backend would drive.
 */

#include "gui.h"

#define AXST_MAX_APPS       48
#define AXST_MAX_CATEGORIES 6
#define AXST_MAX_QUEUE      8
#define AXST_CARD_WIDTH     220
#define AXST_CARD_HEIGHT    148
#define AXST_RAIL_WIDTH     180
#define AXST_DETAIL_WIDTH   320

#define AXST_C_BG      GUI_RGB(0x1C, 0x1E, 0x24)
#define AXST_C_RAIL    GUI_RGB(0x24, 0x27, 0x2E)
#define AXST_C_CARD    GUI_RGB(0x2A, 0x2D, 0x36)
#define AXST_C_CARD_H  GUI_RGB(0x32, 0x36, 0x40)
#define AXST_C_TEXT    GUI_RGB(0xD8, 0xDC, 0xE6)
#define AXST_C_DIM     GUI_RGB(0x8A, 0x90, 0x9E)
#define AXST_C_ACCENT  GUI_RGB(0x59, 0xA8, 0xFF)
#define AXST_C_GREEN   GUI_RGB(0x5C, 0xC0, 0x86)
#define AXST_C_STAR    GUI_RGB(0xE0, 0xB8, 0x58)
#define AXST_C_LINE    GUI_RGB(0x36, 0x3A, 0x44)

enum axst_install_state
{
    AXST_STATE_AVAILABLE = 0,
    AXST_STATE_QUEUED,
    AXST_STATE_INSTALLING,
    AXST_STATE_INSTALLED,
    AXST_STATE_UPDATE
};

typedef struct axst_app
{
    char name[28];
    char vendor[24];
    char summary[72];
    char category[16];
    u32  downloads;
    u8   rating;        /* 0..5 */
    u32  size_kb;
    u8   state;
    u8   progress;
    bool_t featured;
    u32  accent;
} axst_app_t;

typedef struct axst_store
{
    axst_app_t apps[AXST_MAX_APPS];
    u16        app_count;
    u16        category;
    u16        selected;
    u8         queue[AXST_MAX_QUEUE];
    u8         queue_count;
    char       search[28];
    u16        search_length;
    i32        scroll;
    i32        rail_scroll;
    u32        installs;
    u32        updates;
    u32        tick;
} axst_store_t;

static void axst_zero(void *ptr, u32 size);
static void axst_copy(char *dst, u16 capacity, const char *src);
static void axst_add_app(axst_store_t *store, const char *name, const char *vendor,
                         const char *summary, const char *category, u32 downloads,
                         u8 rating, u32 size_kb, u32 accent, bool_t featured);
static bool_t axst_matches(const axst_store_t *store, const axst_app_t *app);
static u16  axst_visible(const axst_store_t *store, u8 *indices, u16 capacity);
static void axst_queue_open(axst_store_t *store, u16 index);
static void axst_step_queue(axst_store_t *store);
static void axst_paint_rail(gui_surface_t *surface, const axst_store_t *store,
                            gui_rect_t rail);
static void axst_paint_cards(gui_surface_t *surface, axst_store_t *store,
                             gui_rect_t area);
static void axst_paint_card(gui_surface_t *surface, const axst_store_t *store,
                            const axst_app_t *app, gui_rect_t card,
                            bool_t selected, bool_t hovered);
static void axst_paint_detail(gui_surface_t *surface, const axst_store_t *store,
                              gui_rect_t detail);
static void axst_paint_queue_strip(gui_surface_t *surface,
                                   const axst_store_t *store, gui_rect_t bar);
static void axst_paint_stars(gui_surface_t *surface, i32 x, i32 y, u8 rating);
static const char *axst_state_label(u8 state);
static u32  axst_state_colour(u8 state);
static void axst_format_size(char *out, u32 kb);

static void axst_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axst_copy(char *dst, u16 capacity, const char *src)
{
    u16 i = 0;
    while (src[i] != 0 && i + 1 < capacity)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void axst_add_app(axst_store_t *store, const char *name, const char *vendor,
                         const char *summary, const char *category, u32 downloads,
                         u8 rating, u32 size_kb, u32 accent, bool_t featured)
{
    axst_app_t *app;
    if (store->app_count >= AXST_MAX_APPS)
        return;
    app = &store->apps[store->app_count];
    axst_copy(app->name, 28, name);
    axst_copy(app->vendor, 24, vendor);
    axst_copy(app->summary, 72, summary);
    axst_copy(app->category, 16, category);
    app->downloads = downloads;
    app->rating = rating;
    app->size_kb = size_kb;
    app->accent = accent;
    app->state = AXST_STATE_AVAILABLE;
    app->progress = 0;
    app->featured = featured;
    store->app_count++;
}

static bool_t axst_matches(const axst_store_t *store, const axst_app_t *app)
{
    u16 i;
    u16 j;
    if (app->category[0] == 0)
        return false;
    if (store->category != 0)
    {
        static const char *const categories[AXST_MAX_CATEGORIES] = {
            "", "Productivity", "Developer", "Media", "System", "Games"
        };
        u16 k = 0;
        const char *want = categories[store->category < AXST_MAX_CATEGORIES
                                          ? store->category : 1];
        while (want[k] != 0 && app->category[k] == want[k])
            k++;
        if (want[k] != 0 || app->category[k] != 0)
            return false;
    }
    if (store->search_length == 0)
        return true;
    for (i = 0; i <= 28 - store->search_length; i++)
    {
        bool_t hit = true;
        if (i > 0 && app->name[i - 1] == 0)
            break;
        for (j = 0; j < store->search_length; j++)
        {
            char a = app->name[i + j];
            char b = store->search[j];
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

static u16 axst_visible(const axst_store_t *store, u8 *indices, u16 capacity)
{
    u16 i;
    u16 count = 0;
    for (i = 0; i < store->app_count && count < capacity; i++)
    {
        if (axst_matches(store, &store->apps[i]))
        {
            indices[count] = (u8)i;
            count++;
        }
    }
    return count;
}

static void axst_queue_open(axst_store_t *store, u16 index)
{
    axst_app_t *app;
    if (index >= store->app_count)
        return;
    app = &store->apps[index];
    if (app->state == AXST_STATE_INSTALLED)
        return;
    if (app->state == AXST_STATE_QUEUED || app->state == AXST_STATE_INSTALLING)
        return;
    app->state = AXST_STATE_QUEUED;
    app->progress = 0;
    if (store->queue_count < AXST_MAX_QUEUE)
    {
        store->queue[store->queue_count] = (u8)index;
        store->queue_count++;
    }
}

static void axst_step_queue(axst_store_t *store)
{
    u8 i;
    bool_t active = false;
    store->tick++;
    for (i = 0; i < store->queue_count; i++)
    {
        axst_app_t *app = &store->apps[store->queue[i]];
        if (app->state == AXST_STATE_QUEUED)
        {
            if (!active)
            {
                app->state = AXST_STATE_INSTALLING;
                app->progress = 0;
            }
            continue;
        }
        if (app->state == AXST_STATE_INSTALLING)
        {
            active = true;
            if (store->tick % 3 == 0 && app->progress < 100)
            {
                app->progress = (u8)(app->progress + 8);
                if (app->progress > 100)
                    app->progress = 100;
            }
            if (app->progress >= 100)
            {
                app->state = AXST_STATE_INSTALLED;
                store->installs++;
            }
        }
    }
}

static void axst_paint_stars(gui_surface_t *surface, i32 x, i32 y, u8 rating)
{
    u8 i;
    for (i = 0; i < 5; i++)
    {
        gui_fill_rect(surface, gui_rect(x + (i32)i * 10, y, 8, 8),
                      i < rating ? AXST_C_STAR : GUI_RGB(0x40, 0x44, 0x50));
        gui_fill_rect(surface, gui_rect(x + (i32)i * 10 + 2, y - 2, 4, 2),
                      i < rating ? AXST_C_STAR : GUI_RGB(0x40, 0x44, 0x50));
    }
}

static const char *axst_state_label(u8 state)
{
    switch (state)
    {
    case AXST_STATE_QUEUED: return "Queued";
    case AXST_STATE_INSTALLING: return "Installing";
    case AXST_STATE_INSTALLED: return "Open";
    case AXST_STATE_UPDATE: return "Update";
    default: return "Get";
    }
}

static u32 axst_state_colour(u8 state)
{
    switch (state)
    {
    case AXST_STATE_INSTALLED: return AXST_C_GREEN;
    case AXST_STATE_UPDATE: return AXST_C_STAR;
    default: return AXST_C_ACCENT;
    }
}

static void axst_format_size(char *out, u32 kb)
{
    u32 value = kb;
    u16 digits = 1;
    u16 i;
    if (value >= 1024)
    {
        value /= 1024;
        out[0] = (char)('0' + value % 10);
        out[1] = ' ';
        out[2] = 'M';
        out[3] = 'B';
        out[4] = 0;
        if (value >= 10)
        {
            out[0] = (char)('0' + (value / 10) % 10);
            out[1] = (char)('0' + value % 10);
            out[2] = ' ';
            out[3] = 'M';
            out[4] = 'B';
            out[5] = 0;
        }
        return;
    }
    while (value >= 10)
    {
        value /= 10;
        digits++;
    }
    value = kb;
    for (i = 0; i < digits; i++)
    {
        out[digits - i - 1] = (char)('0' + (value % 10));
        value /= 10;
    }
    out[digits] = ' ';
    out[digits + 1] = 'K';
    out[digits + 2] = 'B';
    out[digits + 3] = 0;
}

/* ------------------------------------------------------------- rendering */

static void axst_paint_rail(gui_surface_t *surface, const axst_store_t *store,
                            gui_rect_t rail)
{
    static const char *const categories[AXST_MAX_CATEGORIES] = {
        "All", "Productivity", "Developer", "Media", "System", "Games"
    };
    u16 i;
    i32 y = rail.y + 16;
    gui_fill_rect(surface, rail, AXST_C_RAIL);
    gui_text(surface, rail.x + 16, y, "Categories", AXST_C_TEXT, 2);
    y += 34;
    for (i = 0; i < AXST_MAX_CATEGORIES; i++)
    {
        gui_rect_t row = gui_rect(rail.x + 8, y, rail.w - 16, 30);
        if (i == store->category)
        {
            gui_fill_round_rect(surface, row, 4, AXST_C_CARD_H);
            gui_fill_rect(surface, gui_rect(row.x, row.y + 6, 3, 18), AXST_C_ACCENT);
        }
        gui_text(surface, rail.x + 22, y + 8, categories[i],
                 i == store->category ? AXST_C_TEXT : AXST_C_DIM, 1);
        y += 32;
    }
    y += 16;
    gui_text(surface, rail.x + 16, y, "Library", AXST_C_DIM, 1);
    y += 20;
    {
        char number[16];
        u32 value = store->installs;
        u16 digits = 1;
        u32 tmp = value;
        while (tmp >= 10)
        {
            tmp /= 10;
            digits++;
        }
        for (i = 0; i < (u16)digits; i++)
        {
            number[digits - i - 1] = (char)('0' + (value % 10));
            value /= 10;
        }
        number[digits] = 0;
        gui_text(surface, rail.x + 16, y, number, AXST_C_TEXT, 1);
        gui_text(surface, rail.x + 16 + (i32)digits * 8, y, " installed",
                 AXST_C_DIM, 1);
    }
}

static void axst_paint_card(gui_surface_t *surface, const axst_store_t *store,
                            const axst_app_t *app, gui_rect_t card, bool_t selected,
                            bool_t hovered)
{
    u32 fill = hovered ? AXST_C_CARD_H : AXST_C_CARD;
    char size[16];
    i32 badge_x;
    gui_fill_round_rect(surface, card, 8, fill);
    gui_rect_outline(surface, card,
                     selected ? AXST_C_ACCENT : AXST_C_LINE);
    gui_fill_round_rect(surface, gui_rect(card.x + 16, card.y + 16, 40, 40), 8,
                        app->accent);
    gui_text(surface, card.x + 30, card.y + 28, "A", AXST_C_TEXT, 2);
    gui_text(surface, card.x + 16, card.y + 66, app->name, AXST_C_TEXT, 1);
    gui_text(surface, card.x + 16, card.y + 82, app->vendor, AXST_C_DIM, 1);
    axst_paint_stars(surface, card.x + 16, card.y + 100, app->rating);
    axst_format_size(size, app->size_kb);
    gui_text(surface, card.x + card.w - 56, card.y + 100, size, AXST_C_DIM, 1);
    badge_x = card.x + card.w - 62;
    gui_fill_round_rect(surface, gui_rect(badge_x, card.y + card.h - 30, 46, 22),
                        11, axst_state_colour(app->state));
    gui_text(surface, badge_x + 8, card.y + card.h - 26,
             axst_state_label(app->state), GUI_RGB(0x10, 0x14, 0x1C), 1);
    if (app->state == AXST_STATE_INSTALLING)
    {
        gui_fill_rect(surface, gui_rect(card.x + 16, card.y + card.h - 10,
                                        card.w - 32, 4), AXST_C_LINE);
        gui_fill_rect(surface, gui_rect(card.x + 16, card.y + card.h - 10,
                                        (card.w - 32) * app->progress / 100, 4),
                      AXST_C_GREEN);
    }
    if (app->featured)
        gui_text(surface, card.x + card.w - 68, card.y + 14, "featured",
                 AXST_C_STAR, 1);
}

static void axst_paint_cards(gui_surface_t *surface, axst_store_t *store,
                             gui_rect_t area)
{
    u8 indices[AXST_MAX_APPS];
    u16 count = axst_visible(store, indices, AXST_MAX_APPS);
    i32 columns = (area.w + 16) / (AXST_CARD_WIDTH + 16);
    i32 index = 0;
    i32 start;
    u16 i;
    i32 y;
    i32 x;
    if (columns < 1)
        columns = 1;
    start = store->scroll * columns;
    gui_fill_rect(surface, area, AXST_C_BG);
    gui_text(surface, area.x + 4, area.y + 4, "Axiom store", AXST_C_TEXT, 2);
    y = area.y + 40;
    for (i = (u16)start; i < count; i++)
    {
        i32 slot = index % columns;
        axst_app_t *app = &store->apps[indices[i]];
        gui_rect_t card;
        if (index >= columns * 4)
            break;
        x = area.x + 4 + slot * (AXST_CARD_WIDTH + 16);
        card = gui_rect(x, y, AXST_CARD_WIDTH, AXST_CARD_HEIGHT);
        axst_paint_card(surface, store, app, card, (bool_t)(i == store->selected),
                        false);
        index++;
        if (slot == columns - 1)
            y += AXST_CARD_HEIGHT + 16;
    }
    if (count == 0)
        gui_text(surface, area.x + 8, area.y + 48, "Nothing matches that search",
                 AXST_C_DIM, 1);
}

static void axst_paint_detail(gui_surface_t *surface, const axst_store_t *store,
                              gui_rect_t detail)
{
    const axst_app_t *app;
    char value[16];
    u16 i;
    u16 digits = 1;
    u32 tmp;
    gui_fill_rect(surface, detail, AXST_C_RAIL);
    gui_vline(surface, detail.x, detail.y, detail.y + detail.h, AXST_C_LINE);
    if (store->selected >= store->app_count)
    {
        gui_text(surface, detail.x + 20, detail.y + 20, "Select an app",
                 AXST_C_DIM, 1);
        return;
    }
    app = &store->apps[store->selected];
    gui_fill_round_rect(surface, gui_rect(detail.x + 24, detail.y + 24, 64, 64), 12,
                        app->accent);
    gui_text(surface, detail.x + 48, detail.y + 44, "A", AXST_C_TEXT, 3);
    gui_text(surface, detail.x + 24, detail.y + 104, app->name, AXST_C_TEXT, 2);
    gui_text(surface, detail.x + 24, detail.y + 132, app->vendor, AXST_C_DIM, 1);
    axst_paint_stars(surface, detail.x + 24, detail.y + 156, app->rating);
    gui_text(surface, detail.x + 24, detail.y + 182, app->summary, AXST_C_TEXT, 1);
    gui_text(surface, detail.x + 24, detail.y + 210, "Category", AXST_C_DIM, 1);
    gui_text(surface, detail.x + 120, detail.y + 210, app->category, AXST_C_TEXT, 1);
    gui_text(surface, detail.x + 24, detail.y + 232, "Downloads", AXST_C_DIM, 1);
    tmp = app->downloads;
    while (tmp >= 10)
    {
        tmp /= 10;
        digits++;
    }
    tmp = app->downloads;
    for (i = 0; i < digits; i++)
    {
        value[digits - i - 1] = (char)('0' + (tmp % 10));
        tmp /= 10;
    }
    value[digits] = 0;
    gui_text(surface, detail.x + 120, detail.y + 232, value, AXST_C_TEXT, 1);
    axst_format_size(value, app->size_kb);
    gui_text(surface, detail.x + 24, detail.y + 254, "Size", AXST_C_DIM, 1);
    gui_text(surface, detail.x + 120, detail.y + 254, value, AXST_C_TEXT, 1);
    {
        gui_rect_t button = gui_rect(detail.x + 24, detail.y + detail.h - 60,
                                     detail.w - 48, 36);
        gui_fill_round_rect(surface, button, 18, axst_state_colour(app->state));
        gui_text(surface, button.x + button.w / 2 - 24, button.y + 10,
                 axst_state_label(app->state), GUI_RGB(0x0E, 0x12, 0x1A), 1);
    }
}

static void axst_paint_queue_strip(gui_surface_t *surface,
                                   const axst_store_t *store, gui_rect_t bar)
{
    u8 i;
    i32 x = bar.x + 12;
    gui_fill_rect(surface, bar, GUI_RGB(0x26, 0x29, 0x31));
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y, AXST_C_LINE);
    if (store->queue_count == 0)
    {
        gui_text(surface, x, bar.y + 6, "Install queue empty", AXST_C_DIM, 1);
        return;
    }
    for (i = 0; i < store->queue_count; i++)
    {
        const axst_app_t *app = &store->apps[store->queue[i]];
        i32 width = gui_text_width(app->name, 1) + 56;
        gui_rect_t chip = gui_rect(x, bar.y + 3, width, bar.h - 6);
        gui_fill_round_rect(surface, chip, 4, AXST_C_CARD);
        gui_text(surface, x + 8, bar.y + 7, app->name, AXST_C_TEXT, 1);
        if (app->state == AXST_STATE_INSTALLING)
            gui_fill_rect(surface, gui_rect(x + width - 30, bar.y + 8, 20, 4),
                          AXST_C_GREEN);
        x += width + 6;
    }
}

/* -------------------------------------------------------------- public API */

void axst_init(axst_store_t *store)
{
    axst_zero(store, (u32)sizeof(*store));
    axst_add_app(store, "Axiom Write", "Axiom Labs", "Document editing with styles",
                 "Productivity", 128000, 4, 2400, GUI_RGB(0x2E, 0x6C, 0xC0), true);
    axst_add_app(store, "Axiom Sheet", "Axiom Labs", "Grid computing for people",
                 "Productivity", 96000, 4, 3100, GUI_RGB(0x3E, 0x8E, 0x62), false);
    axst_add_app(store, "Axiom Code", "Axiom Labs", "Editor for kernel work",
                 "Developer", 84000, 5, 5200, GUI_RGB(0x70, 0x48, 0xB0), true);
    axst_add_app(store, "Hexa", "Northwind", "Hex viewer and patcher",
                 "Developer", 42000, 3, 900, GUI_RGB(0x9A, 0x5A, 0x32), false);
    axst_add_app(store, "Waveform", "Audio Works", "Player and recorder",
                 "Media", 210000, 4, 4700, GUI_RGB(0xC0, 0x50, 0x70), true);
    axst_add_app(store, "Lumen", "Pixel Forge", "Image viewer and editor",
                 "Media", 156000, 3, 6100, GUI_RGB(0x40, 0x80, 0xA0), false);
    axst_add_app(store, "Terminal+", "Axiom Labs", "Tabs, splits and themes",
                 "System", 88000, 4, 1200, GUI_RGB(0x22, 0x24, 0x2C), false);
    axst_add_app(store, "Guard", "Shield Team", "Security posture dashboard",
                 "System", 64000, 4, 2800, GUI_RGB(0x40, 0x60, 0x40), true);
    axst_add_app(store, "Vector", "Playfield", "Vector arcade collection",
                 "Games", 33000, 3, 8600, GUI_RGB(0xB0, 0x90, 0x40), false);
    axst_add_app(store, "Orbit", "Playfield", "Physics puzzle sandbox",
                 "Games", 27000, 2, 7400, GUI_RGB(0x50, 0x70, 0xB0), false);
}

void axst_set_category(axst_store_t *store, u16 category)
{
    if (category >= AXST_MAX_CATEGORIES)
        category = 0;
    store->category = category;
    store->scroll = 0;
}

void axst_search_type(axst_store_t *store, char c)
{
    if (store->search_length + 1 >= 28)
        return;
    store->search[store->search_length] = c;
    store->search_length++;
    store->search[store->search_length] = 0;
}

void axst_search_backspace(axst_store_t *store)
{
    if (store->search_length == 0)
        return;
    store->search_length--;
    store->search[store->search_length] = 0;
}

void axst_click(axst_store_t *store, i32 x, i32 y, i32 rail_width)
{
    i32 columns = (x + 0);
    AXIOM_UNUSED_VAR(columns);
    if (x < rail_width)
        return;
    {
        i32 index = store->scroll * 1;
        AXIOM_UNUSED_VAR(index);
    }
    if (x > rail_width + 400 && y > 0)
    {
        axst_queue_open(store, store->selected);
        return;
    }
}

void axst_select_visible(axst_store_t *store, i32 index)
{
    u8 indices[AXST_MAX_APPS];
    u16 count = axst_visible(store, indices, AXST_MAX_APPS);
    if (count == 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= (i32)count)
        index = (i32)count - 1;
    store->selected = indices[index];
}

void axst_install_selected(axst_store_t *store)
{
    axst_queue_open(store, store->selected);
}

void axst_scroll(axst_store_t *store, i32 delta)
{
    store->scroll += delta / 3;
    if (store->scroll < 0)
        store->scroll = 0;
    if (store->scroll > (i32)store->app_count)
        store->scroll = (i32)store->app_count;
}

void axst_tick(axst_store_t *store)
{
    axst_step_queue(store);
}

void axst_paint(axst_store_t *store, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t rail = gui_rect(bounds.x, bounds.y, AXST_RAIL_WIDTH, bounds.h - 34);
    gui_rect_t detail = gui_rect(bounds.x + bounds.w - AXST_DETAIL_WIDTH, bounds.y,
                                 AXST_DETAIL_WIDTH, bounds.h - 34);
    gui_rect_t area = gui_rect(bounds.x + AXST_RAIL_WIDTH, bounds.y,
                               bounds.w - AXST_RAIL_WIDTH - AXST_DETAIL_WIDTH,
                               bounds.h - 34);
    gui_rect_t queue = gui_rect(bounds.x, bounds.y + bounds.h - 34, bounds.w, 34);
    gui_fill_rect(surface, bounds, AXST_C_BG);
    axst_paint_rail(surface, store, rail);
    axst_paint_cards(surface, store, area);
    axst_paint_detail(surface, store, detail);
    axst_paint_queue_strip(surface, store, queue);
}

void axst_dump(const axst_store_t *store)
{
    printk("axst: %u apps, category=%u, installs=%u, queue=%u\n",
           (unsigned)store->app_count, (unsigned)store->category,
           (unsigned)store->installs, (unsigned)store->queue_count);
}

bool_t axst_selftest(void)
{
    axst_store_t store;
    u8 indices[AXST_MAX_APPS];
    u16 count;
    axst_init(&store);
    if (store.app_count != 10)
        return false;
    count = axst_visible(&store, indices, AXST_MAX_APPS);
    if (count != 10)
        return false;
    axst_set_category(&store, 2);
    count = axst_visible(&store, indices, AXST_MAX_APPS);
    if (count != 2)
        return false;
    axst_set_category(&store, 0);
    axst_search_type(&store, 'g');
    count = axst_visible(&store, indices, AXST_MAX_APPS);
    if (count == 0 || count > 10)
        return false;
    axst_search_backspace(&store);
    axst_select_visible(&store, 0);
    axst_install_selected(&store);
    if (store.queue_count == 0)
        return false;
    {
        u16 i;
        for (i = 0; i < 120; i++)
            axst_tick(&store);
    }
    if (store.installs == 0)
        return false;
    return true;
}
