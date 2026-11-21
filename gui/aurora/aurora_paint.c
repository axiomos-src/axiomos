/* AxiomOS - Aurora: sketch canvas application.
 *
 * A vector-ish paint program: the canvas holds a stroke list, each stroke is a
 * polyline with a colour, width and tool.  Shapes (line, rectangle, ellipse,
 * text box) are recorded as strokes too, which keeps undo trivial: removing a
 * stroke removes whatever it drew.  A 16 colour palette and 6 tools complete
 * the model.
 */

#include "gui.h"

#define AXPA_MAX_STROKES   256
#define AXPA_MAX_POINTS    24
#define AXPA_PALETTE       16
#define AXPA_TOOLBAR       52
#define AXPA_RAIL          56
#define AXPA_STATUS        22

#define AXPA_C_BG      GUI_RGB(0x20, 0x22, 0x28)
#define AXPA_C_CANVAS  GUI_RGB(0xF4, 0xF2, 0xEC)
#define AXPA_C_RAIL    GUI_RGB(0x2A, 0x2D, 0x34)
#define AXPA_C_TEXT    GUI_RGB(0xDC, 0xE0, 0xE8)
#define AXPA_C_DIM     GUI_RGB(0x8A, 0x90, 0x9E)
#define AXPA_C_LINE    GUI_RGB(0x3A, 0x3E, 0x48)
#define AXPA_C_SEL     GUI_RGB(0x59, 0xA8, 0xFF)

enum axpa_tool
{
    AXPA_TOOL_PENCIL = 0,
    AXPA_TOOL_LINE,
    AXPA_TOOL_RECT,
    AXPA_TOOL_ELLIPSE,
    AXPA_TOOL_TEXT,
    AXPA_TOOL_ERASER
};

typedef struct axpa_point
{
    i32 x;
    i32 y;
} axpa_point_t;

typedef struct axpa_stroke
{
    axpa_point_t points[AXPA_MAX_POINTS];
    u16          count;
    u8           tool;
    u8           width;
    u32          colour;
    bool_t       filled;
} axpa_stroke_t;

typedef struct axpa_canvas
{
    axpa_stroke_t strokes[AXPA_MAX_STROKES];
    u16           stroke_count;
    u8            tool;
    u8            width;
    u32           colour;
    u8            palette_index;
    bool_t        filled;
    bool_t        drawing;
    axpa_stroke_t pending;
    i32           canvas_width;
    i32           canvas_height;
    i32           zoom;
    u32           undo_count;
    u32           redraws;
    char          label[24];
} axpa_canvas_t;

static const u32 axpa_palette[AXPA_PALETTE] = {
    GUI_RGB(0x1C, 0x1E, 0x24), GUI_RGB(0x4A, 0x4E, 0x5A),
    GUI_RGB(0x8A, 0x90, 0x9E), GUI_RGB(0xF4, 0xF2, 0xEC),
    GUI_RGB(0xC8, 0x3C, 0x3C), GUI_RGB(0xE0, 0x82, 0x40),
    GUI_RGB(0xE0, 0xC0, 0x50), GUI_RGB(0x5C, 0xB8, 0x6A),
    GUI_RGB(0x3C, 0x9A, 0xA8), GUI_RGB(0x3A, 0x72, 0xC8),
    GUI_RGB(0x70, 0x50, 0xB0), GUI_RGB(0xC0, 0x58, 0x94),
    GUI_RGB(0x8C, 0x62, 0x40), GUI_RGB(0x40, 0x70, 0x60),
    GUI_RGB(0xB0, 0xA8, 0x90), GUI_RGB(0x20, 0x30, 0x58)
};

static void axpa_zero(void *ptr, u32 size);
static void axpa_copy(char *dst, const char *src);
static void axpa_begin(axpa_canvas_t *canvas, i32 x, i32 y);
static void axpa_extend(axpa_canvas_t *canvas, i32 x, i32 y);
static void axpa_finish(axpa_canvas_t *canvas);
static void axpa_undo(axpa_canvas_t *canvas);
static void axpa_clear(axpa_canvas_t *canvas);
static void axpa_set_tool(axpa_canvas_t *canvas, u8 tool);
static void axpa_set_colour(axpa_canvas_t *canvas, u8 index);
static void axpa_set_width(axpa_canvas_t *canvas, u8 width);
static void axpa_paint_stroke(gui_surface_t *surface, const axpa_stroke_t *stroke,
                              gui_rect_t canvas);
