/* AxiomOS UI - collection views.
 *
 * List box, table, tab strip and pop-up menu.  These are the widgets that
 * carry the most state, and they all share three behaviours:
 *
 *   - a hit test maps a pointer position to a row, honouring the scroll offset;
 *   - a keyboard path offers the same navigation without a pointer;
 *   - a scrollbar can be attached by the caller, so the widget only reports
 *     its content height and current offset.
 *
 * The list is fixed-capacity (AXW_LIST_MAX rows) because the shell's file
 * browser and settings pages never exceed it, and a static array keeps the
 * kernel heap out of the widget layer.
 */

#include <axiom/lib.h>
#include "widgets.h"

/* ------------------------------------------------------------------ */
/* List box                                                            */
/* ------------------------------------------------------------------ */

void axw_list_init(axw_list_t *list, gui_rect_t rect, i32 row_height)
{
    ax_memset(list, 0, sizeof(*list));
    list->rect = rect;
    list->row_height = row_height > 16 ? row_height : 24;
    list->selected = -1;
    list->hover = -1;
}

i32 axw_list_add(axw_list_t *list, const char *label, const char *detail,
                 const char *glyph)
{
    if (!list || list->count >= AXW_LIST_MAX) {
        return -1;
    }
    i32 index = list->count++;
    list->items[index].label = label;
    list->items[index].detail = detail;
    list->items[index].glyph = glyph;
    list->items[index].disabled = (bool_t)0;
    list->items[index].checked = (bool_t)0;
    return index;
}

void axw_list_set_detail(axw_list_t *list, bool_t show)
{
    if (list) {
        list->show_detail = show;
    }
}

i32 axw_list_visible_rows(const axw_list_t *list)
{
    if (!list) {
        return 0;
    }
    return list->rect.h / list->row_height;
}

i32 axw_list_content_height(const axw_list_t *list)
{
    if (!list) {
        return 0;
    }
    return list->count * list->row_height;
}

void axw_list_scroll_to(axw_list_t *list, i32 index)
{
    if (!list) {
        return;
    }
    i32 visible = axw_list_visible_rows(list);
    if (visible <= 0) {
        return;
    }
    i32 top_row = list->scroll / list->row_height;
    if (index < top_row) {
        list->scroll = index * list->row_height;
    } else if (index >= top_row + visible) {
        list->scroll = (index - visible + 1) * list->row_height;
    }
    i32 max_scroll = axw_list_content_height(list) - list->rect.h;
    if (list->scroll > max_scroll) {
        list->scroll = max_scroll > 0 ? max_scroll : 0;
    }
    if (list->scroll < 0) {
        list->scroll = 0;
    }
}

