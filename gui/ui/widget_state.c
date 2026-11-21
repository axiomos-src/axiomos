/* AxiomOS UI - widget state bookkeeping.
 *
 * Everything here is about the *state* of a control rather than its pixels:
 * how a raw event bitmask folds into the state enum, a small registry that
 * remembers which rect belongs to which control id (so a shell page can hit
 * test without keeping a parallel array), and a transition log that the
 * settings page renders as a live event trace.
 *
 * The registry is deliberately tiny and stack friendly: at most
 * AXW_REGISTRY_MAX entries, each a rect plus an id and a state word.  Layers
 * are honoured by searching from the last entry backwards, which matches the
 * paint order used by the window manager.
 */

#include <axiom/lib.h>
#include "widgets.h"

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

const char *axw_state_name(axw_state_t state)
{
    switch (state) {
    case AXW_STATE_NORMAL:
        return "normal";
    case AXW_STATE_HOVER:
        return "hover";
    case AXW_STATE_PRESSED:
        return "pressed";
    case AXW_STATE_FOCUSED:
        return "focused";
    case AXW_STATE_DISABLED:
        return "disabled";
    case AXW_STATE_CHECKED:
        return "checked";
    case AXW_STATE_INDETERMINATE:
        return "indeterminate";
    default:
        return "?";
    }
}

/* Append a word to a comma separated list, respecting the caller's capacity. */
static void append_word(char *out, usize cap, usize *used, const char *word)
{
    if (*used >= cap - 1) {
        return;
    }
    if (*used > 0) {
        out[(*used)++] = '|';
    }
    while (*word && *used < cap - 1) {
        out[(*used)++] = *word++;
    }
    out[*used] = 0;
}

/* Human readable form of an event mask, e.g. "click|focus". */
void axw_event_name(u32 events, char *buffer, usize cap)
{
    if (!buffer || cap < 2) {
        return;
    }
    buffer[0] = 0;
    usize used = 0;
    if (events == AXW_NONE) {
        append_word(buffer, cap, &used, "none");
        return;
    }
    if (events & AXW_HOVER) {
        append_word(buffer, cap, &used, "hover");
    }
    if (events & AXW_PRESS) {
        append_word(buffer, cap, &used, "press");
    }
    if (events & AXW_RELEASE) {
        append_word(buffer, cap, &used, "release");
    }
    if (events & AXW_CLICK) {
        append_word(buffer, cap, &used, "click");
    }
    if (events & AXW_CANCELLED) {
        append_word(buffer, cap, &used, "cancelled");
    }
    if (events & AXW_TOGGLE) {
        append_word(buffer, cap, &used, "toggle");
    }
    if (events & AXW_CHANGED) {
        append_word(buffer, cap, &used, "changed");
    }
    if (events & AXW_SELECT) {
        append_word(buffer, cap, &used, "select");
    }
    if (events & AXW_SCROLL) {
        append_word(buffer, cap, &used, "scroll");
    }
    if (events & AXW_FOCUS) {
        append_word(buffer, cap, &used, "focus");
    }
    if (events & AXW_BLUR) {
        append_word(buffer, cap, &used, "blur");
    }
    if (events & AXW_KEY) {
        append_word(buffer, cap, &used, "key");
    }
    if (events & AXW_OPEN) {
        append_word(buffer, cap, &used, "open");
    }
    if (events & AXW_CLOSE) {
        append_word(buffer, cap, &used, "close");
    }
    if (events & ~(u32)(AXW_HOVER | AXW_PRESS | AXW_RELEASE | AXW_CLICK |
                        AXW_CANCELLED | AXW_TOGGLE | AXW_CHANGED | AXW_SELECT |
                        AXW_SCROLL | AXW_FOCUS | AXW_BLUR | AXW_KEY | AXW_OPEN |
                        AXW_CLOSE)) {
        append_word(buffer, cap, &used, "payload");
    }
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */



void axw_mask_clear(axw_enable_mask_t *mask)
{
    if (mask) {
        ax_memset(mask->words, 0, sizeof(mask->words));
    }
}

void axw_mask_disable(axw_enable_mask_t *mask, i32 id)
{
    if (!mask || id < 0 || id >= AXW_DISABLED_WORDS * 32) {
        return;
    }
    mask->words[id / 32] |= 1u << (u32)(id % 32);
}

void axw_mask_enable(axw_enable_mask_t *mask, i32 id)
{
    if (!mask || id < 0 || id >= AXW_DISABLED_WORDS * 32) {
        return;
    }
    mask->words[id / 32] &= ~(1u << (u32)(id % 32));
}

bool_t axw_mask_is_disabled(const axw_enable_mask_t *mask, i32 id)
{
    if (!mask || id < 0 || id >= AXW_DISABLED_WORDS * 32) {
        return (bool_t)0;
    }
    return (bool_t)((mask->words[id / 32] >> (u32)(id % 32)) & 1u);
}
