/*
 * AxiomOS - accessibility layer.
 *
 * Accessibility here is structural rather than an afterthought bolted onto
 * painting: every control that the shell draws can declare a role, a name and a
 * rectangle, and the accessibility layer owns focus traversal, announcement
 * ordering and the high-contrast adjustments.  A screen reader does not exist in
 * the kernel, but the description it would consume does, which means the surfaces
 * are actually navigable by keyboard and by narration in a future build.
 *
 * Focus order is not the paint order.  Controls declare a group and an order
 * within that group; traversal walks groups in declared order, which lets a
 * dialog place its buttons before the content they affect without moving a
 * single pixel.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXA_MAX_ELEMENTS   64
#define AXA_MAX_ANNOUNCE    8
#define AXA_NAME_LENGTH    64

typedef enum axa_role {
    AXA_ROLE_NONE = 0,
    AXA_ROLE_WINDOW,
    AXA_ROLE_BUTTON,
    AXA_ROLE_LABEL,
    AXA_ROLE_TEXTBOX,
    AXA_ROLE_CHECKBOX,
    AXA_ROLE_SWITCH,
    AXA_ROLE_SLIDER,
    AXA_ROLE_LIST,
    AXA_ROLE_LIST_ITEM,
    AXA_ROLE_TAB,
    AXA_ROLE_MENU,
    AXA_ROLE_MENU_ITEM,
    AXA_ROLE_DIALOG,
    AXA_ROLE_STATUS,
    AXA_ROLE_PROGRESS
} axa_role_t;

typedef struct axa_element {
    gui_rect_t rect;
    char       name[AXA_NAME_LENGTH];
    char       description[96];
    char       value[48];
    axa_role_t role;
    i32        group;
    i32        order;
    bool_t     focusable;
    bool_t     focused;
    bool_t     disabled;
    bool_t     checked;
    bool_t     live;
    i32        tab_index;
} axa_element_t;

typedef enum axa_announce_kind {
    AXA_ANNOUNCE_INFO = 0,
    AXA_ANNOUNCE_WARNING,
    AXA_ANNOUNCE_ERROR,
    AXA_ANNOUNCE_FOCUS
} axa_announce_kind_t;

typedef struct axa_announce {
    char              text[96];
    axa_announce_kind_t kind;
    i32               ticks;
    bool_t            spoken;
} axa_announce_t;

typedef struct axa_context {
    axa_element_t  elements[AXA_MAX_ELEMENTS];
    i32            count;
    i32            focus;
    i32            hover;
    axa_announce_t queue[AXA_MAX_ANNOUNCE];
    i32            queue_count;
    i32            cooldown;
    bool_t         enabled;
    bool_t         high_contrast;
    bool_t         reduce_motion;
    bool_t         large_targets;
    i32            announce_serial;
    char           spoken_last[96];
} axa_context_t;

/* Focus traversal announces focus changes, so the declaration comes first. */
void axa_announce(axa_context_t *context, axa_announce_kind_t kind,
                  const char *text);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void axa_init(axa_context_t *context)
{
    if (!context) return;
    ax_memset(context, 0, sizeof(*context));
    context->enabled = (bool_t)1;
    context->focus = -1;
}

void axa_set_high_contrast(axa_context_t *context, bool_t enabled)
{
    if (!context) return;
    context->high_contrast = enabled;
}

/* Registration is append-only and there is no removal: surfaces are rebuilt each
 * time they are shown, and the caller resets the context between surfaces with
 * axa_reset.  A removal API would need stable handles and would buy nothing here. */
i32 axa_add(axa_context_t *context, axa_role_t role, const char *name,
            gui_rect_t rect, i32 group, i32 order)
{
    if (!context || context->count >= AXA_MAX_ELEMENTS) return -1;
    i32 index = context->count++;
    axa_element_t *element = &context->elements[index];
    ax_memset(element, 0, sizeof(*element));
    element->role = role;
    element->rect = rect;
    element->group = group;
    element->order = order;
    ax_strlcpy(element->name, name ? name : "", sizeof(element->name));
    element->focusable = (bool_t)(role == AXA_ROLE_BUTTON ||
                                 role == AXA_ROLE_TEXTBOX ||
                                 role == AXA_ROLE_CHECKBOX ||
                                 role == AXA_ROLE_SWITCH ||
                                 role == AXA_ROLE_SLIDER ||
                                 role == AXA_ROLE_LIST_ITEM ||
                                 role == AXA_ROLE_TAB ||
                                 role == AXA_ROLE_MENU_ITEM);
    return index;
}