void axw_list_paint(gui_surface_t *surface, axw_list_t *list)
{
    if (!surface || !list) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    gui_fill_rect(surface, list->rect, theme->window_bg);

    i32 visible = axw_list_visible_rows(list);
    for (i32 i = 0; i < visible; i++) {
        i32 index = (i32)(i + list->scroll / list->row_height);
        if (index >= list->count) {
            break;
        }
        axw_list_item_t *item = &list->items[index];
        gui_rect_t row = gui_rect(list->rect.x, list->rect.y + i * list->row_height,
                                  list->rect.w - 8, list->row_height);

        /* Row background: selected, hovered, or alternating. */
        if (index == list->selected) {
            gui_fill_rect(surface, row, axui_colour_alpha(theme->selection, 220));
        } else if (index == list->hover) {
            gui_fill_rect(surface, row, axui_hover_tint(theme->window_bg));
        } else if ((index & 1) && list->row_height > 20) {
            gui_fill_rect(surface, row, axui_tint(theme->window_bg_alt,
                                                  theme->window_bg, 40));
        }

        u32 text_colour = item->disabled ? axui_disabled_text() : theme->text;
        if (index == list->selected) {
            text_colour = theme->text;
        }

        i32 pen = row.x + 10;
        if (item->glyph) {
            axui_text_style_t glyph_style = axui_style(theme->accent, 1);
            axui_draw_text(surface, pen, axui_text_baseline(row, 1), item->glyph,
                           glyph_style);
            pen += 20;
        }
        if (item->checked) {
            axui_text_style_t check_style = axui_style(theme->success, 1);
            axui_draw_text(surface, pen, axui_text_baseline(row, 1), "v",
                           check_style);
            pen += 16;
        }

        if (list->show_detail && item->detail) {
            /* Two-line row: label above the detail, both clipped. */
            gui_rect_t label_area = gui_rect(pen, row.y + 2, row.w - pen, row.h / 2);
            gui_rect_t detail_area = gui_rect(pen, row.y + row.h / 2,
                                              row.w - pen, row.h / 2);
            axui_text_style_t label_style = axui_style(text_colour, 1);
            axui_draw_text_clip(surface, label_area, pen, label_area.y, item->label,
                                label_style);
            axui_text_style_t detail_style = axui_style(theme->text_dim, 1);
            axui_draw_text_ellipsis(surface, detail_area, item->detail, detail_style);
        } else {
            axui_text_style_t label_style = axui_style(text_colour, 1);
            axui_draw_text_clip(surface, row, pen, axui_text_baseline(row, 1),
                                item->label, label_style);
        }

        /* A hairline closes each row on the taller layouts. */
        if (list->row_height > 28) {
            gui_fill_rect(surface, gui_rect(row.x, row.y + row.h - 1, row.w, 1),
                          axui_hairline(theme->window_bg));
        }
    }

    /* Scroll marker on the right edge when the content overflows. */
    if (axw_list_content_height(list) > list->rect.h) {
        i32 track_h = list->rect.h;
        i32 content = axw_list_content_height(list);
        i32 thumb_h = (track_h * list->rect.h) / content;
        if (thumb_h < 16) {
            thumb_h = 16;
        }
        i32 max_scroll = content - list->rect.h;
        i32 position = max_scroll > 0
                           ? (list->scroll * (track_h - thumb_h)) / max_scroll
                           : 0;
        gui_fill_rect(surface,
                      gui_rect(list->rect.x + list->rect.w - 3,
                               list->rect.y + position, 3, thumb_h),
                      axui_colour_alpha(theme->accent, 140));
    }

    if (list->focused) {
        axui_round_rect_border(surface, list->rect, 4, 1, theme->accent);
    }
}

u32 axw_list_event(axw_list_t *list, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!list) {
        return AXW_NONE;
    }
    bool_t inside = axw_hit(list->rect, x, y);
    i32 row = inside ? axw_hit_row(list->rect, y, list->row_height, list->scroll) : -1;
    if (row >= list->count) {
        row = -1;
    }
    list->hover = row;

    if (pressed && row >= 0) {
        bool_t changed = (bool_t)(row != list->selected);
        list->selected = row;
        axw_list_scroll_to(list, row);
        return AXW_PRESS | AXW_CLICK | AXW_SELECT | (changed ? AXW_CHANGED : 0);
    }
    if (!pressed) {
        return AXW_NONE;
    }
    (void)buttons;
    return AXW_NONE;
}

u32 axw_list_key(axw_list_t *list, char key, u8 scancode)
{
    if (!list || list->count == 0) {
        return AXW_NONE;
    }
    i32 old = list->selected;
    switch (scancode) {
    case 0x48:      /* up */
    case 0x50:      /* down */
        if (list->selected < 0) {
            list->selected = 0;
        } else {
            list->selected += scancode == 0x48 ? -1 : 1;
        }
        break;
    case 0x49:      /* page up */
        list->selected -= axw_list_visible_rows(list);
        break;
    case 0x51:      /* page down */
        list->selected += axw_list_visible_rows(list);
        break;
    case 0x47:      /* home */
        list->selected = 0;
        break;
    case 0x4F:      /* end */
        list->selected = list->count - 1;
        break;
    default:
        if (key == '\n' || key == '\r') {
            return AXW_CLICK | AXW_SELECT;
        }
        return AXW_NONE;
    }
    list->selected = axui_clamp_i32(list->selected, 0, list->count - 1);
    axw_list_scroll_to(list, list->selected);
    return list->selected != old ? (AXW_SELECT | AXW_CHANGED) : AXW_NONE;
}

