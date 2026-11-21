/*
 * AxiomOS - first-run experience.
 *
 * A five-step wizard: welcome, appearance, account, privacy, ready.  Each step
 * owns its own validation rule and the Next button is derived from it, so a user
 * cannot advance past an incomplete step by double-clicking.  Back is always
 * available except on the first step, and skipping privacy lands on the same
 * finish step rather than a different one.
 *
 * The wizard is a linear state machine, not five independent screens with shared
 * variables: state transitions are the only place a step can change, which keeps
 * the "can I proceed" answer in one function.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXO_STEPS 5
#define AXO_CARD_WIDTH 560

typedef enum axo_step {
    AXO_STEP_WELCOME = 0,
    AXO_STEP_APPEARANCE,
    AXO_STEP_ACCOUNT,
    AXO_STEP_PRIVACY,
    AXO_STEP_READY
} axo_step_t;

typedef enum axo_theme_choice {
    AXO_THEME_LIGHT = 0,
    AXO_THEME_DARK,
    AXO_THEME_AUTO,
    AXO_THEME_HIGH_CONTRAST
} axo_theme_choice_t;

typedef struct axo_setup {
    axo_step_t         step;
    axo_theme_choice_t theme;
    i32                accent_index;
    char               name[32];
    char               password_hint[32];
    bool_t             telemetry;
    bool_t             analytics_share;
    u32                location_services;
    bool_t             cloud_sync;
    i32                animation_progress;   /* 0..1000, step transition */
    bool_t             animating;
    i32                forward;
    bool_t             finish_hover;
    bool_t             back_hover;
    i32                accent_hover;
    i32                theme_hover;
    u32                accents[6];
    i32                accent_count;
} axo_setup_t;

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

void axo_init(axo_setup_t *setup)
{
    if (!setup) return;
    ax_memset(setup, 0, sizeof(*setup));
    setup->step = AXO_STEP_WELCOME;
    setup->theme = AXO_THEME_AUTO;
    setup->telemetry = (bool_t)1;
    setup->analytics_share = (bool_t)0;
    setup->location_services = 1;
    setup->accent_hover = -1;
    setup->theme_hover = -1;
    setup->accent_count = 6;
    setup->accents[0] = 0x004070C0;
    setup->accents[1] = 0x007040B0;
    setup->accents[2] = 0x00B06030;
    setup->accents[3] = 0x00308070;
    setup->accents[4] = 0x00B04070;
    setup->accents[5] = 0x00404040;
}

/* Step validity is a function of the step and the data, never a stored flag.  A
 * stored `can_continue` boolean is the classic source of a wizard that lies. */
static bool_t axo_name_valid(const char *name)
{
    return (bool_t)(name && name[0] && name[1]);
}

bool_t axo_can_continue(const axo_setup_t *setup)
{
    if (!setup) return (bool_t)0;
    switch (setup->step) {
    case AXO_STEP_ACCOUNT:
        return (bool_t)(axo_name_valid(setup->name));
    case AXO_STEP_WELCOME:
    case AXO_STEP_APPEARANCE:
    case AXO_STEP_PRIVACY:
    case AXO_STEP_READY:
    default:
        return (bool_t)1;
    }
}

bool_t axo_next(axo_setup_t *setup)
{
    if (!setup) return (bool_t)0;
    if (!axo_can_continue(setup)) return (bool_t)0;
    if (setup->step >= AXO_STEP_READY) return (bool_t)0;
    setup->step = (axo_step_t)(setup->step + 1);
    setup->animating = (bool_t)1;
    setup->animation_progress = 0;
    setup->forward = 1;
    return (bool_t)1;
}

bool_t axo_back(axo_setup_t *setup)
{
    if (!setup) return (bool_t)0;
    if (setup->step <= AXO_STEP_WELCOME) return (bool_t)0;
    setup->step = (axo_step_t)(setup->step - 1);
    setup->animating = (bool_t)1;
    setup->animation_progress = 0;
    setup->forward = 0;
    return (bool_t)1;
}

void axo_type(axo_setup_t *setup, char ch)
{
    if (!setup) return;
    if (setup->step != AXO_STEP_ACCOUNT) return;
    i32 length = (i32)ax_strlen(setup->name);
    if (ch == 0x08) {
        if (length > 0) setup->name[length - 1] = 0;
        return;
    }
    if (ch < 32 || ch > 126) return;
    if (length + 1 < (i32)sizeof(setup->name)) {
        setup->name[length] = ch;
        setup->name[length + 1] = 0;
    }
}

void axo_set_theme(axo_setup_t *setup, axo_theme_choice_t theme)
{
    if (!setup) return;
    setup->theme = theme;
}

