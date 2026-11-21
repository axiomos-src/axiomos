/* AxiomOS UEFI loader - boot menu.
 *
 * Firmware hands control to us with the display already in a usable GOP mode.
 * Before jumping to the kernel we show a short menu: boot normally, boot into
 * safe mode, or drop to the loader shell.  The menu has a countdown so an
 * unattended machine always proceeds; pressing a key cancels it.
 *
 * The module is deliberately free of input plumbing: main.c polls the
 * firmware keyboard and calls axbm_key(); everything else lives here.
 */

#include "loader.h"

#define AXBM_MAX_ENTRIES   4
#define AXBM_TITLE_LEN     56
#define AXBM_DEFAULT_INDEX 0
#define AXBM_TIMEOUT_TICKS 10

enum axbm_mode
{
    AXBM_MODE_NORMAL = 0,
    AXBM_MODE_SAFE,
    AXBM_MODE_SHELL,
    AXBM_MODE_FIRMWARE
};

typedef struct axbm_entry
{
    char title[AXBM_TITLE_LEN];
    char hint[AXBM_TITLE_LEN];
    u8   mode;
    int enabled;
} axbm_entry_t;

typedef struct axbm_state
{
    axbm_entry_t entries[AXBM_MAX_ENTRIES];
    u8           count;
    u8           selected;
    u8           previous;
    i32          countdown;
    int       armed;
    int       cancelled;
    int       confirmed;
    int       dirty;
    u32          draw_count;
    u32          keys;
} axbm_state_t;

static void axbm_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axbm_paint(struct gop_state *gop, const axbm_state_t *menu)
{
    u32 width = gop->width;
    u32 height = gop->height;
    u32 i;
    u32 panel_w = width > 720 ? 640u : width - 40;
    u32 panel_h = 120u + (u32)menu->count * 56u;
    u32 panel_x = (width - panel_w) / 2;
    u32 panel_y = (height - panel_h) / 2;

    gop_fill(gop, 0x00101418);
    gop_rect(gop, panel_x + 6, panel_y + 6, panel_w, panel_h, 0x00080A0E);
    gop_rect(gop, panel_x, panel_y, panel_w, panel_h, 0x00181C22);

    con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                    panel_x + 28, panel_y + 24, "AxiomOS boot menu", 0x00E8ECF5, 2);
    con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                    panel_x + 28, panel_y + 56, "boot / safe / shell", 0x00848997, 1);

    for (i = 0; i < menu->count; i++)
    {
        const axbm_entry_t *entry = &menu->entries[i];
        u32 row_y = panel_y + 88 + i * 56;
        u32 row_colour = i == menu->selected ? 0x002E5A8C : 0x0020242C;
        u32 text_colour = entry->enabled ? 0x00DCE0E8u : 0x00687080u;
        gop_rect(gop, panel_x + 20, row_y, panel_w - 40, 44, row_colour);
        con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                        panel_x + 36, row_y + 8, entry->title, text_colour, 1);
        con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                        panel_x + 36, row_y + 24, entry->hint, 0x00787E8Cu, 1);
        if (i == menu->selected)
        {
            con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                            panel_x + panel_w - 44, row_y + 14, ">",
                            0x0059A8FFu, 1);
        }
    }

    if (menu->armed && !menu->cancelled && menu->countdown > 0)
    {
        char digits[8];
        u_to_dec((u64)menu->countdown, digits);
        con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                        panel_x + 28, panel_y + panel_h - 28, "auto boot in ",
                        0x00848997u, 1);
        con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                        panel_x + 132, panel_y + panel_h - 28, digits,
                        0x00E8ECF5u, 1);
        con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                        panel_x + 148, panel_y + panel_h - 28, "s (up/down/enter)",
                        0x00687080u, 1);
    }
    else
    {
        con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                        panel_x + 28, panel_y + panel_h - 28,
                        "up/down select  enter boot  esc shell", 0x00848997u, 1);
    }
}

/* -------------------------------------------------------------- public API */

void axbm_init(axbm_state_t *menu)
{
    axbm_zero(menu, (u32)sizeof(*menu));
    menu->count = 0;
    menu->selected = AXBM_DEFAULT_INDEX;
    menu->previous = AXBM_DEFAULT_INDEX;
    menu->countdown = AXBM_TIMEOUT_TICKS;
    menu->armed = 1;
    menu->dirty = 1;
}