static void axpa_paint_toolbar(gui_surface_t *surface, const axpa_canvas_t *canvas,
                               gui_rect_t bar);
static void axpa_paint_rail(gui_surface_t *surface, const axpa_canvas_t *canvas,
                            gui_rect_t rail);
static void axpa_paint_status(gui_surface_t *surface, const axpa_canvas_t *canvas,
                              gui_rect_t bar);
static void axpa_paint_ellipse(gui_surface_t *surface, gui_rect_t rect,
                               u32 colour, bool_t filled);
static bool_t axpa_inside(gui_rect_t rect, i32 x, i32 y);

static void axpa_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axpa_copy(char *dst, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i < 23)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool_t axpa_inside(gui_rect_t rect, i32 x, i32 y)
{
    return gui_rect_contains(rect, x, y);
}

static void axpa_begin(axpa_canvas_t *canvas, i32 x, i32 y)
{
    axpa_zero(&canvas->pending, (u32)sizeof(canvas->pending));
    canvas->pending.count = 1;
    canvas->pending.points[0].x = x;
    canvas->pending.points[0].y = y;
    canvas->pending.tool = canvas->tool;
    canvas->pending.width = canvas->width;
    canvas->pending.colour = canvas->colour;
    canvas->pending.filled = canvas->filled;
    canvas->drawing = true;
}

static void axpa_extend(axpa_canvas_t *canvas, i32 x, i32 y)
{
    if (!canvas->drawing)
        return;
    if (canvas->pending.count >= AXPA_MAX_POINTS)
        return;
    canvas->pending.points[canvas->pending.count].x = x;
    canvas->pending.points[canvas->pending.count].y = y;
    canvas->pending.count++;
}

static void axpa_finish(axpa_canvas_t *canvas)
{
    if (!canvas->drawing)
        return;
    canvas->drawing = false;
    if (canvas->pending.count < 1)
        return;
    if (canvas->stroke_count >= AXPA_MAX_STROKES)
        return;
    canvas->strokes[canvas->stroke_count] = canvas->pending;
    canvas->stroke_count++;
    canvas->redraws++;
}

static void axpa_undo(axpa_canvas_t *canvas)
{
    if (canvas->stroke_count == 0)
        return;
    canvas->stroke_count--;
    canvas->undo_count++;
    canvas->redraws++;
}

static void axpa_clear(axpa_canvas_t *canvas)
{
    canvas->stroke_count = 0;
    canvas->drawing = false;
    canvas->redraws++;
}

static void axpa_set_tool(axpa_canvas_t *canvas, u8 tool)
{
    if (tool > AXPA_TOOL_ERASER)
        return;
    canvas->tool = tool;
}

static void axpa_set_colour(axpa_canvas_t *canvas, u8 index)
{
    if (index >= AXPA_PALETTE)
        return;
    canvas->palette_index = index;
    canvas->colour = axpa_palette[index];
}

static void axpa_set_width(axpa_canvas_t *canvas, u8 width)
{
    if (width < 1)
        width = 1;
    if (width > 12)
        width = 12;
    canvas->width = width;
}

static void axpa_paint_ellipse(gui_surface_t *surface, gui_rect_t rect,
                               u32 colour, bool_t filled)
{
    i32 rx = rect.w / 2;
    i32 ry = rect.h / 2;
    i32 cx = rect.x + rx;
    i32 cy = rect.y + ry;
    i32 i;
    i32 j;
    if (rx <= 0 || ry <= 0)
        return;
    for (i = -ry; i <= ry; i++)
    {
        for (j = -rx; j <= rx; j++)
        {
            i64 nx = (i64)j * j * ry * ry;
            i64 ny = (i64)i * i * rx * rx;
            i64 limit = (i64)rx * rx * ry * ry;
            bool_t inside = (bool_t)(nx + ny <= limit);
            bool_t edge = (bool_t)(nx + ny <= limit &&
                                   nx + ny > limit - (i64)(rx * ry * 2));
            if (filled ? inside : edge)
                gui_fill_rect(surface, gui_rect(cx + j, cy + i, 1, 1), colour);
        }
    }
}