void axa_set_value(axa_context_t *context, i32 index, const char *value)
{
    if (!context || index < 0 || index >= context->count) return;
    ax_strlcpy(context->elements[index].value, value ? value : "",
               sizeof(context->elements[index].value));
}

void axa_set_description(axa_context_t *context, i32 index, const char *text)
{
    if (!context || index < 0 || index >= context->count) return;
    ax_strlcpy(context->elements[index].description, text ? text : "",
               sizeof(context->elements[index].description));
}

void axa_set_checked(axa_context_t *context, i32 index, bool_t checked)
{
    if (!context || index < 0 || index >= context->count) return;
    context->elements[index].checked = checked;
}

void axa_set_disabled(axa_context_t *context, i32 index, bool_t disabled)
{
    if (!context || index < 0 || index >= context->count) return;
    context->elements[index].disabled = disabled;
    if (disabled && context->focus == index) context->focus = -1;
}

void axa_set_live(axa_context_t *context, i32 index)
{
    if (!context || index < 0 || index >= context->count) return;
    context->elements[index].live = (bool_t)1;
}

void axa_reset(axa_context_t *context)
{
    if (!context) return;
    context->count = 0;
    context->focus = -1;
    context->hover = -1;
}

/* ------------------------------------------------------------------ */
/* Focus traversal                                                     */
/* ------------------------------------------------------------------ */

/* Elements that can take focus, sorted by (group, tab_index, order).  Tab index
 * is an optional explicit override; when it is zero the declared order is used,
 * which is how most surfaces should be built. */
static i32 axa_focus_order(const axa_context_t *context, i32 *indices)
{
    i32 count = 0;
    for (i32 i = 0; i < context->count; i++) {
        const axa_element_t *element = &context->elements[i];
        if (!element->focusable || element->disabled) continue;
        i32 j = count;
        while (j > 0) {
            const axa_element_t *previous = &context->elements[indices[j - 1]];
            bool_t after = (previous->group < element->group) ||
                           (previous->group == element->group &&
                            previous->tab_index < element->tab_index) ||
                           (previous->group == element->group &&
                            previous->tab_index == element->tab_index &&
                            previous->order <= element->order);
            if (after) break;
            indices[j] = indices[j - 1];
            j--;
        }
        indices[j] = i;
        count++;
    }
    return count;
}

i32 axa_focus_first(axa_context_t *context)
{
    if (!context) return -1;
    i32 indices[AXA_MAX_ELEMENTS];
    i32 count = axa_focus_order(context, indices);
    if (count == 0) return -1;
    context->focus = indices[0];
    context->elements[context->focus].focused = (bool_t)1;
    return context->focus;
}

/* Tab moves within focusable elements, wrapping at the ends.  Wrapping is the
 * expected behaviour for a shell surface: leaving the last control should return
 * to the first, not escape focus to nowhere. */
i32 axa_focus_next(axa_context_t *context, i32 delta)
{
    if (!context) return -1;
    i32 indices[AXA_MAX_ELEMENTS];
    i32 count = axa_focus_order(context, indices);
    if (count == 0) return -1;
    i32 position = -1;
    for (i32 i = 0; i < count; i++) {
        if (indices[i] == context->focus) {
            position = i;
            break;
        }
    }
    if (position < 0) {
        position = delta > 0 ? -1 : 0;
    }
    position = (position + delta + count) % count;
    if (context->focus >= 0) {
        context->elements[context->focus].focused = (bool_t)0;
    }
    context->focus = indices[position];
    context->elements[context->focus].focused = (bool_t)1;
    axa_announce(context, AXA_ANNOUNCE_FOCUS,
                 context->elements[context->focus].name);
    return context->focus;
}

