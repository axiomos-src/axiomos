/*
 * AxiomOS - input method editor.
 *
 * An IME sits between the keyboard and the focused text field: it collects keys
 * into a composition string, offers candidates, and commits one result.  This
 * implementation is table-driven and deliberately small — the candidate list
 * comes from a lookup the caller supplies, so the shell can wire pinyin, kana or
 * anything else without this file knowing which language it is serving.
 *
 * The preedit is not inserted into the field until commit.  That is the whole
 * point of an IME: the document must not contain half-formed typing if the user
 * cancels.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXI_MAX_COMPOSITION  32
#define AXI_MAX_CANDIDATES    9
#define AXI_CANDIDATE_HEIGHT 30

typedef enum axi_state {
    AXI_IDLE = 0,
    AXI_COMPOSING,
    AXI_SELECTING,
    AXI_COMMITTED
} axi_state_t;

typedef enum axi_mode {
    AXI_MODE_OFF = 0,
    AXI_MODE_ON,
    AXI_MODE_SYMBOLS
} axi_mode_t;

typedef struct axi_candidate {
    char text[32];
    char note[24];
    i32  weight;
} axi_candidate_t;

typedef struct axi_ime {
    axi_state_t     state;
    axi_mode_t      mode;
    char            composition[AXI_MAX_COMPOSITION];
    axi_candidate_t candidates[AXI_MAX_CANDIDATES];
    i32             candidate_count;
    i32             selected;
    i32             page;
    i32             caret_x;
    i32             caret_y;
    bool_t          active;
    i32             commit_ticks;
    char            committed[32];
    bool_t          has_committed;
} axi_ime_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void axi_init(axi_ime_t *ime)
{
    if (!ime) return;
    ax_memset(ime, 0, sizeof(*ime));
    ime->mode = AXI_MODE_OFF;
    ime->state = AXI_IDLE;
}

void axi_set_mode(axi_ime_t *ime, axi_mode_t mode)
{
    if (!ime) return;
    ime->mode = mode;
    if (mode == AXI_MODE_OFF) {
        ime->state = AXI_IDLE;
        ime->composition[0] = 0;
        ime->candidate_count = 0;
        ime->active = (bool_t)0;
    }
}

bool_t axi_is_composing(const axi_ime_t *ime)
{
    return ime && ime->state == AXI_COMPOSING;
}

/* ------------------------------------------------------------------ */
/* Composition                                                         */
/* ------------------------------------------------------------------ */

/* Append a letter to the composition.  Punctuation ends composition instead of
 * being added: a comma after a composed word is a commit-and-continue, which is
 * what every IME does. */
