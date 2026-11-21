/*
 * AxiomOS - Settings surface.
 *
 * Settings is a list of sections, each a list of rows.  A row is one of a small
 * closed set: a toggle, a choice, a value with a chevron, or an action button.
 * Modelling rows as data rather than as bespoke paint code means a new page is
 * a table, and the paint and hit paths stay identical for every page.
 *
 * The width at which a row switches from label-left/control-right to stacked is
 * derived from the label length, not a fixed breakpoint, because a long label on
 * a narrow window is the case that actually breaks the layout.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define SETTINGS_MAX_SECTIONS 12
#define SETTINGS_MAX_ROWS     24
#define SETTINGS_ROW_HEIGHT   56
#define SETTINGS_HEADER       48

typedef enum settings_row_kind {
    SETTINGS_ROW_TOGGLE = 1,
    SETTINGS_ROW_CHOICE,
    SETTINGS_ROW_VALUE,
    SETTINGS_ROW_ACTION,
    SETTINGS_ROW_SLIDER,
    SETTINGS_ROW_INFO
} settings_row_kind_t;

typedef struct settings_row {
    settings_row_kind_t kind;
    char   title[48];
    char   subtitle[64];
    char   value[32];
    char   glyph[4];
    bool_t on;
    i32    slider_value;
    i32    hover;
    i32    anim;
} settings_row_t;

typedef struct settings_section {
    char title[40];
    char description[64];
    settings_row_t rows[SETTINGS_MAX_ROWS];
    i32  row_count;
    i32  scroll;
} settings_section_t;

typedef struct settings_app {
    gui_rect_t        frame;
    settings_section_t sections[SETTINGS_MAX_SECTIONS];
    i32               section_count;
    i32               active_section;
    axau_navigation_t nav;
    i32               hover_row;
    i32               pressed_row;
    i32               scroll;
    bool_t            compact;
    char              search[48];
} settings_app_t;

void settings_app_init(settings_app_t *app, gui_rect_t frame)
{
    if (!app) return;
    ax_memset(app, 0, sizeof(*app));
    app->frame = frame;
    axau_navigation_init(&app->nav,
                         gui_rect(frame.x, frame.y + SETTINGS_HEADER,
                                  frame.w, frame.h - SETTINGS_HEADER), 240);
    ax_strlcpy(app->nav.header, "AxiomOS Settings", sizeof(app->nav.header));
    app->hover_row = -1;
    app->pressed_row = -1;
}

static settings_section_t *settings_add_section(settings_app_t *app,
                                                const char *title,
                                                const char *description)
{
    if (!app || app->section_count >= SETTINGS_MAX_SECTIONS) return 0;
    settings_section_t *section = &app->sections[app->section_count];
    ax_memset(section, 0, sizeof(*section));
    ax_strlcpy(section->title, title, sizeof(section->title));
    ax_strlcpy(section->description, description ? description : "",
               sizeof(section->description));
    axau_navigation_add(&app->nav, title, ">");
    app->section_count++;
    return section;
}

static settings_row_t *settings_add_row(settings_section_t *section,
                                        settings_row_kind_t kind,
                                        const char *title, const char *subtitle)
{
    if (!section || section->row_count >= SETTINGS_MAX_ROWS) return 0;
    settings_row_t *row = &section->rows[section->row_count++];
    ax_memset(row, 0, sizeof(*row));
    row->kind = kind;
    ax_strlcpy(row->title, title, sizeof(row->title));
    ax_strlcpy(row->subtitle, subtitle ? subtitle : "", sizeof(row->subtitle));
    row->hover = -1;
    return row;
}

/* Build the default pages so the application has content the first time it is
 * opened.  Order matches the navigation pane: System, Personalization, Apps,
 * Network, Privacy, About. */