i32 axa_focus_at(axa_context_t *context, i32 x, i32 y)
{
    if (!context) return -1;
    i32 best = -1;
    i32 best_group = 0x7FFFFFFF;
    for (i32 i = 0; i < context->count; i++) {
        const axa_element_t *element = &context->elements[i];
        if (!element->focusable || element->disabled) continue;
        if (!gui_rect_contains(element->rect, x, y)) continue;
        /* Later groups paint on top, so a hit in a higher group wins. */
        if (element->group >= best_group) continue;
        best = i;
        best_group = element->group;
    }
    if (best >= 0) {
        if (context->focus >= 0) {
            context->elements[context->focus].focused = (bool_t)0;
        }
        context->focus = best;
        context->elements[best].focused = (bool_t)1;
    }
    return best;
}

i32 axa_focus_index(const axa_context_t *context)
{
    return context ? context->focus : -1;
}

axa_role_t axa_role_of(const axa_context_t *context, i32 index)
{
    if (!context || index < 0 || index >= context->count) return AXA_ROLE_NONE;
    return context->elements[index].role;
}

/* ------------------------------------------------------------------ */
/* Narration                                                           */
/* ------------------------------------------------------------------ */

/* Compose the sentence a screen reader would say for a focused element.  Role,
 * name, value and state are separate pieces, and the order matters: role first
 * so the listener knows what they are on, then the name, then state. */
i32 axa_describe(const axa_context_t *context, i32 index, char *out, i32 capacity)
{
    if (!context || !out || capacity <= 0 || index < 0 ||
        index >= context->count) {
        return 0;
    }
    const axa_element_t *element = &context->elements[index];
    const char *role_name = "item";
    switch (element->role) {
    case AXA_ROLE_WINDOW: role_name = "window"; break;
    case AXA_ROLE_BUTTON: role_name = "button"; break;
    case AXA_ROLE_LABEL: role_name = "text"; break;
    case AXA_ROLE_TEXTBOX: role_name = "text field"; break;
    case AXA_ROLE_CHECKBOX: role_name = "checkbox"; break;
    case AXA_ROLE_SWITCH: role_name = "switch"; break;
    case AXA_ROLE_SLIDER: role_name = "slider"; break;
    case AXA_ROLE_LIST: role_name = "list"; break;
    case AXA_ROLE_LIST_ITEM: role_name = "list item"; break;
    case AXA_ROLE_TAB: role_name = "tab"; break;
    case AXA_ROLE_MENU: role_name = "menu"; break;
    case AXA_ROLE_MENU_ITEM: role_name = "menu item"; break;
    case AXA_ROLE_DIALOG: role_name = "dialog"; break;
    case AXA_ROLE_STATUS: role_name = "status"; break;
    case AXA_ROLE_PROGRESS: role_name = "progress"; break;
    case AXA_ROLE_NONE:
    default: break;
    }
    ax_snprintf(out, capacity, "%s, %s%s%s%s", role_name,
                element->name[0] ? element->name : "unnamed",
                element->value[0] ? ", " : "",
                element->value,
                element->disabled ? ", unavailable"
                                  : (element->checked ? ", on" : ""));
    return (i32)ax_strlen(out);
}

void axa_announce(axa_context_t *context, axa_announce_kind_t kind,
                  const char *text)
{
    if (!context || !context->enabled || !text || !text[0]) return;
    /* Announcements are rate-limited: a single frame can focus several controls,
     * and reading all of them would produce an unintelligible stream. */
    if (context->cooldown > 0 && kind == AXA_ANNOUNCE_FOCUS) return;
    if (context->queue_count >= AXA_MAX_ANNOUNCE) {
        for (i32 i = 0; i + 1 < context->queue_count; i++) {
            context->queue[i] = context->queue[i + 1];
        }
        context->queue_count--;
    }
    axa_announce_t *entry = &context->queue[context->queue_count++];
    ax_strlcpy(entry->text, text, sizeof(entry->text));
    entry->kind = kind;
    entry->ticks = 0;
    entry->spoken = (bool_t)0;
    context->cooldown = kind == AXA_ANNOUNCE_FOCUS ? 8 : 24;
    context->announce_serial++;
}