static void axpa_paint_stroke(gui_surface_t *surface, const axpa_stroke_t *stroke,
                              gui_rect_t canvas)
{
    u16 i;
    i32 width = stroke->width;
    if (stroke->tool == AXPA_TOOL_PENCIL || stroke->tool == AXPA_TOOL_ERASER)
    {
        for (i = 0; i + 1 < stroke->count; i++)
        {
            i32 x0 = canvas.x + stroke->points[i].x;
            i32 y0 = canvas.y + stroke->points[i].y;
            i32 x1 = canvas.x + stroke->points[i + 1].x;
            i32 y1 = canvas.y + stroke->points[i + 1].y;
            gui_line(surface, x0, y0, x1, y1,
                     stroke->tool == AXPA_TOOL_ERASER ? AXPA_C_CANVAS
                                                      : stroke->colour);
            if (width > 1)
            {
                gui_line(surface, x0, y0 + 1, x1, y1 + 1, stroke->colour);
                gui_line(surface, x0 + 1, y0, x1 + 1, y1, stroke->colour);
            }
        }
        return;
    }
    if (stroke->count < 2)
        return;
    {
        i32 x0 = canvas.x + stroke->points[0].x;
        i32 y0 = canvas.y + stroke->points[0].y;
        i32 x1 = canvas.x + stroke->points[stroke->count - 1].x;
        i32 y1 = canvas.y + stroke->points[stroke->count - 1].y;
        i32 low_x = x0 < x1 ? x0 : x1;
        i32 low_y = y0 < y1 ? y0 : y1;
        i32 w = (x0 < x1 ? x1 - x0 : x0 - x1);
        i32 h = (y0 < y1 ? y1 - y0 : y0 - y1);
        gui_rect_t box = gui_rect(low_x, low_y, w, h);
        switch (stroke->tool)
        {
        case AXPA_TOOL_LINE:
            gui_line(surface, x0, y0, x1, y1, stroke->colour);
            break;
        case AXPA_TOOL_RECT:
            if (stroke->filled)
                gui_fill_rect(surface, box, stroke->colour);
            else
                gui_rect_outline(surface, box, stroke->colour);
            break;
        case AXPA_TOOL_ELLIPSE:
            axpa_paint_ellipse(surface, box, stroke->colour, stroke->filled);
            break;
        case AXPA_TOOL_TEXT:
            gui_text(surface, low_x, low_y, "text", stroke->colour, 2);
            break;
        default:
            break;
        }
    }
}

static void axpa_paint_toolbar(gui_surface_t *surface, const axpa_canvas_t *canvas,
                               gui_rect_t bar)
{
    static const char *const tools[6] = { "pen", "line", "rect", "oval", "text", "erase" };
    i32 x = bar.x + 10;
    u8 i;
    gui_fill_rect(surface, bar, AXPA_C_RAIL);
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y + bar.h - 1, AXPA_C_LINE);
    for (i = 0; i < 6; i++)
    {
        i32 width = gui_text_width(tools[i], 1) + 20;
        gui_rect_t button = gui_rect(x, bar.y + 10, width, bar.h - 20);
        if (i == canvas->tool)
        {
            gui_fill_round_rect(surface, button, 4, AXPA_C_SEL);
        }
        else
        {
            gui_fill_round_rect(surface, button, 4, AXPA_C_RAIL);
            gui_rect_outline(surface, button, AXPA_C_LINE);
        }
        gui_text(surface, x + 10, bar.y + 20, tools[i], AXPA_C_TEXT, 1);
        x += width + 6;
    }
    gui_text(surface, bar.x + bar.w - 120, bar.y + 20, canvas->label, AXPA_C_DIM, 1);
}