void settings_app_seed(settings_app_t *app)
{
    if (!app) return;

    settings_section_t *system = settings_add_section(app, "System",
                                                       "Display, sound, power");
    if (!system) return;
    settings_row_t *row = settings_add_row(system, SETTINGS_ROW_INFO, "Device",
                                           "AxiomOS on x86-64");
    if (row) ax_strlcpy(row->value, "AxiomStation", sizeof(row->value));
    row = settings_add_row(system, SETTINGS_ROW_SLIDER, "Volume",
                           "Output level for the default device");
    if (row) row->slider_value = 60;
    row = settings_add_row(system, SETTINGS_ROW_SLIDER, "Brightness",
                           "Panel backlight");
    if (row) row->slider_value = 70;
    row = settings_add_row(system, SETTINGS_ROW_TOGGLE, "Night light",
                           "Warmer colours after sunset");
    if (row) row->on = (bool_t)0;
    row = settings_add_row(system, SETTINGS_ROW_CHOICE, "Power mode",
                           "Balance performance and energy");
    if (row) ax_strlcpy(row->value, "Balanced", sizeof(row->value));
    row = settings_add_row(system, SETTINGS_ROW_TOGGLE, "Hibernate",
                           "Save session to disk on power loss");
    if (row) row->on = (bool_t)1;

    settings_section_t *personal = settings_add_section(app, "Personalization",
                                                         "Wallpaper, colours, lock screen");
    if (personal) {
        row = settings_add_row(personal, SETTINGS_ROW_CHOICE, "Theme",
                               "Light, dark or follow the system");
        if (row) ax_strlcpy(row->value, "Light", sizeof(row->value));
        row = settings_add_row(personal, SETTINGS_ROW_CHOICE, "Accent colour",
                               "Used for controls and highlights");
        if (row) ax_strlcpy(row->value, "#0067C0", sizeof(row->value));
        row = settings_add_row(personal, SETTINGS_ROW_TOGGLE, "Transparency",
                               "Sample the wallpaper behind surfaces");
        if (row) row->on = (bool_t)1;
        row = settings_add_row(personal, SETTINGS_ROW_TOGGLE, "Motion",
                               "Animate panels and window changes");
        if (row) row->on = (bool_t)1;
        row = settings_add_row(personal, SETTINGS_ROW_VALUE, "Wallpaper",
                               "Aurora bloom, 3840 x 2160");
        if (row) ax_strlcpy(row->value, "Change", sizeof(row->value));
    }

    settings_section_t *apps = settings_add_section(app, "Apps",
                                                     "Installed and default apps");
    if (apps) {
        row = settings_add_row(apps, SETTINGS_ROW_ACTION, "Default terminal",
                               "Which shell opens .axm scripts");
        if (row) ax_strlcpy(row->value, "Axiom Terminal", sizeof(row->value));
        row = settings_add_row(apps, SETTINGS_ROW_VALUE, "Installed apps",
                               "24 applications, 1.2 GiB");
        if (row) ax_strlcpy(row->value, "Manage", sizeof(row->value));
        row = settings_add_row(apps, SETTINGS_ROW_TOGGLE, "Background apps",
                               "Let apps run when not focused");
        if (row) row->on = (bool_t)1;
    }

    settings_section_t *network = settings_add_section(app, "Network",
                                                       "Connectivity and sharing");
    if (network) {
        row = settings_add_row(network, SETTINGS_ROW_TOGGLE, "Wi-Fi",
                               "Connected to AxiomLab");
        if (row) row->on = (bool_t)1;
        row = settings_add_row(network, SETTINGS_ROW_TOGGLE, "Bluetooth",
                               "No paired devices");
        if (row) row->on = (bool_t)0;
        row = settings_add_row(network, SETTINGS_ROW_TOGGLE, "VPN",
                               "Route traffic through a tunnel");
        if (row) row->on = (bool_t)0;
    }

    settings_section_t *privacy = settings_add_section(app, "Privacy",
                                                       "Permissions and diagnostics");
    if (privacy) {
        row = settings_add_row(privacy, SETTINGS_ROW_TOGGLE, "Diagnostics",
                               "Send crash reports locally");
        if (row) row->on = (bool_t)0;
        row = settings_add_row(privacy, SETTINGS_ROW_TOGGLE, "Location",
                               "Allow apps to request position");
        if (row) row->on = (bool_t)0;
        row = settings_add_row(privacy, SETTINGS_ROW_ACTION, "Clear activity",
                               "Remove recent file and app history");
        if (row) ax_strlcpy(row->value, "Clear", sizeof(row->value));
    }

    settings_section_t *about = settings_add_section(app, "About",
                                                    "Version and hardware");
    if (about) {
        row = settings_add_row(about, SETTINGS_ROW_INFO, "AxiomOS",
                               "Aurora shell, 1.0");
        if (row) ax_strlcpy(row->value, "1.0.0", sizeof(row->value));
        row = settings_add_row(about, SETTINGS_ROW_INFO, "Kernel",
                               "Hybrid kernel, 64-bit");
        if (row) ax_strlcpy(row->value, "5.5.0", sizeof(row->value));
        row = settings_add_row(about, SETTINGS_ROW_INFO, "HVCI",
                               "Hypervisor-enforced code integrity");
        if (row) ax_strlcpy(row->value, "Enabled", sizeof(row->value));
    }
}

/* ------------------------------------------------------------------ */
/* Layout and paint                                                    */
/* ------------------------------------------------------------------ */

static gui_rect_t settings_content_rect(const settings_app_t *app)
{
    i32 pane = app->nav.compact ? 56 : 240;
    return gui_rect(app->frame.x + pane + 24, app->frame.y + SETTINGS_HEADER,
                    app->frame.w - pane - 48, app->frame.h - SETTINGS_HEADER - 24);
}