/* ------------------------------------------------------------------ */
/* Table                                                               */
/* ------------------------------------------------------------------ */

void axw_table_init(axw_table_t *table, gui_rect_t rect, i32 columns, i32 row_height)
{
    ax_memset(table, 0, sizeof(*table));
    table->rect = rect;
    table->columns = columns > 0 && columns <= AXW_TABLE_COLS ? columns : 1;
    table->row_height = row_height > 16 ? row_height : 22;
    table->selected = -1;
    table->striped = (bool_t)1;
}

void axw_table_header(axw_table_t *table, i32 column, const char *text, i32 width)
{
    if (!table || column < 0 || column >= table->columns) {
        return;
    }
    table->headers[column] = text;
    table->column_width[column] = width;
}

void axw_table_cell(axw_table_t *table, i32 row, i32 column, const char *text)
{
    if (!table || row < 0 || column < 0 || column >= table->columns) {
        return;
    }
    if (row >= AXW_LIST_MAX) {
        return;
    }
    table->cells[column][row] = text;
    if (row >= table->rows) {
        table->rows = row + 1;
    }
}

/* Header height, plus the sticky offset the body starts at. */
static i32 table_body_y(const axw_table_t *table)
{
    return table->rect.y + table->row_height;
}

void axw_table_paint(gui_surface_t *surface, axw_table_t *table)
{
    if (!surface || !table) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    gui_fill_rect(surface, table->rect, theme->window_bg);

    /* Header strip. */
    gui_rect_t header = gui_rect(table->rect.x, table->rect.y, table->rect.w,
                                 table->row_height);
    gui_fill_rect(surface, header, theme->window_bg_alt);
    axui_text_style_t head_style = axui_style(theme->text_dim, 1);
    i32 pen = header.x;
    for (i32 c = 0; c < table->columns; c++) {
        i32 width = table->column_width[c] > 0 ? table->column_width[c]
                                               : header.w / table->columns;
        if (table->headers[c]) {
            axui_draw_text_clip(surface, gui_rect(pen, header.y, width, header.h),
                                pen + 8, axui_text_baseline(header, 1),
                                table->headers[c], head_style);
        }
        pen += width;
        gui_fill_rect(surface, gui_rect(pen - 1, header.y, 1, header.h),
                      axui_hairline(theme->window_bg_alt));
    }
    gui_fill_rect(surface, gui_rect(header.x, header.y + header.h - 1, header.w, 1),
                  theme->border);

    i32 body_y = table_body_y(table);
    i32 visible = (table->rect.y + table->rect.h - body_y) / table->row_height;
    i32 first = table->scroll / table->row_height;

    for (i32 i = 0; i < visible; i++) {
        i32 row = first + i;
        if (row >= table->rows) {
            break;
        }
        gui_rect_t row_rect = gui_rect(table->rect.x, body_y + i * table->row_height,
                                       table->rect.w, table->row_height);
        if (row == table->selected) {
            gui_fill_rect(surface, row_rect, axui_colour_alpha(theme->selection, 220));
        } else if (table->striped && (row & 1)) {
            gui_fill_rect(surface, row_rect, axui_tint(theme->window_bg_alt,
                                                       theme->window_bg, 45));
        }

        i32 pen = row_rect.x;
        for (i32 c = 0; c < table->columns; c++) {
            i32 width = table->column_width[c] > 0 ? table->column_width[c]
                                                   : row_rect.w / table->columns;
            const char *text = table->cells[c][row];
            if (text) {
                gui_rect_t cell = gui_rect(pen, row_rect.y, width, row_rect.h);
                axui_text_style_t style = axui_style(theme->text, 1);
                axui_draw_text_ellipsis(surface, cell, text, style);
            }
            pen += width;
        }
    }

    if (table->rows == 0) {
        axw_empty_state_paint(surface,
                              gui_rect(table->rect.x, body_y, table->rect.w,
                                       table->rect.h - table->row_height),
                              "-", "no rows", "the table is empty");
    }

    if (table->focused) {
        axui_round_rect_border(surface, table->rect, 4, 1, theme->accent);
    }
}

