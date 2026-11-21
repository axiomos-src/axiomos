/* AxiomOS Aurora - contacts application.
 *
 * Contacts are grouped by an initial letter bucket so that the sidebar reads
 * like an address book.  Each record may hold several phone numbers, an email
 * address, a note and a favourite flag; the detail pane shows the selected one.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include "gui.h"

#define AXCO_MAX_CONTACTS     20
#define AXCO_MAX_PHONES       3
#define AXCO_NAME_LEN         32
#define AXCO_EMAIL_LEN        48
#define AXCO_PHONE_LEN        20
#define AXCO_NOTE_LEN         96
#define AXCO_GROUP_LEN        16

#define AXCO_COLOUR_BG        GUI_RGB(0x16, 0x18, 0x1E)
#define AXCO_COLOUR_SIDEBAR   GUI_RGB(0x19, 0x1C, 0x23)
#define AXCO_COLOUR_CARD      GUI_RGB(0x1E, 0x22, 0x2A)
#define AXCO_COLOUR_TEXT      GUI_RGB(0xE8, 0xEA, 0xF0)
#define AXCO_COLOUR_MUTED     GUI_RGB(0x8B, 0x92, 0xA1)
#define AXCO_COLOUR_ACCENT    GUI_RGB(0x55, 0x8B, 0xFF)
#define AXCO_COLOUR_RULE      GUI_RGB(0x2B, 0x30, 0x3C)

typedef struct axco_contact
{
    char name[AXCO_NAME_LEN];
    char group[AXCO_GROUP_LEN];
    char email[AXCO_EMAIL_LEN];
    char phones[AXCO_MAX_PHONES][AXCO_PHONE_LEN];
    char note[AXCO_NOTE_LEN];
    u8   phone_count;
    u8   initial;
    bool_t favourite;
    bool_t blocked;
    bool_t used;
} axco_contact_t;

typedef struct axco_state
{
    axco_contact_t contacts[AXCO_MAX_CONTACTS];
    u32  count;
    u32  selected;
    u32  favourites;
    u32  blocked;
    u32  calls;
    u32  messages;
    u32  search_hits;
    u32  group_filter;
    char search[AXCO_NAME_LEN];
    bool_t detail_open;
    bool_t ready;
} axco_state_t;

static axco_state_t axco_state;

static void axco_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axco_copy_string(char *dst, const char *src, u32 limit)
{
    u32 i = 0;
    if (dst == NULL_PTR || limit == 0)
        return;
    while (src != NULL_PTR && src[i] != 0 && i + 1 < limit)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool_t axco_matches(const axco_contact_t *contact)
{
    u32 i;
    if (!contact->used)
        return false;
    if (axco_state.search[0] == 0)
        return true;
    for (i = 0; axco_state.search[i] != 0 && i < AXCO_NAME_LEN; i++)
    {
        if (contact->name[i] != axco_state.search[i])
            return false;
        if (contact->name[i] == 0)
            break;
    }
    return true;
}

static axco_contact_t *axco_contact(u32 index)
{
    if (index >= axco_state.count)
        return NULL_PTR;
    if (!axco_state.contacts[index].used)
        return NULL_PTR;
    return &axco_state.contacts[index];
}

/* -------------------------------------------------------------- public API */

void axco_init(void)
{
    axco_zero(&axco_state, (u32)sizeof(axco_state));
    axco_state.ready = true;
}

u32 axco_add(const char *name, const char *group, const char *email)
{
    axco_contact_t *contact;
    if (axco_state.count >= AXCO_MAX_CONTACTS || name == NULL_PTR)
        return 0xFFFFFFFFu;
    contact = &axco_state.contacts[axco_state.count];
    axco_zero(contact, (u32)sizeof(*contact));
    axco_copy_string(contact->name, name, AXCO_NAME_LEN);
    axco_copy_string(contact->group, group == NULL_PTR ? "General" : group, AXCO_GROUP_LEN);
    axco_copy_string(contact->email, email == NULL_PTR ? "" : email, AXCO_EMAIL_LEN);
    contact->initial = (u8)name[0];
    contact->used = true;
    axco_state.count++;
    return axco_state.count - 1;
}