int axbm_add(axbm_state_t *menu, const char *title, const char *hint, u8 mode,
                int enabled)
{
    axbm_entry_t *entry;
    if (menu->count >= AXBM_MAX_ENTRIES)
        return 0;
    entry = &menu->entries[menu->count];
    u_strcpy(entry->title, title, AXBM_TITLE_LEN);
    u_strcpy(entry->hint, hint, AXBM_TITLE_LEN);
    entry->mode = mode;
    entry->enabled = enabled;
    menu->count++;
    menu->dirty = 1;
    return 1;
}

void axbm_select(axbm_state_t *menu, u8 index)
{
    if (index >= menu->count)
        return;
    if (!menu->entries[index].enabled)
        return;
    menu->previous = menu->selected;
    menu->selected = index;
    menu->dirty = 1;
}

void axbm_move(axbm_state_t *menu, i32 delta)
{
    i32 index = (i32)menu->selected;
    i32 guard = 0;
    if (menu->count == 0)
        return;
    do
    {
        index += delta;
        if (index < 0)
            index = (i32)menu->count - 1;
        if (index >= (i32)menu->count)
            index = 0;
        guard++;
    } while (!menu->entries[index].enabled && guard < (i32)menu->count);
    axbm_select(menu, (u8)index);
    menu->cancelled = 1;
}

u8 axbm_key(axbm_state_t *menu, u16 key)
{
    menu->keys++;
    switch (key)
    {
    case 0x01: /* up */
        axbm_move(menu, -1);
        return 0xFF;
    case 0x02: /* down */
        axbm_move(menu, 1);
        return 0xFF;
    case 0x0D: /* enter */
        menu->confirmed = 1;
        menu->armed = 0;
        return menu->entries[menu->selected].mode;
    case 0x1B: /* escape */
        menu->confirmed = 1;
        menu->armed = 0;
        menu->selected = AXBM_MODE_SHELL;
        return AXBM_MODE_SHELL;
    default:
        menu->cancelled = 1;
        return 0xFF;
    }
}

u8 axbm_tick(axbm_state_t *menu)
{
    if (!menu->armed || menu->cancelled || menu->confirmed)
        return 0xFF;
    if (menu->countdown > 0)
        menu->countdown--;
    if (menu->countdown == 0)
    {
        menu->confirmed = 1;
        menu->armed = 0;
        return menu->entries[menu->selected].mode;
    }
    return 0xFF;
}

u8 axbm_current(const axbm_state_t *menu)
{
    if (menu->count == 0)
        return AXBM_MODE_NORMAL;
    return menu->entries[menu->selected].mode;
}

void axbm_draw(struct gop_state *gop, axbm_state_t *menu)
{
    if (gop == 0 || gop->fb_base == 0)
        return;
    axbm_paint(gop, menu);
    menu->draw_count++;
    menu->dirty = 0;
}

int axbm_done(const axbm_state_t *menu)
{
    return menu->confirmed && !menu->armed;
}

int axbm_selftest(void)
{
    axbm_state_t menu;
    axbm_init(&menu);
    if (menu.count != 0)
        return 0;
    if (!axbm_add(&menu, "AxiomOS", "normal boot", AXBM_MODE_NORMAL, 1))
        return 0;
    if (!axbm_add(&menu, "AxiomOS safe", "no drivers", AXBM_MODE_SAFE, 1))
        return 0;
    if (menu.count != 2)
        return 0;
    axbm_move(&menu, 1);
    if (menu.selected != 1)
        return 0;
    axbm_move(&menu, 1);
    if (menu.selected != 0)
        return 0;
    if (axbm_key(&menu, 0x0D) != AXBM_MODE_NORMAL)
        return 0;
    if (!axbm_done(&menu))
        return 0;
    axbm_init(&menu);
    axbm_add(&menu, "A", "a", AXBM_MODE_NORMAL, 1);
    menu.countdown = 2;
    if (axbm_tick(&menu) != 0xFF)
        return 0;
    if (axbm_tick(&menu) != AXBM_MODE_NORMAL)
        return 0;
    return 1;
}