u32 axw_table_event(axw_table_t *table, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!table) {
        return AXW_NONE;
    }
    if (pressed && axw_hit(table->rect, x, y)) {
        i32 body_y = table_body_y(table);
        if (y < body_y) {
            /* A header click sorts in a real table; here it reports the column
             * so the caller can implement its own ordering. */
            i32 column = axw_hit_column(table->rect, x, table->column_width,
                                        table->columns);
            return AXW_CLICK | (u32)(column >= 0 ? (column << 16) : 0);
        }
        i32 row = table->scroll / table->row_height + (y - body_y) / table->row_height;
        if (row >= 0 && row < table->rows) {
            bool_t changed = (bool_t)(row != table->selected);
            table->selected = row;
            return AXW_CLICK | AXW_SELECT | (changed ? AXW_CHANGED : 0);
        }
    }
    (void)buttons;
    return AXW_NONE;
}

/* ------------------------------------------------------------------ */
/* Tabs                                                                */
/* ------------------------------------------------------------------ */

void axw_tabs_init(axw_tabs_t *tabs, gui_rect_t rect, i32 tab_width)
{
    ax_memset(tabs, 0, sizeof(*tabs));
    tabs->rect = rect;
    tabs->tab_width = tab_width > 40 ? tab_width : 96;
    tabs->hover = -1;
    tabs->underline = (bool_t)1;
}

i32 axw_tabs_add(axw_tabs_t *tabs, const char *label)
{
    if (!tabs || tabs->count >= 8) {
        return -1;
    }
    tabs->labels[tabs->count] = label;
    return tabs->count++;
}

void axw_tabs_paint(gui_surface_t *surface, const axw_tabs_t *tabs)
{
    if (!surface || !tabs) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();

    gui_fill_rect(surface, gui_rect(tabs->rect.x, tabs->rect.y, tabs->rect.w, 1),
                  axui_hairline(theme->window_bg));

    for (i32 i = 0; i < tabs->count; i++) {
        gui_rect_t tab = gui_rect(tabs->rect.x + i * tabs->tab_width, tabs->rect.y,
                                  tabs->tab_width, tabs->rect.h);
        bool_t active = (bool_t)(i == tabs->selected);
        u32 fill = active ? theme->window_bg_alt
                          : i == tabs->hover ? axui_hover_tint(theme->window_bg)
                                             : theme->window_bg;
        axui_round_rect_aa(surface, gui_rect(tab.x + 2, tab.y + 2, tab.w - 4,
                                             tab.h - 2), 6, fill);

        axui_text_style_t style = axui_style(active ? theme->text : theme->text_dim, 1);
        style.bold = active;
        axui_draw_text_center(surface, tab, tabs->labels[i], style);

        if (active && tabs->underline) {
            gui_fill_rect(surface, gui_rect(tab.x + 8, tab.y + tab.h - 2,
                                            tab.w - 16, 2), theme->accent);
        }
    }
}

u32 axw_tabs_event(axw_tabs_t *tabs, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!tabs) {
        return AXW_NONE;
    }
    i32 index = -1;
    if (x >= tabs->rect.x && y >= tabs->rect.y && y < tabs->rect.y + tabs->rect.h) {
        index = (x - tabs->rect.x) / tabs->tab_width;
        if (index >= tabs->count) {
            index = -1;
        }
    }
    tabs->hover = index;

    if (pressed && index >= 0) {
        bool_t changed = (bool_t)(index != tabs->selected);
        tabs->selected = index;
        return AXW_CLICK | AXW_SELECT | (changed ? AXW_CHANGED : 0);
    }
    (void)buttons;
    return AXW_NONE;
}