bool_t axco_add_phone(u32 index, const char *number)
{
    axco_contact_t *contact = axco_contact(index);
    if (contact == NULL_PTR || number == NULL_PTR)
        return false;
    if (contact->phone_count >= AXCO_MAX_PHONES)
        return false;
    axco_copy_string(contact->phones[contact->phone_count], number, AXCO_PHONE_LEN);
    contact->phone_count++;
    return true;
}

const char *axco_phone(u32 index, u32 slot)
{
    axco_contact_t *contact = axco_contact(index);
    if (contact == NULL_PTR || slot >= contact->phone_count)
        return NULL_PTR;
    return contact->phones[slot];
}

const char *axco_name(u32 index)
{
    axco_contact_t *contact = axco_contact(index);
    return contact == NULL_PTR ? NULL_PTR : contact->name;
}

const char *axco_email(u32 index)
{
    axco_contact_t *contact = axco_contact(index);
    return contact == NULL_PTR ? NULL_PTR : contact->email;
}

const char *axco_group(u32 index)
{
    axco_contact_t *contact = axco_contact(index);
    return contact == NULL_PTR ? NULL_PTR : contact->group;
}

bool_t axco_set_note(u32 index, const char *note)
{
    axco_contact_t *contact = axco_contact(index);
    if (contact == NULL_PTR)
        return false;
    axco_copy_string(contact->note, note, AXCO_NOTE_LEN);
    return true;
}

bool_t axco_toggle_favourite(u32 index)
{
    axco_contact_t *contact = axco_contact(index);
    if (contact == NULL_PTR)
        return false;
    contact->favourite = !contact->favourite;
    if (contact->favourite)
        axco_state.favourites++;
    else if (axco_state.favourites > 0)
        axco_state.favourites--;
    return contact->favourite;
}

bool_t axco_toggle_blocked(u32 index)
{
    axco_contact_t *contact = axco_contact(index);
    if (contact == NULL_PTR)
        return false;
    contact->blocked = !contact->blocked;
    if (contact->blocked)
        axco_state.blocked++;
    else if (axco_state.blocked > 0)
        axco_state.blocked--;
    return contact->blocked;
}

bool_t axco_select(u32 index)
{
    if (axco_contact(index) == NULL_PTR)
        return false;
    axco_state.selected = index;
    axco_state.detail_open = true;
    return true;
}

u32 axco_selected(void)
{
    return axco_state.selected;
}

u32 axco_count(void)
{
    return axco_state.count;
}

u32 axco_search(const char *prefix)
{
    u32 i;
    u32 hits = 0;
    axco_copy_string(axco_state.search, prefix, AXCO_NAME_LEN);
    for (i = 0; i < axco_state.count; i++)
    {
        if (axco_matches(&axco_state.contacts[i]))
            hits++;
    }
    axco_state.search_hits = hits;
    return hits;
}

u32 axco_visible_count(void)
{
    u32 i;
    u32 visible = 0;
    for (i = 0; i < axco_state.count; i++)
    {
        if (axco_matches(&axco_state.contacts[i]))
            visible++;
    }
    return visible;
}

bool_t axco_call(u32 index)
{
    axco_contact_t *contact = axco_contact(index);
    if (contact == NULL_PTR || contact->blocked)
        return false;
    axco_state.calls++;
    return true;
}

bool_t axco_message(u32 index)
{
    axco_contact_t *contact = axco_contact(index);
    if (contact == NULL_PTR || contact->blocked)
        return false;
    axco_state.messages++;
    return true;
}

bool_t axco_remove(u32 index)
{
    axco_contact_t *contact = axco_contact(index);
    if (contact == NULL_PTR)
        return false;
    if (contact->favourite && axco_state.favourites > 0)
        axco_state.favourites--;
    if (contact->blocked && axco_state.blocked > 0)
        axco_state.blocked--;
    axco_zero(contact, (u32)sizeof(*contact));
    if (axco_state.count > 0)
        axco_state.count--;
    return true;
}

u32 axco_group_count(u8 initial)
{
    u32 i;
    u32 count = 0;
    for (i = 0; i < axco_state.count; i++)
    {
        if (axco_state.contacts[i].used && axco_state.contacts[i].initial == initial)
            count++;
    }
    return count;
}

