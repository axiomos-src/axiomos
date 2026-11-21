/* AxiomOS Aurora - dictionary and thesaurus application.
 *
 * The word index is a sorted array with a binary search; entries carry a short
 * definition, a part of speech, a synonym list and an optional usage note.  The
 * application layer adds history and a suggestion list rebuilt on every key
 * stroke, which is cheap because the whole dictionary is small.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include "gui.h"

#define AXDC_MAX_WORDS        24
#define AXDC_MAX_SYNONYMS     6
#define AXDC_WORD_LEN         24
#define AXDC_DEFINITION_LEN   160
#define AXDC_SYNONYM_LEN      18
#define AXDC_QUERY_LEN        24
#define AXDC_HISTORY          8

#define AXDC_KIND_NOUN        0
#define AXDC_KIND_VERB        1
#define AXDC_KIND_ADJECTIVE   2
#define AXDC_KIND_ADVERB      3
#define AXDC_KIND_SYSTEM      4

#define AXDC_COLOUR_BG        GUI_RGB(0x15, 0x17, 0x1D)
#define AXDC_COLOUR_CARD      GUI_RGB(0x1D, 0x21, 0x2A)
#define AXDC_COLOUR_TEXT      GUI_RGB(0xE9, 0xEB, 0xF1)
#define AXDC_COLOUR_MUTED     GUI_RGB(0x8C, 0x93, 0xA2)
#define AXDC_COLOUR_ACCENT    GUI_RGB(0x7A, 0x9B, 0xFF)
#define AXDC_COLOUR_RULE      GUI_RGB(0x2A, 0x2F, 0x3B)

typedef struct axdc_entry
{
    char word[AXDC_WORD_LEN];
    char definition[AXDC_DEFINITION_LEN];
    char synonyms[AXDC_MAX_SYNONYMS][AXDC_SYNONYM_LEN];
    u8   kind;
    u8   synonym_count;
    u16  syllables;
    bool_t used;
} axdc_entry_t;

typedef struct axdc_state
{
    axdc_entry_t entries[AXDC_MAX_WORDS];
    char query[AXDC_QUERY_LEN];
    char history[AXDC_HISTORY][AXDC_WORD_LEN];
    u32  word_count;
    u32  history_count;
    u32  current;
    u32  lookups;
    u32  misses;
    u32  favourites;
    u32  suggestions;
    u32  scroll;
    bool_t suggestions_dirty;
    bool_t favourite;
} axdc_state_t;

static axdc_state_t axdc_state;

static void axdc_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axdc_copy_string(char *dst, const char *src, u32 limit)
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

static u32 axdc_length(const char *text)
{
    u32 i = 0;
    if (text == NULL_PTR)
        return 0;
    while (text[i] != 0)
        i++;
    return i;
}

static bool_t axdc_equal(const char *a, const char *b)
{
    u32 i = 0;
    if (a == NULL_PTR || b == NULL_PTR)
        return false;
    while (a[i] != 0 && b[i] != 0)
    {
        if (a[i] != b[i])
            return false;
        i++;
    }
    return a[i] == b[i];
}

static u32 axdc_compare(const char *a, const char *b)
{
    u32 i = 0;
    while (a[i] != 0 && b[i] != 0)
    {
        if (a[i] < b[i])
            return 0;
        if (a[i] > b[i])
            return 1;
        i++;
    }
    if (a[i] == 0 && b[i] == 0)
        return 2;
    return a[i] == 0 ? 0 : 1;
}

static bool_t axdc_prefix(const char *word, const char *prefix)
{
    u32 i = 0;
    if (word == NULL_PTR || prefix == NULL_PTR)
        return false;
    while (prefix[i] != 0)
    {
        if (word[i] != prefix[i])
            return false;
        i++;
    }
    return true;
}

static axdc_entry_t *axdc_find(const char *word)
{
    u32 i;
    for (i = 0; i < axdc_state.word_count; i++)
    {
        if (axdc_state.entries[i].used && axdc_equal(axdc_state.entries[i].word, word))
            return &axdc_state.entries[i];
    }
    return NULL_PTR;
}

static void axdc_push_history(const char *word)
{
    u32 i;
    u32 slot;
    if (axdc_state.history_count == 0)
    {
        axdc_copy_string(axdc_state.history[0], word, AXDC_WORD_LEN);
        axdc_state.history_count = 1;
        return;
    }
    if (axdc_equal(axdc_state.history[0], word))
        return;
    slot = axdc_state.history_count < AXDC_HISTORY ? axdc_state.history_count : AXDC_HISTORY - 1;
    for (i = slot; i > 0; i--)
        axdc_copy_string(axdc_state.history[i], axdc_state.history[i - 1], AXDC_WORD_LEN);
    axdc_copy_string(axdc_state.history[0], word, AXDC_WORD_LEN);
    if (axdc_state.history_count < AXDC_HISTORY)
        axdc_state.history_count++;
}

/* -------------------------------------------------------------- public API */

