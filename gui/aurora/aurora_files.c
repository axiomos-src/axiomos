/*
 * AxiomOS - File manager surface.
 *
 * One directory at a time, three view modes, and a details pane that only
 * appears when the window is wide enough to keep the list readable.  Sorting is
 * derived: the model keeps the original order and the view applies a comparison
 * when it paints, so changing the sort never reorders the stored entries and a
 * refresh from the filesystem cannot lose the user's selection.
 *
 * Icons are drawn from a small shape language rather than bitmaps: folder,
 * document, executable, image, archive.  That keeps the surface independent of
 * an icon atlas that does not exist yet.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define FM_MAX_ENTRIES   128
#define FM_ROW_HEIGHT    32
#define FM_TOOLBAR       48
#define FM_SIDEBAR       200
#define FM_DETAILS       260

typedef enum fm_entry_kind {
    FM_KIND_FOLDER = 1,
    FM_KIND_FILE,
    FM_KIND_EXECUTABLE,
    FM_KIND_IMAGE,
    FM_KIND_ARCHIVE
} fm_entry_kind_t;

typedef enum fm_view_mode {
    FM_VIEW_DETAILS = 0,
    FM_VIEW_LIST,
    FM_VIEW_GRID
} fm_view_mode_t;

typedef enum fm_sort {
    FM_SORT_NAME = 0,
    FM_SORT_SIZE,
    FM_SORT_DATE,
    FM_SORT_KIND
} fm_sort_t;

typedef struct fm_entry {
    char            name[64];
    fm_entry_kind_t kind;
    u64             size;
    u32             modified_day;
    bool_t          selected;
    bool_t          hidden;
} fm_entry_t;

typedef struct fm_app {
    gui_rect_t       frame;
    char             path[128];
    fm_entry_t       entries[FM_MAX_ENTRIES];
    i32              entry_count;
    i32              scroll;
    i32              hover;
    i32              selected;
    fm_view_mode_t   view;
    fm_sort_t        sort;
    bool_t           sort_descending;
    bool_t           show_hidden;
    char             search[40];
    axau_breadcrumb_t crumb;
    axau_command_bar_t bar;
    axau_menu_t      context;
    bool_t           context_open;
    gui_rect_t       rows[FM_MAX_ENTRIES];
    i32              visible_rows;
    bool_t           details_open;
    bool_t           compact;
} fm_app_t;

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

static const char *fm_kind_name(fm_entry_kind_t kind)
{
    switch (kind) {
    case FM_KIND_FOLDER: return "Folder";
    case FM_KIND_EXECUTABLE: return "Application";
    case FM_KIND_IMAGE: return "Image";
    case FM_KIND_ARCHIVE: return "Archive";
    case FM_KIND_FILE:
    default: return "File";
    }
}

static const char *fm_kind_glyph(fm_entry_kind_t kind)
{
    switch (kind) {
    case FM_KIND_FOLDER: return "/";
    case FM_KIND_EXECUTABLE: return ">";
    case FM_KIND_IMAGE: return "#";
    case FM_KIND_ARCHIVE: return "=";
    case FM_KIND_FILE:
    default: return ".";
    }
}

static u32 fm_kind_colour(const fm_entry_kind_t kind,
                          const axau_palette_t *palette)
{
    switch (kind) {
    case FM_KIND_FOLDER:
        return axau_colour(palette, AXAU_COLOUR_ACCENT);
    case FM_KIND_EXECUTABLE:
        return axau_colour(palette, AXAU_COLOUR_SUCCESS);
    case FM_KIND_IMAGE:
        return axau_colour(palette, AXAU_COLOUR_WARNING);
    case FM_KIND_ARCHIVE:
        return axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY);
    case FM_KIND_FILE:
    default:
        return axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY);
    }
}

void fm_app_init(fm_app_t *app, gui_rect_t frame)
{
    if (!app) return;
    ax_memset(app, 0, sizeof(*app));
    app->frame = frame;
    app->view = FM_VIEW_DETAILS;
    app->sort = FM_SORT_NAME;
    app->hover = -1;
    app->selected = -1;
    ax_strlcpy(app->path, "AxiomOS > Home", sizeof(app->path));
    axau_breadcrumb_set(&app->crumb,
                        gui_rect(frame.x + FM_SIDEBAR + 8, frame.y + 12,
                                 frame.w - FM_SIDEBAR - 260, 32),
                        "AxiomOS/Home/Documents");
    axau_command_bar_init(&app->bar,
                          gui_rect(frame.x + FM_SIDEBAR + 8, frame.y + FM_TOOLBAR,
                                   frame.w - FM_SIDEBAR - 16, 40));
    axau_command_bar_add(&app->bar, "New", "+", (bool_t)1);
    axau_command_bar_add(&app->bar, "Open", ">", (bool_t)0);
    axau_command_bar_add(&app->bar, "Copy", "=", (bool_t)0);
    axau_command_bar_add(&app->bar, "Delete", "x", (bool_t)0);
    axau_menu_init(&app->context, 0, 0);
}

void fm_app_add(fm_app_t *app, const char *name, fm_entry_kind_t kind, u64 size,
                u32 modified_day)
{
    if (!app || app->entry_count >= FM_MAX_ENTRIES) return;
    fm_entry_t *entry = &app->entries[app->entry_count++];
    ax_memset(entry, 0, sizeof(*entry));
    ax_strlcpy(entry->name, name, sizeof(entry->name));
    entry->kind = kind;
    entry->size = size;
    entry->modified_day = modified_day;
}

/* Seed a plausible home directory.  Sizes are bytes and dates are day numbers
 * so the surface has something to sort and display without a real disk. */