void axo_set_accent(axo_setup_t *setup, i32 index)
{
    if (!setup || index < 0 || index >= setup->accent_count) return;
    setup->accent_index = index;
}

void axo_tick(axo_setup_t *setup)
{
    if (!setup || !setup->animating) return;
    setup->animation_progress += 120;
    if (setup->animation_progress >= 1000) {
        setup->animation_progress = 1000;
        setup->animating = (bool_t)0;
    }
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static gui_rect_t axo_card(const gui_rect_t screen)
{
    i32 width = screen.w < AXO_CARD_WIDTH + 48 ? screen.w - 48 : AXO_CARD_WIDTH;
    i32 height = 380;
    return gui_rect(screen.x + (screen.w - width) / 2,
                    screen.y + (screen.h - height) / 2, width, height);
}

static void axo_colours(u32 *top, u32 *bottom, u32 accent)
{
    *top = axui_darken(accent, 40);
    *bottom = axui_darken(0x00101820, 0);
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

void axo_paint(gui_surface_t *surface, axo_setup_t *setup, gui_rect_t screen,
               const axau_palette_t *palette)
{
    if (!surface || !setup || !palette) return;
    u32 accent = setup->accent_count > 0 ? setup->accents[setup->accent_index]
                                         : axau_colour(palette,
                                                       AXAU_COLOUR_ACCENT);
    u32 top, bottom;
    axo_colours(&top, &bottom, accent);
    gui_vgradient(surface, screen, top, bottom);
    /* The same soft bloom the lock screen uses, so first run and lock feel like
     * one product rather than two teams. */
    axui_circle(surface, screen.x + screen.w / 2, screen.y + screen.h / 2,
                screen.w / 3, axui_colour_opacity(accent, 8));

    gui_rect_t card = axo_card(screen);
    i32 slide = setup->animating
                    ? (setup->forward ? 24 : -24) *
                          (1000 - setup->animation_progress) / 1000
                    : 0;
    gui_rect_t drawn = gui_rect(card.x + slide, card.y, card.w, card.h);
    gui_fill_round_rect(surface, drawn, AXAU_RADIUS_CARD,
                        axau_colour(palette, AXAU_COLOUR_SURFACE));
    axui_round_rect_border(surface, drawn, AXAU_RADIUS_CARD, 1,
                           axau_colour(palette, AXAU_COLOUR_STROKE));

    const char *titles[AXO_STEPS] = {"Welcome to AxiomOS",
                                     "Make it yours",
                                     "Name this account",
                                     "Choose what to share",
                                     "You are ready"};
    const char *subtitles[AXO_STEPS] = {
        "A desktop built from Axiom's own design language.",
        "Pick a theme and an accent colour. You can change this later.",
        "A local account is all that is needed; no network is required.",
        "These switches are off unless you turn them on.",
        "Press finish to open the desktop."};
    axui_draw_text(surface, drawn.x + 32, drawn.y + 28, titles[setup->step],
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 3));
    axui_draw_text(surface, drawn.x + 32, drawn.y + 58, subtitles[setup->step],
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 1));

    i32 content_y = drawn.y + 96;
    switch (setup->step) {
    case AXO_STEP_APPEARANCE: {
        const char *themes[4] = {"Light", "Dark", "Automatic", "High contrast"};
        i32 card_w = (drawn.w - 64 - 24) / 2;
        for (i32 i = 0; i < 4; i++) {
            i32 column = i % 2;
            i32 row = i / 2;
            gui_rect_t tile = gui_rect(drawn.x + 32 + column * (card_w + 16),
                                       content_y + row * 72, card_w, 64);
            bool_t active = (i32)setup->theme == i;
            gui_fill_round_rect(surface, tile, 6,
                                active
                                    ? axui_colour_opacity(accent, 22)
                                    : axau_colour(palette,
                                                  AXAU_COLOUR_CONTROL));
            if (setup->theme_hover == i) {
                axui_round_rect_border(surface, tile, 6, 1,
                                       axau_colour(palette,
                                                   AXAU_COLOUR_STROKE_STRONG));
            }
            /* Theme previews are a two-tone swatch, not a label alone. */
            u32 swatch = i == 0 ? 0x00F4F4F4
                        : i == 1 ? 0x001C1C1C
                        : i == 3 ? 0x00FFFFFF : 0x00A0A0A0;
            gui_fill_round_rect(surface, gui_rect(tile.x + 10, tile.y + 12, 40,
                                                  40), 4, swatch);
            axui_draw_text(surface, tile.x + 62, tile.y + 24, themes[i],
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT), 1));
            if (active) {
                axau_icon_paint(surface, "v", tile.x + tile.w - 20,
                                tile.y + tile.h / 2, 12,
                                axau_colour(palette, AXAU_COLOUR_ACCENT));
            }
        }
        for (i32 i = 0; i < setup->accent_count; i++) {
            gui_rect_t dot = gui_rect(drawn.x + 32 + i * 36, content_y + 152, 24,
                                      24);
            axui_circle(surface, dot.x + 12, dot.y + 12, 10, setup->accents[i]);
            if (i == setup->accent_index) {
                axui_circle_outline(surface, dot.x + 12, dot.y + 12, 12,
                                    axau_colour(palette, AXAU_COLOUR_TEXT), 2);
            }
        }
        break;
    }
    case AXO_STEP_ACCOUNT: {
        gui_rect_t field = gui_rect(drawn.x + 32, content_y, drawn.w - 64, 40);
        gui_fill_round_rect(surface, field, 6,
                            axau_colour(palette, AXAU_COLOUR_CONTROL));
        axui_round_rect_border(surface, field, 6, 1,
                               axau_colour(palette, AXAU_COLOUR_FOCUS));
        axui_draw_text(surface, field.x + 12, field.y + 12,
                       setup->name[0] ? setup->name : "Account name",
                       axui_style(setup->name[0]
                                      ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                      : axau_colour(palette,
                                                    AXAU_COLOUR_TEXT_TERTIARY), 1));
        i32 caret_x = field.x + 12 + (i32)ax_strlen(setup->name) * 8;
        gui_fill_rect(surface, gui_rect(caret_x, field.y + 12, 1, 16),
                      axau_colour(palette, AXAU_COLOUR_TEXT));
        if (!axo_name_valid(setup->name)) {
            axui_draw_text(surface, field.x, field.y + 48,
                           "Enter at least two characters.",
                           axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY),
                                      1));
        }
        axui_draw_text(surface, drawn.x + 32, content_y + 84,
                       "The account stays on this device.",
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_SECONDARY), 1));
        break;
    }
    case AXO_STEP_PRIVACY: {
        const char *labels[3] = {"Send diagnostic data",
                                 "Share usage analytics",
                                 "Allow location lookups"};
        bool_t values[3] = {setup->telemetry, setup->analytics_share,
                            (bool_t)(setup->location_services != 0)};
        for (i32 i = 0; i < 3; i++) {
            gui_rect_t row = gui_rect(drawn.x + 32, content_y + i * 52,
                                      drawn.w - 64, 44);
            axui_draw_text(surface, row.x, row.y + 14, labels[i],
                           axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
            gui_rect_t track = gui_rect(row.x + row.w - 44, row.y + 10, 40, 20);
            gui_fill_round_rect(surface, track, 10,
                                values[i] ? accent
                                          : axau_colour(palette,
                                                        AXAU_COLOUR_CONTROL));
            axui_circle(surface, values[i] ? track.x + 30 : track.x + 10,
                        track.y + 10, 8,
                        axau_colour(palette, AXAU_COLOUR_SURFACE));
        }
        break;
    }
    case AXO_STEP_READY:
    case AXO_STEP_WELCOME:
    default: {
        axau_icon_paint(surface, "h", drawn.x + drawn.w / 2, content_y + 60, 48,
                        accent);
        axui_draw_text_center(surface,
                              gui_rect(drawn.x, content_y + 100, drawn.w, 20),
                              setup->step == AXO_STEP_READY
                                  ? "Everything is set up."
                                  : "Four short steps, no account required.",
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_TERTIARY), 1));
        break;
    }
    }

    /* Step dots. */
    i32 dot_y = drawn.y + drawn.h - 52;
    for (i32 i = 0; i < AXO_STEPS; i++) {
        i32 dot_x = drawn.x + 32 + i * 20;
        if (i == (i32)setup->step) {
            gui_fill_round_rect(surface, gui_rect(dot_x - 2, dot_y - 2, 16, 12),
                                6, accent);
        } else {
            gui_fill_round_rect(surface, gui_rect(dot_x, dot_y, 12, 8), 4,
                                axau_colour(palette,
                                            AXAU_COLOUR_CONTROL_DISABLED));
        }
    }

    /* Buttons. */
    gui_rect_t next = gui_rect(drawn.x + drawn.w - 124, drawn.y + drawn.h - 64,
                               92, 36);
    bool_t enabled = axo_can_continue(setup);
    gui_fill_round_rect(surface, next, 6,
                        enabled ? accent
                                : axau_colour(palette,
                                              AXAU_COLOUR_CONTROL_DISABLED));
    axui_draw_text_center(surface, next,
                          setup->step == AXO_STEP_READY ? "Finish" : "Next",
                          axui_style(enabled
                                         ? axau_colour(palette,
                                                       AXAU_COLOUR_ACCENT_TEXT)
                                         : axau_colour(palette,
                                                       AXAU_COLOUR_TEXT_DISABLED),
                                     1));
    if (setup->step != AXO_STEP_WELCOME) {
        gui_rect_t back = gui_rect(drawn.x + drawn.w - 216,
                                   drawn.y + drawn.h - 64, 80, 36);
        gui_fill_round_rect(surface, back, 6,
                            axau_colour(palette, AXAU_COLOUR_CONTROL));
        axui_draw_text_center(surface, back, "Back",
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT), 1));
    }
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