static i32 settings_row_height(const settings_row_t *row, i32 width)
{
    if (row->kind == SETTINGS_ROW_INFO) return 64;
    i32 label_width = axui_measure_text(row->title, 1);
    if (label_width > width - 200) return SETTINGS_ROW_HEIGHT + 24;
    return SETTINGS_ROW_HEIGHT;
}

void settings_app_paint(gui_surface_t *surface, settings_app_t *app)
{
    if (!surface || !app) return;
    const axau_context_t *context = axau_context();
    const axau_palette_t *palette = &context->palette;

    gui_fill_rect(surface, app->frame, axau_colour(palette, AXAU_COLOUR_SURFACE));
    gui_rect_t header = gui_rect(app->frame.x, app->frame.y, app->frame.w,
                                 SETTINGS_HEADER);
    gui_fill_rect(surface, header, axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    axui_draw_text(surface, header.x + 20, header.y + 16, "Settings",
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
    axui_draw_text_right(surface,
                         gui_rect(header.x, header.y, header.w - 20, header.h),
                         axau_theme_name(axau_context()->palette.kind),
                         axui_style(axau_colour(palette,
                                                AXAU_COLOUR_TEXT_TERTIARY), 1));
    axau_paint_divider(surface, gui_rect(app->frame.x, header.y + header.h - 1,
                                         header.w, 1), palette);

    axau_navigation_paint(surface, &app->nav, palette, &context->metrics);
    gui_rect_t content = settings_content_rect(app);
    gui_vline(surface, content.x - 12, content.y, content.y + content.h,
              axau_colour(palette, AXAU_COLOUR_DIVIDER));

    i32 section_index = app->active_section;
    if (section_index < 0 || section_index >= app->section_count) {
        section_index = 0;
    }
    settings_section_t *section = &app->sections[section_index];
    i32 y = content.y + 4;
    axui_draw_text(surface, content.x, y, section->title,
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
    y += 36;
    if (section->description[0]) {
        axui_draw_text(surface, content.x, y, section->description,
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_SECONDARY), 1));
        y += 28;
    }

    /* Rows sit on a card: one rounded surface holds the whole list, and
     * separators mark the boundaries between rows. */
    i32 row_width = content.w;
    i32 total_height = 0;
    for (i32 i = 0; i < section->row_count; i++) {
        total_height += settings_row_height(&section->rows[i], row_width);
    }
    gui_rect_t card = gui_rect(content.x, y, row_width,
                               total_height > content.h - (y - content.y)
                                   ? content.h - (y - content.y)
                                   : total_height);
    axau_material_t material;
    axau_material_card(&material, palette, AXAU_RADIUS_CARD);
    axau_paint_material(surface, card, &material, palette);

    i32 cursor = y;
    for (i32 i = 0; i < section->row_count; i++) {
        settings_row_t *row = &section->rows[i];
        i32 height = settings_row_height(row, row_width);
        if (cursor + height > card.y + card.h) {
            break;
        }
        gui_rect_t row_rect = gui_rect(card.x + 16, cursor, card.w - 32, height);
        if (app->hover_row == i) {
            gui_fill_round_rect(surface, row_rect, 4,
                                axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
        }
        if (row->kind == SETTINGS_ROW_INFO) {
            axui_draw_text(surface, row_rect.x, row_rect.y + 12, row->title,
                           axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
            axui_draw_text(surface, row_rect.x, row_rect.y + 32, row->subtitle,
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT_TERTIARY), 1));
            axui_draw_text_right(surface, row_rect, row->value,
                                 axui_style(axau_colour(palette,
                                                        AXAU_COLOUR_TEXT_SECONDARY), 1));
        } else {
            i32 text_top = height > SETTINGS_ROW_HEIGHT ? row_rect.y + 10
                                                        : row_rect.y + 8;
            axui_draw_text(surface, row_rect.x, text_top, row->title,
                           axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
            if (row->subtitle[0]) {
                axui_draw_text_clip(surface,
                                    gui_rect(row_rect.x, text_top + 20,
                                             row_rect.w - 140, 18),
                                    row_rect.x, text_top + 20, row->subtitle,
                                    axui_style(axau_colour(palette,
                                                           AXAU_COLOUR_TEXT_TERTIARY), 1));
            }
            if (row->kind == SETTINGS_ROW_TOGGLE) {
                axau_toggle_t toggle;
                ax_memset(&toggle, 0, sizeof(toggle));
                toggle.rect = gui_rect(row_rect.x + row_rect.w - 44,
                                       row_rect.y + (height - 20) / 2, 40, 20);
                toggle.on = row->on;
                toggle.thumb_anim = row->on ? 100 : 0;
                toggle.enabled = (bool_t)1;
                axau_toggle_paint(surface, &toggle, palette, &context->metrics);
            } else if (row->kind == SETTINGS_ROW_SLIDER) {
                axau_slider_t slider;
                ax_memset(&slider, 0, sizeof(slider));
                slider.rect = gui_rect(row_rect.x + row_rect.w / 2,
                                       row_rect.y + (height - 32) / 2,
                                       row_rect.w / 2 - 8, 32);
                slider.minimum = 0;
                slider.maximum = 100;
                slider.value = row->slider_value;
                slider.enabled = (bool_t)1;
                slider.show_value = (bool_t)1;
                axau_slider_paint(surface, &slider, palette, &context->metrics);
            } else if (row->value[0]) {
                axui_draw_text_right(surface, row_rect, row->value,
                                     axui_style(axau_colour(palette,
                                                            AXAU_COLOUR_ACCENT), 1));
            }
        }
        if (i + 1 < section->row_count) {
            axau_paint_divider(surface,
                               gui_rect(card.x + 16, cursor + height - 1,
                                        card.w - 32, 1), palette);
        }
        cursor += height;
    }
    (void)app->search;
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

static i32 settings_row_at(settings_app_t *app, i32 x, i32 y)
{
    gui_rect_t content = settings_content_rect(app);
    i32 section_index = app->active_section;
    if (section_index < 0 || section_index >= app->section_count) return -1;
    settings_section_t *section = &app->sections[section_index];
    i32 start_y = content.y + 4 + 36;
    if (section->description[0]) start_y += 28;
    i32 row_width = content.w;
    i32 cursor = start_y;
    for (i32 i = 0; i < section->row_count; i++) {
        i32 height = settings_row_height(&section->rows[i], row_width);
        gui_rect_t row_rect = gui_rect(content.x + 16, cursor, row_width - 32, height);
        if (gui_rect_contains(row_rect, x, y)) {
            return i;
        }
        cursor += height;
    }
    return -1;
}

i32 settings_app_click(settings_app_t *app, i32 x, i32 y)
{
    if (!app) return 0;
    i32 nav_index = axau_navigation_hit(&app->nav, x, y);
    if (nav_index >= 0 && nav_index < app->section_count) {
        app->active_section = nav_index;
        for (i32 i = 0; i < app->section_count; i++) {
            app->nav.items[i].selected = (bool_t)(i == nav_index);
        }
        return 1;
    }
    i32 index = settings_row_at(app, x, y);
    if (index < 0) return 0;
    settings_section_t *section = &app->sections[app->active_section];
    settings_row_t *row = &section->rows[index];
    switch (row->kind) {
    case SETTINGS_ROW_TOGGLE:
        row->on = (bool_t)!row->on;
        return 1;
    case SETTINGS_ROW_SLIDER: {
        gui_rect_t content = settings_content_rect(app);
        i32 row_width = content.w;
        /* Recompute the slider's x range for the drag. */
        i32 section_start = 2;
        (void)section_start;
        i32 height = settings_row_height(row, row_width);
        gui_rect_t row_rect = gui_rect(content.x + 16, y - height / 2,
                                       row_width - 32, height);
        axau_slider_t slider;
        ax_memset(&slider, 0, sizeof(slider));
        slider.rect = gui_rect(row_rect.x + row_rect.w / 2,
                               row_rect.y + (height - 32) / 2,
                               row_rect.w / 2 - 8, 32);
        slider.minimum = 0;
        slider.maximum = 100;
        slider.value = row->slider_value;
        slider.enabled = (bool_t)1;
        slider.dragging = (bool_t)1;
        row->slider_value = axau_slider_from_x(&slider, x);
        return 1;
    }
    case SETTINGS_ROW_CHOICE:
    case SETTINGS_ROW_VALUE:
    case SETTINGS_ROW_ACTION:
        /* Placeholder for the eventual picker flyout. */
        return 1;
    case SETTINGS_ROW_INFO:
    default:
        return 0;
    }
}

void settings_app_hover(settings_app_t *app, i32 x, i32 y)
{
    if (!app) return;
    app->hover_row = settings_row_at(app, x, y);
}

void settings_app_set_page(settings_app_t *app, i32 page)
{
    if (!app || page < 0 || page >= app->section_count) return;
    app->active_section = page;
    for (i32 i = 0; i < app->section_count; i++) {
        app->nav.items[i].selected = (bool_t)(i == page);
        app->nav.items[i].selection_anim = (bool_t)(i == page) ? 1000 : 0;
    }
}

i32 settings_app_page_count(const settings_app_t *app)
{
    return app ? app->section_count : 0;
}

void settings_app_dump(const settings_app_t *app)
{
    if (!app) return;
    printk("settings: %d sections, active=%d hover=%d\n", app->section_count,
           app->active_section, app->hover_row);
    for (i32 i = 0; i < app->section_count; i++) {
        printk("settings: [%d] %s rows=%d\n", i, app->sections[i].title,
               app->sections[i].row_count);
    }
}
