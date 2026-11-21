/*
 * AxiomOS Aurora - first run tips and coach marks.
 *
 * New users get a short, dismissible sequence of hints anchored to shell
 * surfaces (the launcher, the file browser, the terminal).  Tips are ordered,
 * each has an anchor rectangle supplied by the owning app, and the control keeps
 * only one card on screen at a time so it never competes with real content.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>
#include "gui.h"
#include "aurora.h"

#define AXTP_MAX_TIPS        12
#define AXTP_CARD_WIDTH      260
#define AXTP_CARD_PADDING    12
#define AXTP_MAX_VISIBLE     1

#define AXTP_ANCHOR_LAUNCHER 0
#define AXTP_ANCHOR_FILES    1
#define AXTP_ANCHOR_TERMINAL 2
#define AXTP_ANCHOR_DESKTOP  3
#define AXTP_ANCHOR_SETTINGS 4

#define AXTP_STATE_PENDING   0
#define AXTP_STATE_SHOWN     1
#define AXTP_STATE_DISMISSED 2
#define AXTP_STATE_COMPLETE  3

typedef struct axtp_tip
{
    u16  title;
    u16  body;
    gui_rect_t anchor;
    u32  order;
    u32  state;
    u32  shown_ms;
    u8   anchor_kind;
    bool_t pinned;
} axtp_tip_t;

typedef struct axtp_state
{
    axtp_tip_t tips[AXTP_MAX_TIPS];
    u32  tip_count;
    u32  current;
    u32  dismissed;
    u32  completed;
    u32  shown_total;
    u32  auto_dismissed;
    u32  now_ms;
    bool_t active;
    bool_t sequence_done;
} axtp_state_t;

static axtp_state_t axtp_state;

static void axtp_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static const char *axtp_anchor_name(u8 kind)
{
    switch (kind)
    {
    case AXTP_ANCHOR_LAUNCHER: return "Launcher";
    case AXTP_ANCHOR_FILES:    return "Files";
    case AXTP_ANCHOR_TERMINAL: return "Terminal";
    case AXTP_ANCHOR_DESKTOP:  return "Desktop";
    case AXTP_ANCHOR_SETTINGS: return "Settings";
    default:                   return "Shell";
    }
}

static u32 axtp_next_pending(void)
{
    for (u32 i = 0; i < axtp_state.tip_count; i++)
    {
        if (axtp_state.tips[i].state == AXTP_STATE_PENDING)
            return i;
    }
    return 0xFFFFFFFFu;
}

/* -------------------------------------------------------------- public API */

void axtp_init(void)
{
    axtp_zero(&axtp_state, (u32)sizeof(axtp_state));
    axtp_state.current = 0xFFFFFFFFu;
}

bool_t axtp_add(u16 title, u16 body, u8 anchor_kind, gui_rect_t anchor)
{
    if (axtp_state.tip_count >= AXTP_MAX_TIPS)
        return false;
    axtp_tip_t *tip = &axtp_state.tips[axtp_state.tip_count];
    tip->title = title;
    tip->body = body;
    tip->anchor = anchor;
    tip->anchor_kind = anchor_kind;
    tip->order = axtp_state.tip_count;
    tip->state = AXTP_STATE_PENDING;
    tip->pinned = false;
    axtp_state.tip_count++;
    return true;
}

bool_t axtp_start(void)
{
    u32 index = axtp_next_pending();
    if (index == 0xFFFFFFFFu)
        return false;
    axtp_state.tips[index].state = AXTP_STATE_SHOWN;
    axtp_state.current = index;
    axtp_state.active = true;
    axtp_state.shown_total++;
    return true;
}

bool_t axtp_dismiss(void)
{
    if (axtp_state.current >= axtp_state.tip_count)
    {
        axtp_state.active = false;
        return false;
    }
    axtp_state.tips[axtp_state.current].state = AXTP_STATE_DISMISSED;
    axtp_state.dismissed++;
    axtp_state.current = 0xFFFFFFFFu;
    u32 next = axtp_next_pending();
    if (next == 0xFFFFFFFFu)
    {
        axtp_state.active = false;
        axtp_state.sequence_done = true;
        axtp_state.completed++;
        return true;
    }
    axtp_state.tips[next].state = AXTP_STATE_SHOWN;
    axtp_state.current = next;
    axtp_state.shown_total++;
    return true;
}

bool_t axtp_skip_all(void)
{
    u32 skipped = 0;
    for (u32 i = 0; i < axtp_state.tip_count; i++)
    {
        if (axtp_state.tips[i].state == AXTP_STATE_SHOWN ||
            axtp_state.tips[i].state == AXTP_STATE_PENDING)
        {
            axtp_state.tips[i].state = AXTP_STATE_DISMISSED;
            skipped++;
        }
    }
    axtp_state.dismissed += skipped;
    axtp_state.current = 0xFFFFFFFFu;
    axtp_state.active = false;
    axtp_state.sequence_done = true;
    return skipped != 0;
}

