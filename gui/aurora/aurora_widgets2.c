/* AxiomOS - Aurora: extended widget kit.
 *
 * Widgets that live above the primitive controls in aurora_controls.c and are
 * shared by several applications: segmented control, rating stars, numeric
 * stepper, accordion section, toast stack, inline banner, colour picker,
 * key/value list, tag input and a skeleton loader.
 */

#include "gui.h"

#define AXW2_MAX_TAGS       12
#define AXW2_MAX_ROWS       16
#define AXW2_MAX_TOASTS      4
#define AXW2_TAG_LENGTH     24

#define AXW2_C_TEXT     GUI_RGB(0xDC, 0xE0, 0xE8)
#define AXW2_C_DIM      GUI_RGB(0x8A, 0x90, 0x9E)
#define AXW2_C_STROKE   GUI_RGB(0x3C, 0x40, 0x4A)
#define AXW2_C_SURFACE  GUI_RGB(0x26, 0x29, 0x31)
#define AXW2_C_ACCENT   GUI_RGB(0x59, 0xA8, 0xFF)
#define AXW2_C_WARNING  GUI_RGB(0xE0, 0xB8, 0x58)
#define AXW2_C_ERROR    GUI_RGB(0xE0, 0x64, 0x5C)
#define AXW2_C_SUCCESS  GUI_RGB(0x5C, 0xC0, 0x86)

typedef struct axw2_segment
{
    const char *label;
    bool_t      enabled;
} axw2_segment_t;

typedef struct axw2_segmented
{
    axw2_segment_t segments[6];
    u8             count;
    u8             selected;
} axw2_segmented_t;

typedef struct axw2_stepper
{
    i32  value;
    i32  minimum;
    i32  maximum;
    i32  step;
} axw2_stepper_t;

typedef struct axw2_accordion
{
    const char *title[8];
    const char *body[8];
    bool_t      open[8];
    u8          count;
} axw2_accordion_t;

typedef struct axw2_toast
{
    char message[48];
    u8   kind;      /* 0 info, 1 success, 2 warning, 3 error */
    u32  ticks;
    u32  lifetime;
    bool_t active;
} axw2_toast_t;

typedef struct axw2_toast_stack
{
    axw2_toast_t toasts[AXW2_MAX_TOASTS];
    u8           count;
} axw2_toast_stack_t;

typedef struct axw2_tag_input
{
    char tags[AXW2_MAX_TAGS][AXW2_TAG_LENGTH];
    u8   count;
    char draft[AXW2_TAG_LENGTH];
    u16  draft_length;
} axw2_tag_input_t;

typedef struct axw2_kv
{
    char key[24];
    char value[32];
} axw2_kv_t;

typedef struct axw2_kv_list
{
    axw2_kv_t rows[AXW2_MAX_ROWS];
    u8        count;
} axw2_kv_list_t;

static void axw2_zero(void *ptr, u32 size);
static void axw2_copy(char *dst, u32 capacity, const char *src);
static void axw2_paint_segmented(gui_surface_t *surface, const axw2_segmented_t *seg,
                                 gui_rect_t rect);
static void axw2_paint_rating(gui_surface_t *surface, i32 x, i32 y, u8 value);
static void axw2_paint_stepper(gui_surface_t *surface, const axw2_stepper_t *stepper,
                               gui_rect_t rect);
static void axw2_paint_accordion(gui_surface_t *surface, const axw2_accordion_t *acc,
                                 gui_rect_t rect);
static void axw2_paint_toasts(gui_surface_t *surface, const axw2_toast_stack_t *stack,
                              gui_rect_t area);
static void axw2_paint_banner(gui_surface_t *surface, const char *text, u8 kind,
                              gui_rect_t rect);
static void axw2_paint_colour_picker(gui_surface_t *surface, gui_rect_t rect,
                                     u8 selected);
static void axw2_paint_tags(gui_surface_t *surface, const axw2_tag_input_t *tags,
                            gui_rect_t rect);
static void axw2_paint_kv(gui_surface_t *surface, const axw2_kv_list_t *list,
                          gui_rect_t rect);
static void axw2_paint_skeleton(gui_surface_t *surface, gui_rect_t rect,
                                u32 phase);
static u32  axw2_kind_colour(u8 kind);