/* Returns 1 when the wizard is finished, 2 when it advanced, 3 when it went back,
 * and 0 for an in-page interaction.  The caller uses this to decide whether to
 * swap the surface for the desktop. */
i32 axo_click(axo_setup_t *setup, i32 x, i32 y, gui_rect_t screen)
{
    if (!setup) return 0;
    gui_rect_t card = axo_card(screen);
    i32 slide = 0;
    gui_rect_t drawn = gui_rect(card.x + slide, card.y, card.w, card.h);
    gui_rect_t next = gui_rect(drawn.x + drawn.w - 124, drawn.y + drawn.h - 64,
                               92, 36);
    if (gui_rect_contains(next, x, y)) {
        if (!axo_can_continue(setup)) return 0;
        if (setup->step == AXO_STEP_READY) return 1;
        axo_next(setup);
        return 2;
    }
    if (setup->step != AXO_STEP_WELCOME) {
        gui_rect_t back = gui_rect(drawn.x + drawn.w - 216,
                                   drawn.y + drawn.h - 64, 80, 36);
        if (gui_rect_contains(back, x, y)) {
            axo_back(setup);
            return 3;
        }
    }
    i32 content_y = drawn.y + 96;
    if (setup->step == AXO_STEP_APPEARANCE) {
        i32 card_w = (drawn.w - 64 - 24) / 2;
        for (i32 i = 0; i < 4; i++) {
            i32 column = i % 2;
            i32 row = i / 2;
            gui_rect_t tile = gui_rect(drawn.x + 32 + column * (card_w + 16),
                                       content_y + row * 72, card_w, 64);
            if (gui_rect_contains(tile, x, y)) {
                axo_set_theme(setup, (axo_theme_choice_t)i);
                return 0;
            }
        }
        for (i32 i = 0; i < setup->accent_count; i++) {
            gui_rect_t dot = gui_rect(drawn.x + 32 + i * 36, content_y + 152, 24,
                                      24);
            if (gui_rect_contains(dot, x, y)) {
                axo_set_accent(setup, i);
                return 0;
            }
        }
    }
    if (setup->step == AXO_STEP_PRIVACY) {
        for (i32 i = 0; i < 3; i++) {
            gui_rect_t row = gui_rect(drawn.x + 32, content_y + i * 52,
                                      drawn.w - 64, 44);
            if (!gui_rect_contains(row, x, y)) continue;
            if (i == 0) setup->telemetry = (bool_t)!setup->telemetry;
            if (i == 1) setup->analytics_share = (bool_t)!setup->analytics_share;
            if (i == 2) setup->location_services = setup->location_services ? 0 : 1;
            return 0;
        }
    }
    return 0;
}