void fm_app_seed(fm_app_t *app)
{
    if (!app) return;
    fm_app_add(app, "Documents", FM_KIND_FOLDER, 0, 120);
    fm_app_add(app, "Downloads", FM_KIND_FOLDER, 0, 121);
    fm_app_add(app, "Pictures", FM_KIND_FOLDER, 0, 118);
    fm_app_add(app, "Projects", FM_KIND_FOLDER, 0, 122);
    fm_app_add(app, "readme.md", FM_KIND_FILE, 4096, 122);
    fm_app_add(app, "kernel.elf", FM_KIND_EXECUTABLE, 7340032, 123);
    fm_app_add(app, "notes.txt", FM_KIND_FILE, 12800, 119);
    fm_app_add(app, "aurora-theme.json", FM_KIND_FILE, 2048, 117);
    fm_app_add(app, "screenshot.png", FM_KIND_IMAGE, 921600, 123);
    fm_app_add(app, "backup.axz", FM_KIND_ARCHIVE, 52428800, 110);
    fm_app_add(app, "build.log", FM_KIND_FILE, 65536, 123);
    fm_app_add(app, "AXIOM.CONF", FM_KIND_FILE, 512, 111);
}

/* ------------------------------------------------------------------ */
/* Sorting                                                             */
/* ------------------------------------------------------------------ */

/* Folders always sort before files: that is a file manager convention, not an
 * incidental side effect of the comparison. */
static i32 fm_compare(const fm_app_t *app, const fm_entry_t *a,
                      const fm_entry_t *b)
{
    if (a->kind == FM_KIND_FOLDER && b->kind != FM_KIND_FOLDER) return -1;
    if (b->kind == FM_KIND_FOLDER && a->kind != FM_KIND_FOLDER) return 1;
    i32 result = 0;
    switch (app->sort) {
    case FM_SORT_SIZE:
        result = a->size < b->size ? -1 : (a->size > b->size ? 1 : 0);
        break;
    case FM_SORT_DATE:
        result = a->modified_day < b->modified_day ? -1
                 : (a->modified_day > b->modified_day ? 1 : 0);
        break;
    case FM_SORT_KIND:
        result = (i32)a->kind - (i32)b->kind;
        break;
    case FM_SORT_NAME:
    default: {
        /* A case-insensitive, digit-aware comparison would be better for real
         * filenames; this compares by byte and is honest about it. */
        const char *pa = a->name;
        const char *pb = b->name;
        while (*pa && *pb) {
            char ca = *pa >= 'A' && *pa <= 'Z' ? (char)(*pa + 32) : *pa;
            char cb = *pb >= 'A' && *pb <= 'Z' ? (char)(*pb + 32) : *pb;
            if (ca != cb) {
                result = ca < cb ? -1 : 1;
                break;
            }
            pa++;
            pb++;
        }
        if (result == 0) {
            if (*pa) result = 1;
            else if (*pb) result = -1;
        }
        break;
    }
    }
    return app->sort_descending ? -result : result;
}