static const u32 axw2_palette[12] = {
    GUI_RGB(0xC8, 0x3C, 0x3C), GUI_RGB(0xE0, 0x82, 0x40), GUI_RGB(0xE0, 0xC0, 0x50),
    GUI_RGB(0x5C, 0xB8, 0x6A), GUI_RGB(0x3C, 0x9A, 0xA8), GUI_RGB(0x3A, 0x72, 0xC8),
    GUI_RGB(0x70, 0x50, 0xB0), GUI_RGB(0xC0, 0x58, 0x94), GUI_RGB(0x8C, 0x62, 0x40),
    GUI_RGB(0x40, 0x70, 0x60), GUI_RGB(0xB0, 0xA8, 0x90), GUI_RGB(0x20, 0x30, 0x58)
};

static void axw2_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axw2_copy(char *dst, u32 capacity, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < capacity)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static u32 axw2_kind_colour(u8 kind)
{
    switch (kind)
    {
    case 1: return AXW2_C_SUCCESS;
    case 2: return AXW2_C_WARNING;
    case 3: return AXW2_C_ERROR;
    default: return AXW2_C_ACCENT;
    }
}

static void axw2_paint_segmented(gui_surface_t *surface, const axw2_segmented_t *seg,
                                  gui_rect_t rect)
{
    u8 i;
    i32 width = rect.w / (seg->count > 0 ? seg->count : 1);
    gui_fill_round_rect(surface, rect, 6, AXW2_C_SURFACE);
    for (i = 0; i < seg->count; i++)
    {
        gui_rect_t cell = gui_rect(rect.x + (i32)i * width, rect.y, width, rect.h);
        if (i == seg->selected)
            gui_fill_round_rect(surface, cell, 5, AXW2_C_ACCENT);
        gui_text(surface, cell.x + cell.w / 2 -
                 gui_text_width(seg->segments[i].label, 1) / 2,
                 cell.y + cell.h / 2 - 8, seg->segments[i].label,
                 i == seg->selected ? GUI_RGB(0x0E, 0x16, 0x22) : AXW2_C_TEXT, 1);
    }
}

static void axw2_paint_rating(gui_surface_t *surface, i32 x, i32 y, u8 value)
{
    u8 i;
    for (i = 0; i < 5; i++)
    {
        gui_fill_rect(surface, gui_rect(x + (i32)i * 12, y, 9, 9),
                      i < value ? AXW2_C_WARNING : AXW2_C_STROKE);
        gui_fill_rect(surface, gui_rect(x + (i32)i * 12 + 2, y - 3, 5, 3),
                      i < value ? AXW2_C_WARNING : AXW2_C_STROKE);
    }
}

static void axw2_paint_stepper(gui_surface_t *surface, const axw2_stepper_t *stepper,
                               gui_rect_t rect)
{
    char buffer[16];
    i32 value = stepper->value;
    i32 digits = 1;
    i32 i;
    i32 tmp = value < 0 ? -value : value;
    while (tmp >= 10)
    {
        tmp /= 10;
        digits++;
    }
    gui_fill_round_rect(surface, rect, 4, AXW2_C_SURFACE);
    gui_text(surface, rect.x + 10, rect.y + rect.h / 2 - 6, "-", AXW2_C_TEXT, 2);
    tmp = value < 0 ? -value : value;
    for (i = 0; i < digits; i++)
    {
        buffer[digits - i - 1] = (char)('0' + (tmp % 10));
        tmp /= 10;
    }
    buffer[digits] = 0;
    gui_text(surface, rect.x + rect.w / 2 - digits * 4, rect.y + rect.h / 2 - 8,
             buffer, AXW2_C_TEXT, 2);
    gui_text(surface, rect.x + rect.w - 18, rect.y + rect.h / 2 - 6, "+",
             AXW2_C_TEXT, 2);
}

static void axw2_paint_accordion(gui_surface_t *surface, const axw2_accordion_t *acc,
                                 gui_rect_t rect)
{
    u8 i;
    i32 y = rect.y;
    for (i = 0; i < acc->count; i++)
    {
        gui_rect_t head = gui_rect(rect.x, y, rect.w, 32);
        gui_fill_rect(surface, head, AXW2_C_SURFACE);
        gui_text(surface, rect.x + 12, y + 8, acc->title[i], AXW2_C_TEXT, 1);
        gui_text(surface, rect.x + rect.w - 20, y + 8,
                 acc->open[i] ? "-" : "+", AXW2_C_DIM, 1);
        y += 32;
        if (acc->open[i] && acc->body[i] != NULL_PTR)
        {
            gui_text(surface, rect.x + 20, y + 6, acc->body[i], AXW2_C_DIM, 1);
            y += 34;
        }
        gui_hline(surface, rect.x, rect.x + rect.w, y, AXW2_C_STROKE);
    }
}