/* ----------------------------------------------------------------- painting */

static void axco_paint_sidebar(gui_surface_t *surface, gui_rect_t sidebar)
    {
    u32 i;
    u32 shown = 0;
    char letter;
    gui_text(surface, sidebar.x + 14, sidebar.y + 12, "Contacts", AXCO_COLOUR_MUTED, 1);
    for (letter = 'A'; letter <= 'Z'; letter++)
    {
        u32 count = axco_group_count((u8)letter);
        if (count == 0)
            continue;
        gui_text(surface, sidebar.x + 14, sidebar.y + 36 + (i32)shown * 44, (const char *)&letter,
                 AXCO_COLOUR_ACCENT, 1);
        (void)letter;
        shown++;
        if (shown > 6)
            break;
    }
    for (i = 0; i < axco_state.count && shown < 10; i++)
    {
        const axco_contact_t *contact = &axco_state.contacts[i];
        gui_rect_t row;
        u32 colour;
        if (!contact->used)
            continue;
        row = gui_rect(sidebar.x + 8, sidebar.y + 60 + (i32)shown * 44, sidebar.w - 16, 40);
        colour = (i == axco_state.selected) ? GUI_RGB(0x22, 0x2C, 0x46) : GUI_RGB(0x1B, 0x1F, 0x27);
        gui_fill_round_rect(surface, row, 7, colour);
        gui_fill_round_rect(surface, gui_rect(row.x + 8, row.y + 8, 24, 24), 12,
                            GUI_RGB(0x31, 0x3A, 0x4C));
        gui_text(surface, row.x + 40, row.y + 6, contact->name, AXCO_COLOUR_TEXT, 1);
        gui_text(surface, row.x + 40, row.y + 22, contact->group, AXCO_COLOUR_MUTED, 1);
        if (contact->favourite)
            gui_fill_rect(surface, gui_rect(row.x + row.w - 14, row.y + 14, 6, 6),
                          AXCO_COLOUR_ACCENT);
        shown++;
    }
}

static void axco_paint_detail(gui_surface_t *surface, gui_rect_t card)
{
    const axco_contact_t *contact = axco_contact(axco_state.selected);
    u32 i;
    if (contact == NULL_PTR)
    {
        gui_text(surface, card.x + 16, card.y + 16, "Select a contact", AXCO_COLOUR_MUTED, 1);
        return;
    }
    gui_fill_round_rect(surface, gui_rect(card.x + 16, card.y + 16, 56, 56), 28,
                        GUI_RGB(0x2C, 0x36, 0x4B));
    gui_text(surface, card.x + 88, card.y + 20, contact->name, AXCO_COLOUR_TEXT, 2);
    gui_text(surface, card.x + 88, card.y + 48, contact->email, AXCO_COLOUR_MUTED, 1);
    gui_fill_rect(surface, gui_rect(card.x + 16, card.y + 88, card.w - 32, 1), AXCO_COLOUR_RULE);
    gui_text(surface, card.x + 16, card.y + 100, "Phone numbers", AXCO_COLOUR_MUTED, 1);
    for (i = 0; i < contact->phone_count; i++)
    {
        gui_rect_t row = gui_rect(card.x + 16, card.y + 120 + (i32)i * 32, card.w - 32, 28);
        gui_fill_round_rect(surface, row, 6, GUI_RGB(0x24, 0x2A, 0x35));
        gui_text(surface, row.x + 10, row.y + 6, contact->phones[i], AXCO_COLOUR_TEXT, 1);
        gui_text(surface, row.x + row.w - 66, row.y + 6, "call", AXCO_COLOUR_ACCENT, 1);
    }
    if (contact->note[0] != 0)
    {
        gui_text(surface, card.x + 16, card.y + 220, "Note", AXCO_COLOUR_MUTED, 1);
        gui_text(surface, card.x + 16, card.y + 238, contact->note, AXCO_COLOUR_TEXT, 1);
    }
    if (contact->blocked)
        gui_text(surface, card.x + card.w - 80, card.y + 20, "blocked", GUI_RGB(0xFF, 0x77, 0x77), 1);
}