/* The content area below the tab strip, inset by one pixel. */
gui_rect_t axw_tabs_content(const axw_tabs_t *tabs)
{
    if (!tabs) {
        return gui_rect(0, 0, 0, 0);
    }
    return gui_rect(tabs->rect.x, tabs->rect.y + tabs->rect.h, tabs->rect.w,
                    tabs->rect.h > 0 ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* Menu                                                                */
/* ------------------------------------------------------------------ */

void axw_menu_init(axw_menu_t *menu, gui_rect_t rect, i32 item_height)
{
    ax_memset(menu, 0, sizeof(*menu));
    menu->rect = rect;
    menu->item_height = item_height > 16 ? item_height : 24;
    menu->hover = -1;
}

i32 axw_menu_add(axw_menu_t *menu, const char *label, const char *shortcut,
                 const char *glyph)
{
    if (!menu || menu->count >= 16) {
        return -1;
    }
    i32 index = menu->count++;
    menu->items[index].label = label;
    menu->items[index].shortcut = shortcut;
    menu->items[index].glyph = glyph;
    menu->items[index].enabled = (bool_t)1;
    return index;
}

i32 axw_menu_separator(axw_menu_t *menu)
{
    if (!menu || menu->count >= 16) {
        return -1;
    }
    i32 index = menu->count++;
    menu->items[index].separator = (bool_t)1;
    return index;
}

void axw_menu_paint(gui_surface_t *surface, const axw_menu_t *menu)
{
    if (!surface || !menu) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    gui_shadow(surface, menu->rect, 6, (u8)axui_shadow_strength());
    axui_round_rect_aa(surface, menu->rect, 6, axui_surface_overlay());
    axui_round_rect_border(surface, menu->rect, 6, 1, theme->border);

    i32 y = menu->rect.y + 4;
    for (i32 i = 0; i < menu->count; i++) {
        const axw_menu_item_t *item = &menu->items[i];
        if (item->separator) {
            gui_fill_rect(surface, gui_rect(menu->rect.x + 8, y + 3,
                                            menu->rect.w - 16, 1),
                          axui_hairline(theme->window_bg_alt));
            y += 8;
            continue;
        }
        gui_rect_t row = gui_rect(menu->rect.x + 4, y, menu->rect.w - 8,
                                  menu->item_height);
        if (i == menu->hover) {
            gui_fill_rect(surface, row, axui_colour_alpha(theme->selection, 200));
        }

        u32 colour = item->enabled ? theme->text : axui_disabled_text();
        i32 pen = row.x + 10;
        if (item->glyph) {
            axui_draw_text(surface, pen, axui_text_baseline(row, 1), item->glyph,
                           axui_style(theme->accent, 1));
            pen += 18;
        }
        axui_text_style_t style = axui_style(colour, 1);
        axui_draw_text_clip(surface, row, pen, axui_text_baseline(row, 1),
                            item->label, style);

        if (item->checked) {
            axui_draw_text_right(surface, gui_rect(row.x, row.y, row.w - 52, row.h),
                                 "v", axui_style(theme->success, 1));
        }
        if (item->shortcut) {
            axui_draw_text_right(surface, row, item->shortcut,
                                 axui_style(theme->text_dim, 1));
        }
        y += menu->item_height;
    }
}

u32 axw_menu_event(axw_menu_t *menu, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!menu) {
        return AXW_NONE;
    }
    /* Recompute the row from a running y, mirroring the paint loop. */
    i32 pen_y = menu->rect.y + 4;
    i32 hit = -1;
    for (i32 i = 0; i < menu->count; i++) {
        if (menu->items[i].separator) {
            pen_y += 8;
            continue;
        }
        gui_rect_t row = gui_rect(menu->rect.x + 4, pen_y, menu->rect.w - 8,
                                  menu->item_height);
        if (axw_hit(row, x, y)) {
            hit = i;
            break;
        }
        pen_y += menu->item_height;
    }
    menu->hover = hit;

    if (pressed && hit >= 0) {
        if (!menu->items[hit].enabled) {
            return AXW_NONE;
        }
        menu->highlighted = hit;
        menu->open = (bool_t)0;
        return AXW_CLICK | AXW_CLOSE | (u32)(hit << 16);
    }
    (void)buttons;
    return AXW_NONE;
}

/* ------------------------------------------------------------------ */
/* Tree                                                                */
/* ------------------------------------------------------------------ */

/* A flattened tree row: depth, label, expanded flag and whether it has
 * children.  The caller builds the flat list, which keeps the widget free of
 * recursion. */
void axw_tree_init(axw_tree_t *tree, gui_rect_t rect, i32 row_height)
{
    ax_memset(tree, 0, sizeof(*tree));
    tree->rect = rect;
    tree->row_height = row_height > 16 ? row_height : 20;
    tree->selected = -1;
}

i32 axw_tree_add(axw_tree_t *tree, const char *label, i32 depth, bool_t expanded,
                 bool_t leaf)
{
    if (!tree || tree->count >= AXW_LIST_MAX) {
        return -1;
    }
    i32 index = tree->count++;
    tree->rows[index].label = label;
    tree->rows[index].depth = depth;
    tree->rows[index].expanded = expanded;
    tree->rows[index].leaf = leaf;
    return index;
}

void axw_tree_paint(gui_surface_t *surface, axw_tree_t *tree)
{
    if (!surface || !tree) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    i32 visible = tree->rect.h / tree->row_height;
    i32 first = tree->scroll / tree->row_height;

    for (i32 i = 0; i < visible; i++) {
        i32 index = first + i;
        if (index >= tree->count) {
            break;
        }
        axw_tree_row_t *row = &tree->rows[index];
        gui_rect_t rect = gui_rect(tree->rect.x, tree->rect.y + i * tree->row_height,
                                   tree->rect.w, tree->row_height);
        if (index == tree->selected) {
            gui_fill_rect(surface, rect, axui_colour_alpha(theme->selection, 200));
        }

        i32 indent = 10 + row->depth * 14;
        i32 pen = rect.x + indent;
        if (row->leaf) {
            axui_draw_text(surface, pen, axui_text_baseline(rect, 1), "-",
                           axui_style(theme->text_dim, 1));
            pen += 12;
        } else {
            axui_draw_text(surface, pen, axui_text_baseline(rect, 1),
                           row->expanded ? "v" : ">", axui_style(theme->accent, 1));
            pen += 12;
        }
        axui_text_style_t style = axui_style(theme->text, 1);
        axui_draw_text_clip(surface, rect, pen, axui_text_baseline(rect, 1),
                            row->label, style);
    }
}

u32 axw_tree_event(axw_tree_t *tree, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!tree) {
        return AXW_NONE;
    }
    if (pressed && axw_hit(tree->rect, x, y)) {
        i32 index = axw_hit_row(tree->rect, y, tree->row_height, tree->scroll);
        if (index >= 0 && index < tree->count) {
            tree->selected = index;
            return AXW_CLICK | AXW_SELECT | AXW_TOGGLE;
        }
    }
    (void)buttons;
    return AXW_NONE;
}