gui_rect_t axtp_card_rect(void)
{
    gui_rect_t empty = gui_rect(0, 0, 0, 0);
    if (axtp_state.current >= axtp_state.tip_count)
        return empty;
    const axtp_tip_t *tip = &axtp_state.tips[axtp_state.current];
    i32 x = tip->anchor.x;
    i32 y = tip->anchor.y + tip->anchor.h + 8;
    if (x + AXTP_CARD_WIDTH > 1000)
        x = 1000 - AXTP_CARD_WIDTH;
    return gui_rect(x, y, AXTP_CARD_WIDTH, 96);
}

void axtp_paint(gui_surface_t *surface)
{
    gui_rect_t card;
    u32 surface_fill = GUI_RGB(30, 35, 44);
    u32 stroke = GUI_RGB(60, 68, 84);
    u32 text_colour = GUI_RGB(232, 236, 244);
    u32 muted = GUI_RGB(164, 172, 186);
    u32 accent = GUI_RGB(118, 178, 246);
    if (!axtp_state.active || axtp_state.current >= axtp_state.tip_count)
        return;
    if (surface == NULL_PTR)
        return;
    card = axtp_card_rect();
    gui_shadow(surface, card, 6, 96);
    gui_fill_round_rect(surface, card, AXAU_RADIUS_FLYOUT, surface_fill);
    gui_rect_outline(surface, card, stroke);
    const axtp_tip_t *tip = &axtp_state.tips[axtp_state.current];
    gui_text(surface, card.x + AXTP_CARD_PADDING, card.y + AXTP_CARD_PADDING,
             axtp_anchor_name(tip->anchor_kind), accent, 1);
    gui_text(surface, card.x + AXTP_CARD_PADDING, card.y + AXTP_CARD_PADDING + 20,
             "Tip", text_colour, 2);
    gui_text(surface, card.x + AXTP_CARD_PADDING, card.y + 56, "Press Enter", muted, 1);
    gui_rect_t confirm = gui_rect(card.x + card.w - 96, card.y + card.h - 32, 76, 24);
    gui_fill_round_rect(surface, confirm, AXAU_RADIUS_CONTROL, GUI_RGB(46, 54, 68));
    gui_text(surface, confirm.x + 10, confirm.y + 4, "Got it", text_colour, 1);
    if (axtp_state.tip_count > 1)
    {
        gui_text(surface, card.x + AXTP_CARD_PADDING, card.y + card.h - 24,
                 "1 of 3", muted, 1);
    }
}

u32 axtp_current(void)
{
    return axtp_state.current;
}

bool_t axtp_active(void)
{
    return axtp_state.active;
}

u32 axtp_dismissed_count(void)
{
    return axtp_state.dismissed;
}

u32 axtp_shown_count(void)
{
    return axtp_state.shown_total;
}

bool_t axtp_sequence_done(void)
{
    return axtp_state.sequence_done;
}

void axtp_report(void)
{
    printk("axtp: tips=%u shown=%u dismissed=%u current=%u active=%u done=%u\n",
           (unsigned)axtp_state.tip_count, (unsigned)axtp_state.shown_total,
           (unsigned)axtp_state.dismissed, (unsigned)axtp_state.current,
           (unsigned)axtp_state.active, (unsigned)axtp_state.sequence_done);
}

bool_t axtp_selftest(void)
{
    gui_rect_t card;
    axtp_init();
    if (axtp_active())
        return false;
    if (!axtp_add(0x10, 0x20, AXTP_ANCHOR_LAUNCHER, gui_rect(8, 8, 48, 48)))
        return false;
    if (!axtp_add(0x11, 0x21, AXTP_ANCHOR_FILES, gui_rect(200, 100, 120, 40)))
        return false;
    if (!axtp_add(0x12, 0x22, AXTP_ANCHOR_TERMINAL, gui_rect(400, 200, 200, 60)))
        return false;
    if (axtp_state.tip_count != 3)
        return false;
    if (!axtp_start())
        return false;
    if (!axtp_active())
        return false;
    if (axtp_current() != 0)
        return false;
    card = axtp_card_rect();
    if (card.w != AXTP_CARD_WIDTH)
        return false;
    if (card.y <= 8)
        return false;
    if (!axtp_dismiss())
        return false;
    if (axtp_current() != 1)
        return false;
    if (axtp_dismissed_count() != 1)
        return false;
    if (!axtp_dismiss())
        return false;
    if (axtp_current() != 2)
        return false;
    if (!axtp_dismiss())
        return false;
    if (!axtp_sequence_done())
        return false;
    if (axtp_active())
        return false;
    if (axtp_shown_count() != 3)
        return false;
    if (!axtp_add(0x13, 0x23, AXTP_ANCHOR_DESKTOP, gui_rect(0, 0, 10, 10)))
        return false;
    if (!axtp_skip_all())
        return false;
    if (!axtp_sequence_done())
        return false;
    if (axtp_anchor_name(AXTP_ANCHOR_LAUNCHER)[0] != 'L')
        return false;
    return true;
}