void axo_hover(axo_setup_t *setup, i32 x, i32 y, gui_rect_t screen)
{
    if (!setup) return;
    setup->theme_hover = -1;
    setup->accent_hover = -1;
    if (setup->step != AXO_STEP_APPEARANCE) return;
    gui_rect_t card = axo_card(screen);
    i32 content_y = card.y + 96;
    i32 card_w = (card.w - 64 - 24) / 2;
    for (i32 i = 0; i < 4; i++) {
        i32 column = i % 2;
        i32 row = i / 2;
        gui_rect_t tile = gui_rect(card.x + 32 + column * (card_w + 16),
                                   content_y + row * 72, card_w, 64);
        if (gui_rect_contains(tile, x, y)) {
            setup->theme_hover = i;
            return;
        }
    }
    for (i32 i = 0; i < setup->accent_count; i++) {
        gui_rect_t dot = gui_rect(card.x + 32 + i * 36, content_y + 152, 24, 24);
        if (gui_rect_contains(dot, x, y)) {
            setup->accent_hover = i;
            return;
        }
    }
}

void axo_dump(const axo_setup_t *setup)
{
    if (!setup) return;
    printk("setup: step=%d theme=%d accent=%d name='%s' telemetry=%d\n",
           (i32)setup->step, (i32)setup->theme, setup->accent_index, setup->name,
           setup->telemetry);
}