static void axw2_paint_toasts(gui_surface_t *surface, const axw2_toast_stack_t *stack,
                              gui_rect_t area)
{
    u8 i;
    i32 y = area.y;
    for (i = 0; i < stack->count; i++)
    {
        const axw2_toast_t *toast = &stack->toasts[i];
        if (!toast->active)
            continue;
        gui_rect_t card = gui_rect(area.x, y, area.w, 40);
        gui_fill_round_rect(surface, card, 6, AXW2_C_SURFACE);
        gui_fill_rect(surface, gui_rect(card.x, card.y + 6, 3, 28),
                      axw2_kind_colour(toast->kind));
        gui_text(surface, card.x + 14, card.y + 12, toast->message, AXW2_C_TEXT, 1);
        gui_text(surface, card.x + card.w - 16, card.y + 12, "x", AXW2_C_DIM, 1);
        if (toast->lifetime > 0)
        {
            i32 progress = (i32)(toast->ticks * 100 / toast->lifetime);
            gui_fill_rect(surface, gui_rect(card.x + 14, card.y + 32,
                                            (card.w - 30) * progress / 100, 2),
                          axw2_kind_colour(toast->kind));
        }
        y += 46;
    }
}

static void axw2_paint_banner(gui_surface_t *surface, const char *text, u8 kind,
                              gui_rect_t rect)
{
    gui_fill_round_rect(surface, rect, 6, AXW2_C_SURFACE);
    gui_fill_rect(surface, gui_rect(rect.x, rect.y + 6, 4, rect.h - 12),
                  axw2_kind_colour(kind));
    gui_text(surface, rect.x + 14, rect.y + rect.h / 2 - 6, text, AXW2_C_TEXT, 1);
}

static void axw2_paint_colour_picker(gui_surface_t *surface, gui_rect_t rect,
                                     u8 selected)
{
    u8 i;
    i32 swatch = 24;
    i32 columns = rect.w / (swatch + 6);
    gui_fill_rect(surface, rect, AXW2_C_SURFACE);
    for (i = 0; i < 12; i++)
    {
        i32 column = i % (u8)(columns > 0 ? columns : 1);
        i32 row = i / (u8)(columns > 0 ? columns : 1);
        gui_rect_t cell = gui_rect(rect.x + 6 + column * (swatch + 6),
                                   rect.y + 6 + row * (swatch + 6), swatch, swatch);
        gui_fill_rect(surface, cell, axw2_palette[i]);
        if (i == selected)
            gui_rect_outline(surface, gui_rect(cell.x - 2, cell.y - 2,
                                               cell.w + 4, cell.h + 4), AXW2_C_ACCENT);
    }
}

static void axw2_paint_tags(gui_surface_t *surface, const axw2_tag_input_t *tags,
                            gui_rect_t rect)
{
    u8 i;
    i32 x = rect.x + 4;
    gui_fill_rect(surface, rect, AXW2_C_SURFACE);
    for (i = 0; i < tags->count; i++)
    {
        i32 width = gui_text_width(tags->tags[i], 1) + 18;
        gui_rect_t chip = gui_rect(x, rect.y + 4, width, rect.h - 8);
        gui_fill_round_rect(surface, chip, 9, AXW2_C_STROKE);
        gui_text(surface, x + 8, rect.y + 8, tags->tags[i], AXW2_C_TEXT, 1);
        x += width + 4;
    }
    if (tags->draft_length > 0)
        gui_text(surface, x + 4, rect.y + 8, tags->draft, AXW2_C_DIM, 1);
    gui_text(surface, x + 4 + gui_text_width(tags->draft, 1), rect.y + 8, "_",
             AXW2_C_ACCENT, 1);
}

static void axw2_paint_kv(gui_surface_t *surface, const axw2_kv_list_t *list,
                          gui_rect_t rect)
{
    u8 i;
    i32 y = rect.y;
    for (i = 0; i < list->count; i++)
    {
        gui_text(surface, rect.x, y, list->rows[i].key, AXW2_C_DIM, 1);
        gui_text(surface, rect.x + rect.w / 2, y, list->rows[i].value,
                 AXW2_C_TEXT, 1);
        gui_hline(surface, rect.x, rect.x + rect.w, y + 20, AXW2_C_STROKE);
        y += 24;
    }
}