static void axpa_paint_rail(gui_surface_t *surface, const axpa_canvas_t *canvas,
                            gui_rect_t rail)
{
    u8 i;
    i32 y = rail.y + 12;
    gui_fill_rect(surface, rail, AXPA_C_RAIL);
    gui_text(surface, rail.x + 12, y, "Colour", AXPA_C_DIM, 1);
    y += 20;
    for (i = 0; i < AXPA_PALETTE; i++)
    {
        i32 column = (i32)(i % 2);
        i32 row = (i32)(i / 2);
        gui_rect_t swatch = gui_rect(rail.x + 10 + column * 22, y + row * 22, 18, 18);
        gui_fill_rect(surface, swatch, axpa_palette[i]);
        if (i == canvas->palette_index)
            gui_rect_outline(surface, gui_rect(swatch.x - 2, swatch.y - 2,
                                               swatch.w + 4, swatch.h + 4), AXPA_C_SEL);
    }
    y += 8 * 22 + 16;
    gui_text(surface, rail.x + 12, y, "Width", AXPA_C_DIM, 1);
    y += 18;
    {
        char label[8];
        label[0] = (char)('0' + canvas->width);
        label[1] = 0;
        gui_fill_rect(surface, gui_rect(rail.x + 10, y, 36, 22), AXPA_C_BG);
        gui_text(surface, rail.x + 22, y + 7, label, AXPA_C_TEXT, 1);
        gui_text(surface, rail.x + 54, y + 7, "+", AXPA_C_DIM, 1);
    }
    y += 32;
    gui_fill_round_rect(surface, gui_rect(rail.x + 10, y, 36, 22), 4,
                        canvas->filled ? AXPA_C_SEL : AXPA_C_LINE);
    gui_text(surface, rail.x + 18, y + 7, "fill", AXPA_C_TEXT, 1);
}

static void axpa_paint_status(gui_surface_t *surface, const axpa_canvas_t *canvas,
                              gui_rect_t bar)
{
    char buffer[24];
    u32 value = canvas->stroke_count;
    u16 digits = 1;
    u16 i;
    gui_fill_rect(surface, bar, AXPA_C_RAIL);
    while (value >= 10)
    {
        value /= 10;
        digits++;
    }
    value = canvas->stroke_count;
    for (i = 0; i < digits; i++)
    {
        buffer[digits - i - 1] = (char)('0' + (value % 10));
        value /= 10;
    }
    buffer[digits] = 0;
    gui_text(surface, bar.x + 12, bar.y + 5, buffer, AXPA_C_DIM, 1);
    gui_text(surface, bar.x + 12 + (i32)digits * 8, bar.y + 5, "strokes",
             AXPA_C_DIM, 1);
}

/* -------------------------------------------------------------- public API */

void axpa_init(axpa_canvas_t *canvas, const char *label)
{
    axpa_zero(canvas, (u32)sizeof(*canvas));
    canvas->tool = AXPA_TOOL_PENCIL;
    canvas->width = 2;
    canvas->palette_index = 0;
    canvas->colour = axpa_palette[0];
    canvas->zoom = 100;
    axpa_copy(canvas->label, label);
}

void axpa_pointer_down(axpa_canvas_t *canvas, i32 x, i32 y, gui_rect_t area)
{
    if (!axpa_inside(area, x, y))
        return;
    axpa_begin(canvas, x - area.x, y - area.y);
}

void axpa_pointer_move(axpa_canvas_t *canvas, i32 x, i32 y, gui_rect_t area)
{
    i32 local_x = x - area.x;
    i32 local_y = y - area.y;
    if (local_x < 0)
        local_x = 0;
    if (local_y < 0)
        local_y = 0;
    if (local_x >= area.w)
        local_x = area.w - 1;
    if (local_y >= area.h)
        local_y = area.h - 1;
    axpa_extend(canvas, local_x, local_y);
}

void axpa_pointer_up(axpa_canvas_t *canvas)
{
    axpa_finish(canvas);
}