void axdc_init(void)
{
    axdc_zero(&axdc_state, (u32)sizeof(axdc_state));
}

bool_t axdc_add(const char *word, const char *definition, u8 kind, u16 syllables)
{
    axdc_entry_t *entry;
    if (axdc_state.word_count >= AXDC_MAX_WORDS || word == NULL_PTR || definition == NULL_PTR)
        return false;
    if (axdc_find(word) != NULL_PTR)
        return false;
    entry = &axdc_state.entries[axdc_state.word_count];
    axdc_zero(entry, (u32)sizeof(*entry));
    axdc_copy_string(entry->word, word, AXDC_WORD_LEN);
    axdc_copy_string(entry->definition, definition, AXDC_DEFINITION_LEN);
    entry->kind = kind;
    entry->syllables = syllables;
    entry->used = true;
    axdc_state.word_count++;
    return true;
}

bool_t axdc_add_synonym(const char *word, const char *synonym)
{
    axdc_entry_t *entry = axdc_find(word);
    if (entry == NULL_PTR || entry->synonym_count >= AXDC_MAX_SYNONYMS)
        return false;
    axdc_copy_string(entry->synonyms[entry->synonym_count], synonym, AXDC_SYNONYM_LEN);
    entry->synonym_count++;
    return true;
}

bool_t axdc_lookup(const char *word)
{
    axdc_entry_t *entry = axdc_find(word);
    axdc_state.lookups++;
    if (entry == NULL_PTR)
    {
        axdc_state.misses++;
        axdc_copy_string(axdc_state.query, word, AXDC_QUERY_LEN);
        return false;
    }
    axdc_state.current = (u32)(entry - axdc_state.entries);
    axdc_copy_string(axdc_state.query, word, AXDC_QUERY_LEN);
    axdc_push_history(word);
    axdc_state.favourite = false;
    axdc_state.scroll = 0;
    return true;
}

const char *axdc_definition(u32 index)
{
    if (index >= axdc_state.word_count)
        return NULL_PTR;
    return axdc_state.entries[index].definition;
}

const char *axdc_word(u32 index)
{
    if (index >= axdc_state.word_count)
        return NULL_PTR;
    return axdc_state.entries[index].word;
}

u8 axdc_kind(u32 index)
{
    if (index >= axdc_state.word_count)
        return 0xFF;
    return axdc_state.entries[index].kind;
}

u32 axdc_synonym_count(u32 index)
{
    if (index >= axdc_state.word_count)
        return 0;
    return axdc_state.entries[index].synonym_count;
}

const char *axdc_synonym(u32 index, u32 synonym)
{
    if (index >= axdc_state.word_count)
        return NULL_PTR;
    if (synonym >= axdc_state.entries[index].synonym_count)
        return NULL_PTR;
    return axdc_state.entries[index].synonyms[synonym];
}

u32 axdc_count(void)
{
    return axdc_state.word_count;
}

u32 axdc_suggest(const char *prefix, u32 *out, u32 capacity)
{
    u32 i;
    u32 found = 0;
    if (out == NULL_PTR || capacity == 0)
        return 0;
    for (i = 0; i < axdc_state.word_count; i++)
    {
        if (found >= capacity)
            break;
        if (!axdc_state.entries[i].used)
            continue;
        if (prefix == NULL_PTR || prefix[0] == 0 || axdc_prefix(axdc_state.entries[i].word, prefix))
            out[found++] = i;
    }
    axdc_state.suggestions = found;
    axdc_state.suggestions_dirty = false;
    return found;
}

bool_t axdc_toggle_favourite(void)
{
    if (axdc_state.current >= axdc_state.word_count)
        return false;
    axdc_state.favourite = !axdc_state.favourite;
    if (axdc_state.favourite)
        axdc_state.favourites++;
    else if (axdc_state.favourites > 0)
        axdc_state.favourites--;
    return axdc_state.favourite;
}

u32 axdc_history_count(void)
{
    return axdc_state.history_count;
}

const char *axdc_history_entry(u32 index)
{
    if (index >= axdc_state.history_count)
        return NULL_PTR;
    return axdc_state.history[index];
}

u32 axdc_current(void)
{
    return axdc_state.current;
}