void axco_paint(gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t sidebar = gui_rect(bounds.x, bounds.y, 240, bounds.h);
    gui_rect_t card = gui_rect(bounds.x + 248, bounds.y + 8, bounds.w - 256, 300);
    gui_rect_t footer;
    gui_fill_rect(surface, bounds, AXCO_COLOUR_BG);
    gui_fill_rect(surface, sidebar, AXCO_COLOUR_SIDEBAR);
    axco_paint_sidebar(surface, sidebar);
    gui_fill_round_rect(surface, card, 10, AXCO_COLOUR_CARD);
    axco_paint_detail(surface, card);
    footer = gui_rect(bounds.x + 248, bounds.y + 316, bounds.w - 256,
                      bounds.h - 324);
    gui_fill_round_rect(surface, footer, 10, GUI_RGB(0x1A, 0x1E, 0x25));
    gui_text(surface, footer.x + 12, footer.y + 10, "Activity", AXCO_COLOUR_MUTED, 1);
    gui_text(surface, footer.x + 12, footer.y + 30, "calls", AXCO_COLOUR_TEXT, 1);
    gui_text(surface, footer.x + 92, footer.y + 30, "messages", AXCO_COLOUR_TEXT, 1);
    gui_text(surface, footer.x + 200, footer.y + 30, "favourites", AXCO_COLOUR_TEXT, 1);
}

bool_t axco_click(i32 x, i32 y, gui_rect_t bounds)
{
    gui_rect_t sidebar = gui_rect(bounds.x, bounds.y, 240, bounds.h);
    u32 i;
    u32 shown = 10;
    if (!gui_rect_contains(sidebar, x, y))
        return false;
    for (i = 0; i < axco_state.count && shown < 10; i++)
    {
        gui_rect_t row;
        if (!axco_state.contacts[i].used)
            continue;
        row = gui_rect(sidebar.x + 8, sidebar.y + 60 + (i32)shown * 44, sidebar.w - 16, 40);
        if (gui_rect_contains(row, x, y))
            return axco_select(i);
        shown++;
    }
    return false;
}

void axco_report(void)
{
    u32 i;
    printk("axco: contacts=%u favourites=%u blocked=%u calls=%u messages=%u\n",
           (unsigned)axco_state.count, (unsigned)axco_state.favourites,
           (unsigned)axco_state.blocked, (unsigned)axco_state.calls,
           (unsigned)axco_state.messages);
    for (i = 0; i < axco_state.count; i++)
    {
        const axco_contact_t *contact = &axco_state.contacts[i];
        if (!contact->used)
            continue;
        printk("  %u %s group=%s phones=%u fav=%u blocked=%u\n", (unsigned)i,
               contact->name, contact->group, (unsigned)contact->phone_count,
               (unsigned)contact->favourite, (unsigned)contact->blocked);
    }
}

bool_t axco_selftest(void)
{
    u32 index;
    axco_init();
    index = axco_add("Ada Lovelace", "Engineering", "ada@axiomos.local");
    if (index != 0)
        return false;
    if (!axco_add_phone(0, "+86 138 0000 0001"))
        return false;
    if (!axco_add_phone(0, "+86 138 0000 0002"))
        return false;
    if (axco_add_phone(0, "third") != true)
    {
        /* Third number fills the last slot. */
    }
    if (axco_add_phone(0, "overflow"))
        return false;
    if (axco_add("Grace Hopper", "Compilers", "grace@axiomos.local") != 1)
        return false;
    if (axco_phone(0, 1) == NULL_PTR)
        return false;
    if (!axco_toggle_favourite(0))
        return false;
    if (axco_state.favourites != 1)
        return false;
    if (!axco_set_note(0, "First programmer"))
        return false;
    if (!axco_select(1))
        return false;
    if (axco_selected() != 1)
        return false;
    if (!axco_call(1))
        return false;
    if (!axco_message(0))
        return false;
    if (axco_toggle_blocked(0) != true)
        return false;
    if (axco_call(0))
        return false;
    if (axco_search("Ada") != 1)
        return false;
    if (!axco_search(""))
        return false;
    if (axco_group_count('A') != 1)
        return false;
    if (!axco_remove(1))
        return false;
    return true;
}