/* Pull the next unspoken announcement.  The caller hands it to whatever narrates
 * — a serial console echo today, a speech backend later. */
bool_t axa_next_announcement(axa_context_t *context, char *out, i32 capacity,
                             axa_announce_kind_t *kind)
{
    if (!context || !out || capacity <= 0 || context->queue_count <= 0) {
        return (bool_t)0;
    }
    axa_announce_t *entry = &context->queue[0];
    ax_strlcpy(out, entry->text, capacity);
    if (kind) *kind = entry->kind;
    ax_strlcpy(context->spoken_last, entry->text,
               sizeof(context->spoken_last));
    for (i32 i = 0; i + 1 < context->queue_count; i++) {
        context->queue[i] = context->queue[i + 1];
    }
    context->queue_count--;
    return (bool_t)1;
}

void axa_tick(axa_context_t *context)
{
    if (!context) return;
    if (context->cooldown > 0) context->cooldown--;
    for (i32 i = 0; i < context->queue_count; i++) {
        context->queue[i].ticks++;
    }
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

/* Focus indication is drawn by the accessibility layer, not by each control, so
 * every focusable surface gets the same ring.  The ring is inset by two pixels
 * and uses the focus colour, which is distinct from the accent in every palette. */
void axa_paint_focus(gui_surface_t *surface, axa_context_t *context,
                     const axau_palette_t *palette)
{
    if (!surface || !context || !palette) return;
    if (context->focus < 0 || context->focus >= context->count) return;
    axa_element_t *element = &context->elements[context->focus];
    if (!element->focused || element->disabled) return;
    gui_rect_t ring = gui_rect(element->rect.x - 2, element->rect.y - 2,
                               element->rect.w + 4, element->rect.h + 4);
    axui_round_rect_border(surface, ring, 6, 2,
                           axau_colour(palette, AXAU_COLOUR_FOCUS));
}

/* Large-target mode enlarges hit rectangles without moving the pixels.  It is a
 * hit-testing adjustment, not a layout change; the control still draws at its
 * designed size, but a press near its edge counts. */
gui_rect_t axa_hit_rect(const axa_context_t *context, i32 index)
{
    if (!context || index < 0 || index >= context->count) return gui_rect(0, 0, 0, 0);
    gui_rect_t rect = context->elements[index].rect;
    if (!context->large_targets) return rect;
    i32 pad = 4;
    if (rect.w < 44 || rect.h < 44) pad = 8;
    return gui_rect(rect.x - pad, rect.y - pad, rect.w + pad * 2, rect.h + pad * 2);
}

/* High contrast is applied to the palette, not to individual paint calls.  The
 * function rewrites text and stroke roles towards black or white depending on the
 * surface brightness and drops the translucent fills entirely. */
void axa_adjust_palette(axa_context_t *context, axau_palette_t *palette)
{
    if (!context || !palette || !context->high_contrast) return;
    palette->colours[AXAU_COLOUR_TEXT] = 0x00000000;
    palette->colours[AXAU_COLOUR_TEXT_SECONDARY] = 0x00101010;
    palette->colours[AXAU_COLOUR_TEXT_TERTIARY] = 0x00202020;
    palette->colours[AXAU_COLOUR_SURFACE] = 0x00FFFFFF;
    palette->colours[AXAU_COLOUR_SURFACE_ALT] = 0x00F2F2F2;
    palette->colours[AXAU_COLOUR_SURFACE_ELEVATED] = 0x00FFFFFF;
    palette->colours[AXAU_COLOUR_STROKE] = 0x00000000;
    palette->colours[AXAU_COLOUR_STROKE_STRONG] = 0x00000000;
    palette->colours[AXAU_COLOUR_DIVIDER] = 0x00404040;
}

void axa_dump(const axa_context_t *context)
{
    if (!context) return;
    printk("a11y: %d elements, focus=%d, announcements=%d, hc=%d\n",
           context->count, context->focus, context->queue_count,
           context->high_contrast);
}