void axpa_click_rail(axpa_canvas_t *canvas, i32 x, i32 y, gui_rect_t rail)
{
    u8 i;
    for (i = 0; i < AXPA_PALETTE; i++)
    {
        i32 column = (i32)(i % 2);
        i32 row = (i32)(i / 2);
        gui_rect_t swatch = gui_rect(rail.x + 10 + column * 22,
                                     rail.y + 32 + row * 22, 18, 18);
        if (axpa_inside(swatch, x, y))
        {
            axpa_set_colour(canvas, i);
            return;
        }
    }
    {
        gui_rect_t plus = gui_rect(rail.x + 50, rail.y + 32 + 8 * 22 + 18, 18, 20);
        gui_rect_t minus = gui_rect(rail.x + 10, rail.y + 32 + 8 * 22 + 18, 36, 20);
        gui_rect_t fill = gui_rect(rail.x + 10, rail.y + 32 + 8 * 22 + 50, 36, 22);
        if (axpa_inside(plus, x, y))
            axpa_set_width(canvas, (u8)(canvas->width + 1));
        else if (axpa_inside(minus, x, y))
            axpa_set_width(canvas, (u8)(canvas->width > 1 ? canvas->width - 1 : 1));
        else if (axpa_inside(fill, x, y))
            canvas->filled = (bool_t)!canvas->filled;
    }
}

bool_t axpa_key(axpa_canvas_t *canvas, u16 key, bool_t control)
{
    if (control && (key == 'z' || key == 'Z'))
    {
        axpa_undo(canvas);
        return true;
    }
    if (control && (key == 'c' || key == 'C'))
    {
        axpa_clear(canvas);
        return true;
    }
    if (key >= '1' && key <= '6')
    {
        axpa_set_tool(canvas, (u8)(key - '1'));
        return true;
    }
    return false;
}

void axpa_paint(axpa_canvas_t *canvas, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t toolbar = gui_rect(bounds.x, bounds.y, bounds.w, AXPA_TOOLBAR);
    gui_rect_t rail = gui_rect(bounds.x, toolbar.y + toolbar.h, AXPA_RAIL, bounds.h -
                               AXPA_TOOLBAR - AXPA_STATUS);
    gui_rect_t status = gui_rect(bounds.x, bounds.y + bounds.h - AXPA_STATUS,
                                 bounds.w, AXPA_STATUS);
    gui_rect_t sheet = gui_rect(bounds.x + AXPA_RAIL + 8, toolbar.y + toolbar.h + 8,
                                 bounds.w - AXPA_RAIL - 16,
                                 bounds.h - AXPA_TOOLBAR - AXPA_STATUS - 16);
    u16 i;
    gui_fill_rect(surface, bounds, AXPA_C_BG);
    axpa_paint_toolbar(surface, canvas, toolbar);
    axpa_paint_rail(surface, canvas, rail);
    gui_shadow(surface, sheet, 10, 0x40);
    gui_fill_rect(surface, sheet, AXPA_C_CANVAS);
    canvas->canvas_width = sheet.w;
    canvas->canvas_height = sheet.h;
    for (i = 0; i < canvas->stroke_count; i++)
        axpa_paint_stroke(surface, &canvas->strokes[i], sheet);
    if (canvas->drawing)
        axpa_paint_stroke(surface, &canvas->pending, sheet);
    axpa_paint_status(surface, canvas, status);
}

void axpa_dump(const axpa_canvas_t *canvas)
{
    printk("axpa: '%s' strokes=%u tool=%u width=%u\n", canvas->label,
           (unsigned)canvas->stroke_count, (unsigned)canvas->tool,
           (unsigned)canvas->width);
}

bool_t axpa_selftest(void)
{
    axpa_canvas_t canvas;
    gui_rect_t area = gui_rect(0, 0, 400, 300);
    axpa_init(&canvas, "test");
    axpa_pointer_down(&canvas, 10, 10, area);
    axpa_pointer_move(&canvas, 40, 40, area);
    axpa_pointer_up(&canvas);
    if (canvas.stroke_count != 1)
        return false;
    axpa_set_tool(&canvas, AXPA_TOOL_RECT);
    axpa_pointer_down(&canvas, 20, 20, area);
    axpa_pointer_move(&canvas, 120, 90, area);
    axpa_pointer_up(&canvas);
    if (canvas.stroke_count != 2)
        return false;
    axpa_undo(&canvas);
    if (canvas.stroke_count != 1)
        return false;
    axpa_set_colour(&canvas, 4);
    if (canvas.colour != axpa_palette[4])
        return false;
    axpa_clear(&canvas);
    if (canvas.stroke_count != 0)
        return false;
    return true;
}