/* Keyboard navigation for the table: up/down move the selection, page keys
 * jump by the visible row count, home/end go to the ends. */
u32 axw_table_key(axw_table_t *table, u8 scancode)
{
    if (!table || table->rows == 0) {
        return AXW_NONE;
    }
    i32 old = table->selected;
    i32 visible = (table->rect.h - table->row_height) / table->row_height;
    if (visible < 1) {
        visible = 1;
    }
    switch (scancode) {
    case 0x48:      /* up */
        table->selected = table->selected <= 0 ? 0 : table->selected - 1;
        break;
    case 0x50:      /* down */
        table->selected = table->selected < 0 ? 0 : table->selected + 1;
        if (table->selected >= table->rows) {
            table->selected = table->rows - 1;
        }
        break;
    case 0x49:      /* page up */
        table->selected = table->selected < 0 ? 0 : table->selected - visible;
        break;
    case 0x51:      /* page down */
        table->selected = table->selected < 0 ? visible : table->selected + visible;
        if (table->selected >= table->rows) {
            table->selected = table->rows - 1;
        }
        break;
    case 0x47:      /* home */
        table->selected = 0;
        break;
    case 0x4F:      /* end */
        table->selected = table->rows - 1;
        break;
    default:
        return AXW_NONE;
    }
    if (table->selected < 0) {
        table->selected = 0;
    }
    /* Keep the selection visible by nudging the scroll offset. */
    i32 top = table->scroll / table->row_height;
    if (table->selected < top) {
        table->scroll = table->selected * table->row_height;
    } else if (table->selected >= top + visible) {
        table->scroll = (table->selected - visible + 1) * table->row_height;
    }
    return table->selected != old ? (AXW_SELECT | AXW_CHANGED) : AXW_NONE;
}