/* Build an index array sorted by the active comparison.  There is no allocation:
 * the scratch array is on the stack and bounded by FM_MAX_ENTRIES. */
static i32 fm_sorted_indices(const fm_app_t *app, i32 *indices)
{
    i32 count = 0;
    for (i32 i = 0; i < app->entry_count; i++) {
        if (!app->show_hidden && app->entries[i].name[0] == '.') continue;
        if (app->search[0]) {
            /* Substring match, case-insensitive ASCII. */
            bool_t match = (bool_t)0;
            const char *needle = app->search;
            for (const char *hay = app->entries[i].name; *hay && !match; hay++) {
                const char *h = hay;
                const char *n = needle;
                while (*h && *n) {
                    char ch = *h >= 'A' && *h <= 'Z' ? (char)(*h + 32) : *h;
                    char cn = *n >= 'A' && *n <= 'Z' ? (char)(*n + 32) : *n;
                    if (ch != cn) break;
                    h++;
                    n++;
                }
                if (!*n) match = (bool_t)1;
            }
            if (!match) continue;
        }
        indices[count++] = i;
    }
    /* Insertion sort: the list is bounded at 128 entries and the comparison can
     * touch strings, so a stable O(n^2) sort beats a fragile quicksort here. */
    for (i32 i = 1; i < count; i++) {
        i32 key = indices[i];
        i32 j = i - 1;
        while (j >= 0 && fm_compare(app, &app->entries[indices[j]],
                                    &app->entries[key]) > 0) {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = key;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

static void fm_paint_sidebar(gui_surface_t *surface, fm_app_t *app,
                             const axau_palette_t *palette,
                             const axau_metrics_t *metrics)
{
    gui_rect_t pane = gui_rect(app->frame.x, app->frame.y + FM_TOOLBAR,
                               FM_SIDEBAR, app->frame.h - FM_TOOLBAR);
    gui_fill_rect(surface, pane,
                  axui_colour_alpha(axau_colour(palette, AXAU_COLOUR_CONTROL), 24));
    const char *labels[] = {"Home", "Documents", "Downloads", "Pictures",
                            "Projects", "This device", "Network"};
    const char *glyphs[] = {"*", "/", "v", "#", "=", ">", "n"};
    i32 row_height = metrics ? metrics->row_height : AXAU_ROW_HEIGHT;
    for (i32 i = 0; i < 7; i++) {
        gui_rect_t row = gui_rect(pane.x + 6, pane.y + 16 + i * row_height,
                                  pane.w - 12, row_height - 4);
        bool_t selected = (bool_t)(i == 0);
        if (selected) {
            gui_fill_round_rect(surface, row, 4,
                                axui_colour_opacity(
                                    axau_colour(palette, AXAU_COLOUR_ACCENT), 14));
            gui_fill_round_rect(surface, gui_rect(row.x + 1,
                                                  row.y + row.h / 2 - 8,
                                                  3, 16), 2,
                                axau_colour(palette, AXAU_COLOUR_ACCENT));
        }
        axau_icon_glyph(surface, row.x + 18, row.y + row.h / 2, 12, glyphs[i],
                        selected ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                 : axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY));
        axui_draw_text(surface, row.x + 34, axau_text_center_y(row, 1), labels[i],
                       axui_style(selected ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                          : axau_colour(palette,
                                                        AXAU_COLOUR_TEXT_SECONDARY), 1));
    }
}

static void fm_paint_entry_row(gui_surface_t *surface, const fm_app_t *app,
                               const fm_entry_t *entry, gui_rect_t row,
                               bool_t selected, bool_t hover,
                               const axau_palette_t *palette)
{
    if (selected) {
        gui_fill_round_rect(surface, row, 4,
                            axui_colour_opacity(
                                axau_colour(palette, AXAU_COLOUR_ACCENT), 16));
    } else if (hover) {
        gui_fill_round_rect(surface, row, 4,
                            axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
    }
    u32 colour = fm_kind_colour(entry->kind, palette);
    i32 icon_x = row.x + 18;
    i32 centre = row.y + row.h / 2;
    axau_icon_glyph(surface, icon_x, centre, 14, fm_kind_glyph(entry->kind), colour);
    axui_draw_text_clip(surface, row, row.x + 32, centre - 8, entry->name,
                        axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));

    if (app->view == FM_VIEW_DETAILS) {
        char size[24];
        if (entry->kind == FM_KIND_FOLDER) {
            ax_strlcpy(size, "-", sizeof(size));
        } else if (entry->size >= 1048576) {
            ax_snprintf(size, sizeof(size), "%llu MB",
                        (unsigned long long)(entry->size / 1048576));
        } else {
            ax_snprintf(size, sizeof(size), "%llu KB",
                        (unsigned long long)(entry->size / 1024));
        }
        axui_draw_text_right(surface,
                             gui_rect(row.x + row.w - 120, row.y, 72, row.h),
                             size,
                             axui_style(axau_colour(palette,
                                                    AXAU_COLOUR_TEXT_TERTIARY), 1));
        axui_draw_text_right(surface,
                             gui_rect(row.x + row.w - 44, row.y, 44, row.h),
                             fm_kind_name(entry->kind),
                             axui_style(colour, 1));
    }
}

static void fm_paint_grid(gui_surface_t *surface, fm_app_t *app,
                          const i32 *indices, i32 count,
                          gui_rect_t content, const axau_palette_t *palette)
{
    i32 columns = content.w / 140;
    if (columns < 1) columns = 1;
    i32 cell_w = content.w / columns;
    i32 cell_h = 104;
    i32 visible = (content.h / cell_h) * columns;
    if (visible > count) visible = count;
    for (i32 i = 0; i < visible; i++) {
        i32 column = i % columns;
        i32 row_index = i / columns;
        gui_rect_t cell = gui_rect(content.x + column * cell_w,
                                   content.y + row_index * cell_h,
                                   cell_w - 8, cell_h - 8);
        const fm_entry_t *entry = &app->entries[indices[i]];
        if (entry->selected || app->selected == indices[i]) {
            gui_fill_round_rect(surface, cell, AXAU_RADIUS_CARD,
                                axui_colour_opacity(
                                    axau_colour(palette, AXAU_COLOUR_ACCENT), 16));
        } else if (app->hover == i) {
            gui_fill_round_rect(surface, cell, AXAU_RADIUS_CARD,
                                axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
        }
        axau_icon_glyph(surface, cell.x + cell.w / 2, cell.y + 34, 28,
                        fm_kind_glyph(entry->kind),
                        fm_kind_colour(entry->kind, palette));
        axui_draw_text_center(surface,
                              gui_rect(cell.x, cell.y + 58, cell.w, 16),
                              entry->name,
                              axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
    }
}

void fm_app_paint(gui_surface_t *surface, fm_app_t *app)
{
    if (!surface || !app) return;
    const axau_context_t *context = axau_context();
    const axau_palette_t *palette = &context->palette;

    gui_fill_rect(surface, app->frame, axau_colour(palette, AXAU_COLOUR_SURFACE));
    gui_rect_t toolbar = gui_rect(app->frame.x, app->frame.y, app->frame.w,
                                  FM_TOOLBAR);
    gui_fill_rect(surface, toolbar,
                  axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    axau_breadcrumb_paint(surface, &app->crumb, palette);
    axau_command_bar_paint(surface, &app->bar, palette, &context->metrics);
    axau_paint_divider(surface, gui_rect(toolbar.x, toolbar.y + toolbar.h - 1,
                                         toolbar.w, 1), palette);

    fm_paint_sidebar(surface, app, palette, &context->metrics);

    gui_rect_t content = gui_rect(app->frame.x + FM_SIDEBAR + 8,
                                  app->frame.y + FM_TOOLBAR + 44,
                                  app->frame.w - FM_SIDEBAR - 16,
                                  app->frame.h - FM_TOOLBAR - 52);
    if (app->details_open && content.w > FM_DETAILS + 240) {
        content.w -= FM_DETAILS;
    }

    i32 indices[FM_MAX_ENTRIES];
    i32 count = fm_sorted_indices(app, indices);
    if (app->view == FM_VIEW_GRID) {
        fm_paint_grid(surface, app, indices, count, content, palette);
    } else {
        i32 row_height = app->view == FM_VIEW_LIST ? FM_ROW_HEIGHT + 4
                                                    : FM_ROW_HEIGHT;
        i32 start = 0;
        app->visible_rows = content.h / row_height;
        i32 visible = count - start;
        if (visible > app->visible_rows) visible = app->visible_rows;
        for (i32 i = 0; i < visible; i++) {
            i32 index = indices[start + i];
            gui_rect_t row = gui_rect(content.x, content.y + i * row_height,
                                      content.w, row_height - 1);
            app->rows[i] = row;
            fm_paint_entry_row(surface, app, &app->entries[index], row,
                               app->selected == index, app->hover == i, palette);
        }
        /* Column headers in details mode. */
        if (app->view == FM_VIEW_DETAILS) {
            axui_draw_text(surface, content.x + 32, content.y - 18, "Name",
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT_TERTIARY), 1));
            axui_draw_text_right(surface,
                                 gui_rect(content.x + content.w - 192, content.y - 18,
                                          72, 16), "Size",
                                 axui_style(axau_colour(palette,
                                                        AXAU_COLOUR_TEXT_TERTIARY), 1));
        }
    }

    if (app->details_open && content.w > 240) {
        gui_rect_t details = gui_rect(app->frame.x + app->frame.w - FM_DETAILS,
                                      content.y, FM_DETAILS, content.h);
        axau_paint_divider(surface, gui_rect(details.x - 1, details.y, 1,
                                             details.h), palette);
        if (app->selected >= 0 && app->selected < app->entry_count) {
            const fm_entry_t *entry = &app->entries[app->selected];
            axau_icon_glyph(surface, details.x + 24, details.y + 32, 36,
                            fm_kind_glyph(entry->kind),
                            fm_kind_colour(entry->kind, palette));
            axui_draw_text_wrapped(surface,
                                   gui_rect(details.x + 16, details.y + 64,
                                            details.w - 32, 48),
                                   entry->name,
                                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
            char line[64];
            ax_snprintf(line, sizeof(line), "%s, %llu bytes",
                        fm_kind_name(entry->kind),
                        (unsigned long long)entry->size);
            axui_draw_text(surface, details.x + 16, details.y + 120, line,
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT_SECONDARY), 1));
        }
    }

    if (app->context_open) {
        axau_menu_paint(surface, &app->context, palette);
    }
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

i32 fm_app_click(fm_app_t *app, i32 x, i32 y, u32 button)
{
    if (!app) return 0;
    i32 command = axau_command_bar_hit(&app->bar, x, y);
    if (command >= 0) {
        return 1 + command;
    }
    if (app->context_open) {
        if (axau_menu_hit(&app->context, x, y) >= 0) {
            app->context_open = (bool_t)0;
            return 1;
        }
        app->context_open = (bool_t)0;
        return 0;
    }
    if (button == 0x02) {
        app->context_open = (bool_t)1;
        axau_menu_init(&app->context, x, y);
        axau_menu_add(&app->context, "Open", "Enter", ">");
        axau_menu_add(&app->context, "Copy", "Ctrl-C", "=");
        axau_menu_add(&app->context, "Rename", "F2", "r");
        axau_menu_add_separator(&app->context);
        axau_menu_add(&app->context, "Properties", "Alt-Enter", "i");
        return 1;
    }

    i32 indices[FM_MAX_ENTRIES];
    i32 count = fm_sorted_indices(app, indices);
    gui_rect_t content = gui_rect(app->frame.x + FM_SIDEBAR + 8,
                                  app->frame.y + FM_TOOLBAR + 44,
                                  app->frame.w - FM_SIDEBAR - 16,
                                  app->frame.h - FM_TOOLBAR - 52);
    if (app->view == FM_VIEW_GRID) {
        i32 columns = content.w / 140;
        if (columns < 1) columns = 1;
        i32 cell_w = content.w / columns;
        i32 cell_h = 104;
        i32 column = (x - content.x) / (cell_w ? cell_w : 1);
        i32 row_index = (y - content.y) / cell_h;
        i32 index = row_index * columns + column;
        if (index >= 0 && index < count) {
            app->selected = indices[index];
            app->details_open = (bool_t)1;
            return 1;
        }
        return 0;
    }
    i32 row_height = app->view == FM_VIEW_LIST ? FM_ROW_HEIGHT + 4 : FM_ROW_HEIGHT;
    i32 row_index = (y - content.y) / (row_height ? row_height : 1);
    if (y < content.y || row_index < 0 || row_index >= count ||
        row_index >= app->visible_rows) {
        return 0;
    }
    app->selected = indices[row_index];
    app->details_open = (bool_t)1;
    return 1;
}

void fm_app_hover(fm_app_t *app, i32 x, i32 y)
{
    if (!app) return;
    gui_rect_t content = gui_rect(app->frame.x + FM_SIDEBAR + 8,
                                  app->frame.y + FM_TOOLBAR + 44,
                                  app->frame.w - FM_SIDEBAR - 16,
                                  app->frame.h - FM_TOOLBAR - 52);
    if (app->view == FM_VIEW_GRID) {
        i32 cell_h = 104;
        i32 columns = content.w / 140;
        if (columns < 1) columns = 1;
        i32 column = (x - content.x) / (content.w / columns);
        i32 row = (y - content.y) / cell_h;
        app->hover = row * columns + column;
        return;
    }
    i32 row_height = app->view == FM_VIEW_LIST ? FM_ROW_HEIGHT + 4 : FM_ROW_HEIGHT;
    app->hover = (y - content.y) / (row_height ? row_height : 1);
}

void fm_app_sort(fm_app_t *app, fm_sort_t sort)
{
    if (!app) return;
    if (app->sort == sort) {
        app->sort_descending = (bool_t)!app->sort_descending;
    } else {
        app->sort = sort;
        app->sort_descending = (bool_t)0;
    }
}

void fm_app_navigate_up(fm_app_t *app)
{
    if (!app) return;
    usize length = ax_strlen(app->path);
    while (length > 0 && app->path[length - 1] != '/') length--;
    if (length > 0) {
        app->path[length - 1] = 0;
    }
    axau_breadcrumb_set(&app->crumb,
                        gui_rect(app->frame.x + FM_SIDEBAR + 8,
                                 app->frame.y + 12,
                                 app->frame.w - FM_SIDEBAR - 260, 32),
                        app->path);
}

i32 fm_app_count(const fm_app_t *app)
{
    return app ? app->entry_count : 0;
}

void fm_app_dump(const fm_app_t *app)
{
    if (!app) return;
    printk("files: %d entries, view=%d sort=%d desc=%d selected=%d\n",
           app->entry_count, (i32)app->view, (i32)app->sort,
           app->sort_descending, app->selected);
    printk("files: path=%s\n", app->path);
}