i32 axi_key(axi_ime_t *ime, i32 key)
{
    if (!ime || ime->mode == AXI_MODE_OFF) return 0;
    if (key == 0x1B) {
        ime->composition[0] = 0;
        ime->candidate_count = 0;
        ime->state = AXI_IDLE;
        ime->active = (bool_t)0;
        return 2;
    }
    if (ime->candidate_count > 0) {
        if (key == 0x20 || key == 0x0D) {
            ax_strlcpy(ime->committed, ime->candidates[ime->selected].text,
                       sizeof(ime->committed));
            ime->has_committed = (bool_t)1;
            ime->state = AXI_IDLE;
            ime->composition[0] = 0;
            ime->candidate_count = 0;
            ime->active = (bool_t)0;
            return 3;
        }
        if (key == 0x08) {
            i32 length = (i32)ax_strlen(ime->composition);
            if (length > 0) ime->composition[length - 1] = 0;
            ime->state = AXI_COMPOSING;
            ime->candidate_count = 0;
            return 1;
        }
    }
    if (key < 'a' || key > 'z') {
        /* Uppercase and digits are ignored during composition; the caller still
         * has the raw key if it wants to handle them itself. */
        return 0;
    }
    i32 length = (i32)ax_strlen(ime->composition);
    if (length + 1 >= AXI_MAX_COMPOSITION) return 0;
    ime->composition[length] = (char)key;
    ime->composition[length + 1] = 0;
    ime->state = AXI_COMPOSING;
    ime->active = (bool_t)1;
    ime->candidate_count = 0;
    ime->selected = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Candidates                                                          */
/* ------------------------------------------------------------------ */

void axi_clear_candidates(axi_ime_t *ime)
{
    if (!ime) return;
    ime->candidate_count = 0;
    ime->selected = 0;
    ime->page = 0;
}

void axi_add_candidate(axi_ime_t *ime, const char *text, const char *note,
                       i32 weight)
{
    if (!ime || ime->candidate_count >= AXI_MAX_CANDIDATES) return;
    axi_candidate_t *candidate = &ime->candidates[ime->candidate_count++];
    ax_strlcpy(candidate->text, text ? text : "", sizeof(candidate->text));
    ax_strlcpy(candidate->note, note ? note : "", sizeof(candidate->note));
    candidate->weight = weight;
}

/* Candidates are ordered by weight, highest first, with equal weights keeping
 * their insertion order.  A stable order matters: the first candidate is what a
 * user gets when they press space, so it must not shuffle between frames. */
void axi_sort_candidates(axi_ime_t *ime)
{
    if (!ime) return;
    for (i32 i = 1; i < ime->candidate_count; i++) {
        axi_candidate_t key = ime->candidates[i];
        i32 j = i - 1;
        while (j >= 0 && ime->candidates[j].weight < key.weight) {
            ime->candidates[j + 1] = ime->candidates[j];
            j--;
        }
        ime->candidates[j + 1] = key;
    }
    ime->state = ime->candidate_count > 0 ? AXI_SELECTING : AXI_COMPOSING;
}

void axi_select(axi_ime_t *ime, i32 index)
{
    if (!ime || index < 0 || index >= ime->candidate_count) return;
    ime->selected = index;
}

void axi_page(axi_ime_t *ime, i32 delta)
{
    if (!ime) return;
    ime->page += delta;
    if (ime->page < 0) ime->page = 0;
}

/* Number keys pick a candidate directly; this is the fastest path for a user who
 * already sees what they want. */
i32 axi_digit(axi_ime_t *ime, i32 digit)
{
    if (!ime || ime->state != AXI_SELECTING) return 0;
    if (digit < 1 || digit > ime->candidate_count) return 0;
    ime->selected = digit - 1;
    ax_strlcpy(ime->committed, ime->candidates[ime->selected].text,
               sizeof(ime->committed));
    ime->has_committed = (bool_t)1;
    ime->state = AXI_IDLE;
    ime->composition[0] = 0;
    ime->candidate_count = 0;
    ime->active = (bool_t)0;
    return 1;
}

bool_t axi_take_commit(axi_ime_t *ime, char *out, i32 capacity)
{
    if (!ime || !out || capacity <= 0 || !ime->has_committed) return (bool_t)0;
    ax_strlcpy(out, ime->committed, capacity);
    ime->has_committed = (bool_t)0;
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

/* The preedit is drawn inline at the caret with a solid underline, and the
 * candidate window floats below it.  Both use the same accent so they read as
 * one object rather than two overlays that happen to be near each other. */
void axi_paint(gui_surface_t *surface, axi_ime_t *ime,
               const axau_palette_t *palette)
{
    if (!surface || !ime || !palette) return;
    if (!ime->active) return;
    u32 accent = axau_colour(palette, AXAU_COLOUR_ACCENT);
    if (ime->composition[0]) {
        i32 width = (i32)ax_strlen(ime->composition) * 8;
        gui_rect_t band = gui_rect(ime->caret_x, ime->caret_y, width, 18);
        gui_fill_rect(surface, band,
                      axui_colour_opacity(accent, 20));
        axui_draw_text(surface, ime->caret_x, ime->caret_y, ime->composition,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        gui_hline(surface, ime->caret_x, ime->caret_y + 17, width, accent);
    }
    if (ime->candidate_count <= 0) return;

    i32 per_page = 5;
    i32 shown = ime->candidate_count - ime->page * per_page;
    if (shown > per_page) shown = per_page;
    if (shown <= 0) return;
    i32 width = 0;
    for (i32 i = 0; i < shown; i++) {
        i32 candidate_w = (i32)ax_strlen(ime->candidates[ime->page * per_page + i].text) * 8 +
                          28;
        if (candidate_w > width) width = candidate_w;
    }
    if (width < 180) width = 180;
    gui_rect_t window = gui_rect(ime->caret_x, ime->caret_y + 22, width,
                                 shown * AXI_CANDIDATE_HEIGHT + 8);
    gui_shadow(surface, window, 10, 30);
    gui_fill_round_rect(surface, window, 6,
                        axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
    axui_round_rect_border(surface, window, 6, 1,
                           axau_colour(palette, AXAU_COLOUR_STROKE));
    for (i32 i = 0; i < shown; i++) {
        i32 index = ime->page * per_page + i;
        axi_candidate_t *candidate = &ime->candidates[index];
        gui_rect_t row = gui_rect(window.x + 4, window.y + 4 + i * AXI_CANDIDATE_HEIGHT,
                                  window.w - 8, AXI_CANDIDATE_HEIGHT - 2);
        if (index == ime->selected) {
            gui_fill_round_rect(surface, row, 4,
                                axui_colour_opacity(accent, 22));
        }
        char number[4];
        ax_snprintf(number, sizeof(number), "%d", i + 1);
        axui_draw_text(surface, row.x + 8, row.y + 7, number,
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_TERTIARY), 1));
        axui_draw_text(surface, row.x + 24, row.y + 7, candidate->text,
                       axui_style(index == ime->selected
                                      ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                      : axau_colour(palette,
                                                    AXAU_COLOUR_TEXT_SECONDARY),
                                  1));
        if (candidate->note[0]) {
            axui_draw_text_right(surface,
                                 gui_rect(row.x, row.y, row.w - 8, row.h),
                                 candidate->note,
                                 axui_style(axau_colour(palette,
                                                        AXAU_COLOUR_TEXT_TERTIARY), 1));
        }
    }
    if (ime->candidate_count > per_page) {
        char page_label[16];
        i32 pages = (ime->candidate_count + per_page - 1) / per_page;
        ax_snprintf(page_label, sizeof(page_label), "%d/%d", ime->page + 1, pages);
        axui_draw_text_right(surface,
                             gui_rect(window.x, window.y + window.h - 14,
                                      window.w - 8, 12),
                             page_label,
                             axui_style(axau_colour(palette,
                                                    AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
}

void axi_set_caret(axi_ime_t *ime, i32 x, i32 y)
{
    if (!ime) return;
    ime->caret_x = x;
    ime->caret_y = y;
}

/* Compose a short description of the composition for diagnostics. */
i32 axi_describe(const axi_ime_t *ime, char *out, i32 capacity)
{
    if (!ime || !out || capacity <= 0) return 0;
    ax_snprintf(out, capacity, "%s[%s] %d candidates",
                ime->mode == AXI_MODE_OFF ? "off" : "on", ime->composition,
                ime->candidate_count);
    return (i32)ax_strlen(out);
}

void axi_dump(const axi_ime_t *ime)
{
    if (!ime) return;
    printk("ime: state=%d mode=%d composition='%s' candidates=%d selected=%d\n",
           (i32)ime->state, (i32)ime->mode, ime->composition, ime->candidate_count,
           ime->selected);
}