static void axw2_paint_skeleton(gui_surface_t *surface, gui_rect_t rect, u32 phase)
{
    u32 shade = 0x20 + (phase % 6) * 8;
    gui_fill_rect(surface, rect, GUI_ARGB(0xFF, shade, shade + 2, shade + 6));
    {
        i32 highlight = (i32)((phase * 12) % (rect.w + 40)) - 20;
        gui_fill_rect(surface, gui_rect(rect.x + highlight, rect.y, 24, rect.h),
                      AXW2_C_STROKE);
    }
}

/* -------------------------------------------------------------- public API */

void axw2_init_segmented(axw2_segmented_t *seg, const char **labels, u8 count)
{
    axw2_zero(seg, (u32)sizeof(*seg));
    seg->count = count > 6 ? 6 : count;
    {
        u8 i;
        for (i = 0; i < seg->count; i++)
        {
            seg->segments[i].label = labels[i];
            seg->segments[i].enabled = true;
        }
    }
}

u8 axw2_click_segmented(axw2_segmented_t *seg, i32 x, gui_rect_t rect)
{
    i32 width = rect.w / (seg->count > 0 ? seg->count : 1);
    i32 index = (x - rect.x) / (width > 0 ? width : 1);
    if (index < 0)
        index = 0;
    if (index >= (i32)seg->count)
        index = (i32)seg->count - 1;
    seg->selected = (u8)index;
    return seg->selected;
}

void axw2_init_stepper(axw2_stepper_t *stepper, i32 value, i32 minimum, i32 maximum,
                       i32 step)
{
    stepper->value = value;
    stepper->minimum = minimum;
    stepper->maximum = maximum;
    stepper->step = step > 0 ? step : 1;
}

i32 axw2_stepper_bump(axw2_stepper_t *stepper, bool_t up)
{
    stepper->value += up ? stepper->step : -stepper->step;
    if (stepper->value < stepper->minimum)
        stepper->value = stepper->minimum;
    if (stepper->value > stepper->maximum)
        stepper->value = stepper->maximum;
    return stepper->value;
}

void axw2_init_accordion(axw2_accordion_t *acc)
{
    axw2_zero(acc, (u32)sizeof(*acc));
}

void axw2_add_section(axw2_accordion_t *acc, const char *title, const char *body)
{
    if (acc->count >= 8)
        return;
    acc->title[acc->count] = title;
    acc->body[acc->count] = body;
    acc->open[acc->count] = false;
    acc->count++;
}

bool_t axw2_toggle_section(axw2_accordion_t *acc, u8 index)
{
    if (index >= acc->count)
        return false;
    acc->open[index] = (bool_t)!acc->open[index];
    return acc->open[index];
}

void axw2_push_toast(axw2_toast_stack_t *stack, const char *message, u8 kind,
                     u32 lifetime)
{
    axw2_toast_t *toast;
    if (stack->count >= AXW2_MAX_TOASTS)
    {
        u8 i;
        for (i = 0; i + 1 < AXW2_MAX_TOASTS; i++)
            stack->toasts[i] = stack->toasts[i + 1];
        stack->count = AXW2_MAX_TOASTS - 1;
    }
    toast = &stack->toasts[stack->count];
    axw2_zero(toast, (u32)sizeof(*toast));
    axw2_copy(toast->message, 48, message);
    toast->kind = kind;
    toast->active = true;
    toast->ticks = 0;
    toast->lifetime = lifetime;
    stack->count++;
}

void axw2_tick_toasts(axw2_toast_stack_t *stack)
{
    u8 i;
    for (i = 0; i < stack->count; i++)
    {
        axw2_toast_t *toast = &stack->toasts[i];
        if (!toast->active)
            continue;
        toast->ticks++;
        if (toast->lifetime > 0 && toast->ticks >= toast->lifetime)
            toast->active = false;
    }
}

void axw2_init_tags(axw2_tag_input_t *tags)
{
    axw2_zero(tags, (u32)sizeof(*tags));
}

bool_t axw2_tag_type(axw2_tag_input_t *tags, char c)
{
    if (c == '\n' || c == ' ')
    {
        if (tags->draft_length == 0)
            return false;
        if (tags->count >= AXW2_MAX_TAGS)
            return false;
        tags->draft[tags->draft_length] = 0;
        axw2_copy(tags->tags[tags->count], AXW2_TAG_LENGTH, tags->draft);
        tags->count++;
        tags->draft_length = 0;
        tags->draft[0] = 0;
        return true;
    }
    if (tags->draft_length + 1 >= AXW2_TAG_LENGTH)
        return false;
    tags->draft[tags->draft_length] = c;
    tags->draft_length++;
    tags->draft[tags->draft_length] = 0;
    return true;
}