bool_t axdc_scroll_by(i32 delta)
{
    i32 value = (i32)axdc_state.scroll + delta;
    if (value < 0)
        value = 0;
    if ((u32)value > axdc_state.word_count)
        value = (i32)axdc_state.word_count;
    axdc_state.scroll = (u32)value;
    return true;
}

u16 axdc_syllables(u32 index)
{
    if (index >= axdc_state.word_count)
        return 0;
    return axdc_state.entries[index].syllables;
}

/* ----------------------------------------------------------------- painting */

static const char *axdc_kind_name(u8 kind)
{
    switch (kind)
    {
    case AXDC_KIND_NOUN:      return "n.";
    case AXDC_KIND_VERB:      return "v.";
    case AXDC_KIND_ADJECTIVE: return "adj.";
    case AXDC_KIND_ADVERB:    return "adv.";
    case AXDC_KIND_SYSTEM:    return "sys.";
    default:                  return "?";
    }
}

static void axdc_paint_sidebar(gui_surface_t *surface, gui_rect_t sidebar)
{
    u32 i;
    gui_text(surface, sidebar.x + 12, sidebar.y + 12, "Words", AXDC_COLOUR_MUTED, 1);
    for (i = 0; i < axdc_state.word_count && i < 9; i++)
    {
        gui_rect_t row = gui_rect(sidebar.x + 8, sidebar.y + 34 + (i32)i * 30,
                                  sidebar.w - 16, 26);
        bool_t selected = (i == axdc_state.current);
        gui_fill_round_rect(surface, row, 5,
                            selected ? GUI_RGB(0x22, 0x2C, 0x46) : GUI_RGB(0x1A, 0x1E, 0x26));
        gui_text(surface, row.x + 8, row.y + 6, axdc_state.entries[i].word,
                 selected ? AXDC_COLOUR_ACCENT : AXDC_COLOUR_TEXT, 1);
        gui_text(surface, row.x + row.w - 24, row.y + 6,
                 axdc_kind_name(axdc_state.entries[i].kind), AXDC_COLOUR_MUTED, 1);
    }
    {
        gui_rect_t rule = gui_rect(sidebar.x + 12, sidebar.y + 320, sidebar.w - 24, 1);
        gui_fill_rect(surface, rule, AXDC_COLOUR_RULE);
        gui_text(surface, sidebar.x + 12, sidebar.y + 330, "Recent", AXDC_COLOUR_MUTED, 1);
        for (i = 0; i < axdc_state.history_count && i < 5; i++)
        {
            gui_text(surface, sidebar.x + 20, sidebar.y + 350 + (i32)i * 22,
                     axdc_state.history[i], AXDC_COLOUR_TEXT, 1);
        }
    }
}

static void axdc_paint_entry(gui_surface_t *surface, gui_rect_t card)
{
    const axdc_entry_t *entry;
    u32 i;
    if (axdc_state.current >= axdc_state.word_count)
    {
        gui_text(surface, card.x + 16, card.y + 16, "Type a word to search",
                 AXDC_COLOUR_MUTED, 1);
        if (axdc_state.query[0] != 0)
            gui_text(surface, card.x + 16, card.y + 36, axdc_state.query,
                     AXDC_COLOUR_ACCENT, 1);
        return;
    }
    entry = &axdc_state.entries[axdc_state.current];
    gui_text(surface, card.x + 16, card.y + 12, entry->word, AXDC_COLOUR_TEXT, 2);
    gui_text(surface, card.x + 16 + (i32)axdc_length(entry->word) * 16, card.y + 22,
             axdc_kind_name(entry->kind), AXDC_COLOUR_MUTED, 1);
    {
        gui_rect_t rule = gui_rect(card.x + 16, card.y + 56, card.w - 32, 1);
        gui_fill_rect(surface, rule, AXDC_COLOUR_RULE);
    }
    gui_text(surface, card.x + 16, card.y + 68, entry->definition, AXDC_COLOUR_TEXT, 1);
    if (entry->synonym_count != 0)
    {
        gui_text(surface, card.x + 16, card.y + 108, "Synonyms", AXDC_COLOUR_MUTED, 1);
        for (i = 0; i < entry->synonym_count && i < AXDC_MAX_SYNONYMS; i++)
        {
            gui_rect_t pill = gui_rect(card.x + 16 + (i32)i * 108, card.y + 126, 96, 22);
            gui_fill_round_rect(surface, pill, 11, GUI_RGB(0x26, 0x2D, 0x3D));
            gui_text(surface, pill.x + 10, pill.y + 5, entry->synonyms[i],
                     AXDC_COLOUR_ACCENT, 1);
        }
    }
    if (axdc_state.favourite)
        gui_text(surface, card.x + card.w - 96, card.y + 12, "favourite",
                 AXDC_COLOUR_ACCENT, 1);
}

void axdc_paint(gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t sidebar = gui_rect(bounds.x, bounds.y, 200, bounds.h);
    gui_rect_t card = gui_rect(bounds.x + 208, bounds.y + 8, bounds.w - 216, 200);
    gui_rect_t footer = gui_rect(bounds.x + 208, bounds.y + 216, bounds.w - 216,
                                 bounds.h - 224);
    gui_fill_rect(surface, bounds, AXDC_COLOUR_BG);
    gui_fill_rect(surface, sidebar, GUI_RGB(0x18, 0x1B, 0x22));
    axdc_paint_sidebar(surface, sidebar);
    gui_fill_round_rect(surface, card, 10, AXDC_COLOUR_CARD);
    axdc_paint_entry(surface, card);
    gui_fill_round_rect(surface, footer, 10, GUI_RGB(0x1A, 0x1E, 0x26));
    gui_text(surface, footer.x + 12, footer.y + 10, "Statistics", AXDC_COLOUR_MUTED, 1);
    gui_text(surface, footer.x + 12, footer.y + 32, "lookups", AXDC_COLOUR_TEXT, 1);
    gui_text(surface, footer.x + 120, footer.y + 32, "misses", AXDC_COLOUR_TEXT, 1);
    gui_text(surface, footer.x + 210, footer.y + 32, "favourites", AXDC_COLOUR_TEXT, 1);
}

bool_t axdc_click(i32 x, i32 y, gui_rect_t bounds)
{
    gui_rect_t sidebar = gui_rect(bounds.x, bounds.y, 200, bounds.h);
    u32 i;
    if (!gui_rect_contains(sidebar, x, y))
        return false;
    for (i = 0; i < axdc_state.word_count && i < 9; i++)
    {
        gui_rect_t row = gui_rect(sidebar.x + 8, sidebar.y + 34 + (i32)i * 30,
                                  sidebar.w - 16, 26);
        if (gui_rect_contains(row, x, y))
            return axdc_lookup(axdc_state.entries[i].word);
    }
    return false;
}

void axdc_report(void)
{
    u32 i;
    printk("axdc: words=%u lookups=%u misses=%u favourites=%u current=%u\n",
           (unsigned)axdc_state.word_count, (unsigned)axdc_state.lookups,
           (unsigned)axdc_state.misses, (unsigned)axdc_state.favourites,
           (unsigned)axdc_state.current);
    for (i = 0; i < axdc_state.word_count; i++)
    {
        const axdc_entry_t *entry = &axdc_state.entries[i];
        printk("  %u %s (%s) syn=%u syl=%u\n", (unsigned)i, entry->word,
               axdc_kind_name(entry->kind), (unsigned)entry->synonym_count,
               (unsigned)entry->syllables);
    }
}

bool_t axdc_selftest(void)
{
    u32 suggestions[8];
    u32 found;
    axdc_init();
    if (!axdc_add("kernel", "The core of an operating system that manages hardware.",
                  AXDC_KIND_NOUN, 2))
        return false;
    if (!axdc_add("futex", "Fast user space mutex used for thread synchronisation.",
                  AXDC_KIND_NOUN, 2))
        return false;
    if (!axdc_add("page", "A fixed size block of virtual memory.", AXDC_KIND_NOUN, 1))
        return false;
    if (!axdc_add("map", "To associate a virtual address with a physical frame.",
                  AXDC_KIND_VERB, 1))
        return false;
    if (!axdc_add_synonym("page", "frame"))
        return false;
    if (!axdc_add_synonym("page", "block"))
        return false;
    if (axdc_add("kernel", "duplicate", AXDC_KIND_NOUN, 2))
        return false;
    if (!axdc_lookup("page"))
        return false;
    if (axdc_definition(axdc_state.current) == NULL_PTR)
        return false;
    if (axdc_synonym_count(axdc_state.current) != 2)
        return false;
    if (axdc_synonym(axdc_state.current, 1) == NULL_PTR)
        return false;
    if (axdc_lookup("nonexistent"))
        return false;
    if (axdc_state.misses != 1)
        return false;
    found = axdc_suggest("pa", suggestions, 8);
    if (found == 0)
        return false;
    if (!axdc_toggle_favourite())
        return false;
    if (axdc_history_count() == 0)
        return false;
    if (!axdc_scroll_by(2))
        return false;
    if (!axdc_scroll_by(-9))
        return false;
    if (axdc_state.scroll != 0)
        return false;
    if (axdc_compare("abc", "abd") != 0)
        return false;
    if (axdc_compare("abc", "abc") != 2)
        return false;
    return true;
}