bool_t axw2_tag_backspace(axw2_tag_input_t *tags)
{
    if (tags->draft_length > 0)
    {
        tags->draft_length--;
        tags->draft[tags->draft_length] = 0;
        return true;
    }
    if (tags->count > 0)
    {
        tags->count--;
        return true;
    }
    return false;
}

void axw2_init_kv(axw2_kv_list_t *list)
{
    axw2_zero(list, (u32)sizeof(*list));
}

void axw2_kv_add(axw2_kv_list_t *list, const char *key, const char *value)
{
    axw2_kv_t *row;
    if (list->count >= AXW2_MAX_ROWS)
        return;
    row = &list->rows[list->count];
    axw2_copy(row->key, 24, key);
    axw2_copy(row->value, 32, value);
    list->count++;
}

void axw2_paint_all(gui_surface_t *surface, gui_rect_t bounds)
{
    const char *labels[3] = { "Day", "Week", "Month" };
    axw2_segmented_t seg;
    axw2_stepper_t stepper;
    axw2_kv_list_t kv;
    axw2_accordion_t acc;
    axw2_tag_input_t tags;
    gui_rect_t rect = gui_rect(bounds.x + 16, bounds.y + 16, 240, 30);
    axw2_init_segmented(&seg, labels, 3);
    axw2_paint_segmented(surface, &seg, rect);
    axw2_paint_rating(surface, bounds.x + 16, bounds.y + 70, 4);
    axw2_init_stepper(&stepper, 12, 0, 99, 1);
    axw2_paint_stepper(surface, &stepper,
                       gui_rect(bounds.x + 290, bounds.y + 16, 120, 30));
    axw2_init_accordion(&acc);
    axw2_add_section(&acc, "Section one", "Body copy for the first section.");
    axw2_add_section(&acc, "Section two", "Body copy for the second section.");
    acc.open[0] = true;
    axw2_paint_accordion(surface, &acc,
                         gui_rect(bounds.x + 16, bounds.y + 110, 300, 140));
    axw2_paint_banner(surface, "Only local builds are shown", 2,
                      gui_rect(bounds.x + 16, bounds.y + 270, 360, 40));
    axw2_paint_colour_picker(surface,
                             gui_rect(bounds.x + 16, bounds.y + 330, 180, 90), 3);
    axw2_init_tags(&tags);
    axw2_tag_type(&tags, 'k');
    axw2_tag_type(&tags, 'e');
    axw2_tag_type(&tags, '\n');
    axw2_paint_tags(surface, &tags, gui_rect(bounds.x + 16, bounds.y + 440, 300, 30));
    axw2_init_kv(&kv);
    axw2_kv_add(&kv, "Build", "0.9.4");
    axw2_kv_add(&kv, "Arch", "x86_64");
    axw2_paint_kv(surface, &kv, gui_rect(bounds.x + 16, bounds.y + 490, 300, 60));
    axw2_paint_skeleton(surface, gui_rect(bounds.x + 16, bounds.y + 560, 300, 16), 2);
}

bool_t axw2_selftest(void)
{
    axw2_segmented_t seg;
    axw2_stepper_t stepper;
    axw2_toast_stack_t stack;
    axw2_tag_input_t tags;
    const char *labels[2] = { "A", "B" };
    axw2_init_segmented(&seg, labels, 2);
    if (seg.count != 2)
        return false;
    axw2_init_stepper(&stepper, 5, 0, 10, 2);
    axw2_stepper_bump(&stepper, true);
    if (stepper.value != 7)
        return false;
    axw2_stepper_bump(&stepper, true);
    if (stepper.value != 9)
        return false;
    axw2_stepper_bump(&stepper, true);
    if (stepper.value != 10)
        return false;
    axw2_push_toast(&stack, "hello", 1, 100);
    if (stack.count != 1)
        return false;
    axw2_tick_toasts(&stack);
    axw2_init_tags(&tags);
    axw2_tag_type(&tags, 'a');
    axw2_tag_type(&tags, '\n');
    if (tags.count != 1)
        return false;
    axw2_tag_backspace(&tags);
    if (tags.count != 0)
        return false;
    return true;
}
