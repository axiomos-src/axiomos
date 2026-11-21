/*
 * AxiomOS - mail surface.
 *
 * Three panes: folders, a message list and a reading pane.  The compose sheet is
 * a fourth state that replaces the reading pane rather than opening a new window,
 * because a compose window without a working send path is a promise the surface
 * cannot keep.
 *
 * Message bodies are stored as plain text lines and wrapped at paint time.  No
 * HTML rendering, no attachment decoding: the surface is complete for what it
 * claims to do, which is reading and composing text mail.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXMA_MAX_MESSAGES  40
#define AXMA_FOLDER_WIDTH 168
#define AXMA_LIST_WIDTH   280
#define AXMA_ROW_HEIGHT    56

typedef enum axma_folder {
    AXMA_FOLDER_INBOX = 0,
    AXMA_FOLDER_STARRED,
    AXMA_FOLDER_SENT,
    AXMA_FOLDER_DRAFTS,
    AXMA_FOLDER_ARCHIVE
} axma_folder_t;

typedef struct axma_message {
    char   from[48];
    char   subject[64];
    char   preview[80];
    char   body[512];
    char   when[20];
    u32    avatar;
    bool_t unread;
    bool_t starred;
    bool_t has_attachment;
    i32    folder;
} axma_message_t;

typedef struct axma_compose {
    bool_t open;
    char   to[64];
    char   subject[64];
    char   body[400];
    i32    caret;
    i32    field;         /* 0 to, 1 subject, 2 body */
    bool_t sendable;
} axma_compose_t;

typedef struct axma_app {
    gui_rect_t     rect;
    axma_message_t messages[AXMA_MAX_MESSAGES];
    i32            count;
    i32            folder;
    i32            selected;
    i32            hover;
    i32            scroll;
    char           search[40];
    axma_compose_t compose;
    i32            folder_unread[5];
    i32            page_ticks;
} axma_app_t;

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

void axma_init(axma_app_t *app, gui_rect_t rect)
{
    if (!app) return;
    ax_memset(app, 0, sizeof(*app));
    app->rect = rect;
    app->selected = -1;
    app->hover = -1;
}

void axma_add(axma_app_t *app, axma_folder_t folder, const char *from,
              const char *subject, const char *preview, const char *body,
              const char *when, u32 avatar, bool_t unread)
{
    if (!app || app->count >= AXMA_MAX_MESSAGES) return;
    axma_message_t *message = &app->messages[app->count++];
    ax_memset(message, 0, sizeof(*message));
    ax_strlcpy(message->from, from ? from : "", sizeof(message->from));
    ax_strlcpy(message->subject, subject ? subject : "", sizeof(message->subject));
    ax_strlcpy(message->preview, preview ? preview : "", sizeof(message->preview));
    ax_strlcpy(message->body, body ? body : "", sizeof(message->body));
    ax_strlcpy(message->when, when ? when : "", sizeof(message->when));
    message->avatar = avatar;
    message->unread = unread;
    message->folder = (i32)folder;
}

static bool_t axma_visible(const axma_app_t *app, const axma_message_t *message)
{
    switch (app->folder) {
    case AXMA_FOLDER_STARRED: return message->starred;
    case AXMA_FOLDER_SENT: return message->folder == (i32)AXMA_FOLDER_SENT;
    case AXMA_FOLDER_DRAFTS: return message->folder == (i32)AXMA_FOLDER_DRAFTS;
    case AXMA_FOLDER_ARCHIVE:
        return message->folder == (i32)AXMA_FOLDER_ARCHIVE;
    case AXMA_FOLDER_INBOX:
    default:
        return message->folder == (i32)AXMA_FOLDER_INBOX;
    }
}

/* A simple substring filter over subject, sender and preview.  It runs on every
 * paint rather than being cached because the list is bounded at forty entries. */
static bool_t axma_search_match(const axma_message_t *message, const char *query)
{
    if (!query || !query[0]) return (bool_t)1;
    const char *fields[3] = {message->subject, message->from, message->preview};
    for (i32 f = 0; f < 3; f++) {
        const char *hay = fields[f];
        while (*hay) {
            const char *a = hay;
            const char *b = query;
            while (*a && *b) {
                char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
                char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
                if (ca != cb) break;
                a++;
                b++;
            }
            if (!*b) return (bool_t)1;
            hay++;
        }
    }
    return (bool_t)0;
}

static i32 axma_list(const axma_app_t *app, i32 *indices)
{
    i32 count = 0;
    for (i32 i = 0; i < app->count; i++) {
        if (!axma_visible(app, &app->messages[i])) continue;
        if (!axma_search_match(&app->messages[i], app->search)) continue;
        indices[count++] = i;
    }
    return count;
}

void axma_set_folder(axma_app_t *app, axma_folder_t folder)
{
    if (!app) return;
    app->folder = (i32)folder;
    app->selected = -1;
    app->scroll = 0;
}

i32 axma_recount_unread(axma_app_t *app)
{
    if (!app) return 0;
    for (i32 f = 0; f < 5; f++) app->folder_unread[f] = 0;
    for (i32 i = 0; i < app->count; i++) {
        if (!app->messages[i].unread) continue;
        i32 folder = app->messages[i].folder;
        if (folder >= 0 && folder < 5) app->folder_unread[folder]++;
    }
    return app->folder_unread[AXMA_FOLDER_INBOX];
}

/* ------------------------------------------------------------------ */
/* Compose                                                             */
/* ------------------------------------------------------------------ */

void axma_compose_open(axma_app_t *app)
{
    if (!app) return;
    ax_memset(&app->compose, 0, sizeof(app->compose));
    app->compose.open = (bool_t)1;
    app->compose.field = 0;
}

void axma_compose_close(axma_app_t *app)
{
    if (!app) return;
    app->compose.open = (bool_t)0;
}

/* Sendability is derived, not stored: a draft is sendable when it has a
 * recipient with an at-sign and a non-empty subject.  Deriving it means the
 * button state can never disagree with the fields. */
static void axma_compose_validate(axma_compose_t *compose)
{
    bool_t has_at = (bool_t)0;
    for (const char *p = compose->to; *p; p++) {
        if (*p == '@') {
            has_at = (bool_t)1;
            break;
        }
    }
    compose->sendable = (bool_t)(has_at && compose->subject[0]);
}

void axma_compose_type(axma_app_t *app, char ch)
{
    if (!app || !app->compose.open) return;
    if (ch == '\t') {
        app->compose.field = (app->compose.field + 1) % 3;
        return;
    }
    if (ch == 0x1B) {
        axma_compose_close(app);
        return;
    }
    char *field = app->compose.field == 0 ? app->compose.to
                 : app->compose.field == 1 ? app->compose.subject
                                            : app->compose.body;
    i32 capacity = app->compose.field == 0 ? (i32)sizeof(app->compose.to)
                 : app->compose.field == 1 ? (i32)sizeof(app->compose.subject)
                                            : (i32)sizeof(app->compose.body);
    i32 length = (i32)ax_strlen(field);
    if (length + 1 < capacity) {
        field[length] = ch;
        field[length + 1] = 0;
    }
    axma_compose_validate(&app->compose);
}

void axma_compose_backspace(axma_app_t *app)
{
    if (!app || !app->compose.open) return;
    char *field = app->compose.field == 0 ? app->compose.to
                 : app->compose.field == 1 ? app->compose.subject
                                            : app->compose.body;
    i32 length = (i32)ax_strlen(field);
    if (length > 0) {
        field[length - 1] = 0;
    }
    axma_compose_validate(&app->compose);
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

/* Bodies are wrapped at a byte budget derived from the pane width rather than at
 * word boundaries: the reading pane is narrow enough that word wrapping would
 * leave rivers of whitespace on long quoted lines. */
static i32 axma_paint_body(gui_surface_t *surface, gui_rect_t rect, const char *body,
                           const axau_palette_t *palette)
{
    i32 columns = (rect.w - 16) / 8;
    if (columns < 8) columns = 8;
    i32 y = rect.y;
    i32 line_start = 0;
    i32 length = (i32)ax_strlen(body);
    for (i32 i = 0; i <= length; i++) {
        bool_t at_end = i == length;
        if (!at_end && body[i] != '\n' && (i - line_start) < columns) continue;
        i32 line_length = i - line_start;
        if (line_length > columns) line_length = columns;
        char line[160];
        if (line_length > (i32)sizeof(line) - 1) line_length = (i32)sizeof(line) - 1;
        for (i32 c = 0; c < line_length; c++) line[c] = body[line_start + c];
        line[line_length] = 0;
        axui_draw_text(surface, rect.x + 8, y, line,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        y += 18;
        if (y + 18 > rect.y + rect.h) break;
        line_start = at_end ? i : i + 1;
    }
    return y;
}

void axma_paint(gui_surface_t *surface, axma_app_t *app,
                const axau_palette_t *palette)
{
    if (!surface || !app || !palette) return;
    gui_fill_rect(surface, app->rect, axau_colour(palette, AXAU_COLOUR_SURFACE));

    /* Folder rail. */
    gui_rect_t rail = gui_rect(app->rect.x, app->rect.y, AXMA_FOLDER_WIDTH,
                               app->rect.h);
    gui_fill_rect(surface, rail, axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    const char *folders[5] = {"Inbox", "Starred", "Sent", "Drafts", "Archive"};
    const char *glyphs[5] = {"e", "m", ">", ".", "="};
    for (i32 i = 0; i < 5; i++) {
        gui_rect_t row = gui_rect(rail.x + 8, rail.y + 16 + i * 40,
                                  rail.w - 16, 36);
        bool_t active = i == app->folder;
        if (active) {
            gui_fill_round_rect(surface, row, 4,
                                axui_colour_opacity(
                                    axau_colour(palette, AXAU_COLOUR_ACCENT), 18));
        }
        axau_icon_paint(surface, glyphs[i], row.x + 16, row.y + 18, 12,
                        active ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                               : axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY));
        axui_draw_text(surface, row.x + 32, row.y + 10, folders[i],
                       axui_style(axau_colour(palette,
                                              active ? AXAU_COLOUR_TEXT
                                                     : AXAU_COLOUR_TEXT_SECONDARY),
                                  1));
        if (app->folder_unread[i] > 0) {
            char count[8];
            ax_snprintf(count, sizeof(count), "%d", app->folder_unread[i]);
            axui_draw_text_right(surface,
                                 gui_rect(row.x, row.y, row.w - 8, row.h), count,
                                 axui_style(axau_colour(palette,
                                                        AXAU_COLOUR_ACCENT), 1));
        }
    }

    /* Compose button pinned to the rail bottom. */
    gui_rect_t compose_button = gui_rect(rail.x + 12, rail.y + rail.h - 52,
                                         rail.w - 24, 36);
    gui_fill_round_rect(surface, compose_button, 6,
                        axau_colour(palette, AXAU_COLOUR_ACCENT));
    axui_draw_text_center(surface, compose_button, "New message",
                          axui_style(axau_colour(palette,
                                                 AXAU_COLOUR_ACCENT_TEXT), 1));

    /* Message list. */
    gui_rect_t list = gui_rect(app->rect.x + AXMA_FOLDER_WIDTH, app->rect.y,
                               AXMA_LIST_WIDTH, app->rect.h);
    gui_fill_rect(surface, list, axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    axau_paint_divider(surface, gui_rect(list.x, list.y, 1, list.h), palette);
    i32 indices[AXMA_MAX_MESSAGES];
    i32 count = axma_list(app, indices);
    i32 visible = (list.h - 8) / AXMA_ROW_HEIGHT;
    for (i32 i = 0; i < count && i < visible + app->scroll; i++) {
        i32 row_index = i - app->scroll;
        if (row_index < 0) continue;
        if (row_index >= visible) break;
        axma_message_t *message = &app->messages[indices[i]];
        gui_rect_t row = gui_rect(list.x + 6, list.y + row_index * AXMA_ROW_HEIGHT,
                                  list.w - 12, AXMA_ROW_HEIGHT - 2);
        if (message->unread || app->hover == i) {
            gui_fill_round_rect(surface, row, 4,
                                message->unread
                                    ? axui_colour_opacity(
                                          axau_colour(palette,
                                                      AXAU_COLOUR_ACCENT), 10)
                                    : axau_colour(palette,
                                                  AXAU_COLOUR_CONTROL_HOVER));
        }
        if (indices[i] == app->selected) {
            gui_fill_round_rect(surface, row, 4,
                                axui_colour_opacity(
                                    axau_colour(palette, AXAU_COLOUR_ACCENT), 18));
        }
        gui_fill_round_rect(surface, gui_rect(row.x + 8, row.y + 10, 28, 28), 14,
                            message->avatar);
        axui_draw_text_center(surface, gui_rect(row.x + 8, row.y + 10, 28, 28),
                              message->from[0] ? message->from : "?",
                              axui_style(0x00FFFFFF, 1));
        axui_draw_text_clip(surface,
                            gui_rect(row.x + 44, row.y + 6, row.w - 76, 16),
                            row.x + 44, row.y + 6, message->from,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_SECONDARY), 1));
        axui_draw_text_right(surface,
                             gui_rect(row.x, row.y + 6, row.w - 10, 16),
                             message->when,
                             axui_style(axau_colour(palette,
                                                    AXAU_COLOUR_TEXT_TERTIARY), 1));
        axui_draw_text_clip(surface,
                            gui_rect(row.x + 44, row.y + 24, row.w - 60, 16),
                            row.x + 44, row.y + 24, message->subject,
                            axui_style(axau_colour(palette,
                                                   message->unread
                                                       ? AXAU_COLOUR_TEXT
                                                       : AXAU_COLOUR_TEXT_SECONDARY),
                                       1));
        axui_draw_text_clip(surface,
                            gui_rect(row.x + 44, row.y + 40, row.w - 60, 14),
                            row.x + 44, row.y + 40, message->preview,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_TERTIARY), 1));
        if (message->starred) {
            axau_icon_paint(surface, "m", row.x + 36, row.y + 8, 10,
                            axau_colour(palette, AXAU_COLOUR_WARNING));
        }
        if (message->has_attachment) {
            axau_icon_paint(surface, "=", row.x + row.w - 16, row.y + row.h - 12,
                            10, axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY));
        }
    }
    if (count == 0) {
        axui_draw_text_center(surface,
                              gui_rect(list.x, list.y + 80, list.w, 20),
                              "No messages",
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_TERTIARY), 1));
    }

    /* Reading pane or compose sheet. */
    gui_rect_t pane = gui_rect(list.x + AXMA_LIST_WIDTH, app->rect.y,
                               app->rect.w - AXMA_FOLDER_WIDTH - AXMA_LIST_WIDTH,
                               app->rect.h);
    gui_fill_rect(surface, pane, axau_colour(palette, AXAU_COLOUR_SURFACE));
    if (app->compose.open) {
        axui_draw_text(surface, pane.x + 16, pane.y + 14, "New message",
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
        const char *labels[3] = {"To", "Subject", "Body"};
        const char *values[3] = {app->compose.to, app->compose.subject,
                                 app->compose.body};
        i32 y = pane.y + 48;
        for (i32 i = 0; i < 2; i++) {
            gui_rect_t field = gui_rect(pane.x + 16, y, pane.w - 32, 34);
            gui_fill_round_rect(surface, field, 4,
                                axau_colour(palette, AXAU_COLOUR_CONTROL));
            if (app->compose.field == i) {
                axui_round_rect_border(surface, field, 4, 1,
                                       axau_colour(palette, AXAU_COLOUR_FOCUS));
            }
            axui_draw_text(surface, field.x + 8, field.y + 9, labels[i],
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT_TERTIARY), 1));
            axui_draw_text_clip(surface,
                                gui_rect(field.x + 80, field.y, field.w - 88, 34),
                                field.x + 80, field.y + 9, values[i],
                                axui_style(axau_colour(palette,
                                                       AXAU_COLOUR_TEXT), 1));
            y += 42;
        }
        gui_rect_t body = gui_rect(pane.x + 16, y, pane.w - 32, pane.h - y - 64);
        gui_fill_round_rect(surface, body, 4,
                            axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
        axui_round_rect_border(surface, body, 4, 1,
                               app->compose.field == 2
                                   ? axau_colour(palette, AXAU_COLOUR_FOCUS)
                                   : axau_colour(palette, AXAU_COLOUR_STROKE));
        axma_paint_body(surface, body, app->compose.body, palette);
        gui_rect_t send = gui_rect(pane.x + pane.w - 108, pane.y + pane.h - 48,
                                   92, 34);
        gui_fill_round_rect(surface, send, 4,
                            app->compose.sendable
                                ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                                : axau_colour(palette,
                                              AXAU_COLOUR_CONTROL_DISABLED));
        axui_draw_text_center(surface, send, "Send",
                              axui_style(app->compose.sendable
                                             ? axau_colour(palette,
                                                           AXAU_COLOUR_ACCENT_TEXT)
                                             : axau_colour(palette,
                                                           AXAU_COLOUR_TEXT_DISABLED),
                                         1));
    } else if (app->selected >= 0 && app->selected < app->count) {
        const axma_message_t *message = &app->messages[app->selected];
        axui_draw_text_clip(surface,
                            gui_rect(pane.x + 16, pane.y + 16, pane.w - 32, 24),
                            pane.x + 16, pane.y + 16, message->subject,
                            axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
        gui_fill_round_rect(surface, gui_rect(pane.x + 16, pane.y + 48, 32, 32), 16,
                            message->avatar);
        axui_draw_text(surface, pane.x + 58, pane.y + 50, message->from,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        axui_draw_text(surface, pane.x + 58, pane.y + 66, message->when,
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_TERTIARY), 1));
        gui_rect_t body = gui_rect(pane.x + 16, pane.y + 92, pane.w - 32,
                                   pane.h - 108);
        axma_paint_body(surface, body, message->body, palette);
    } else {
        axui_draw_text_center(surface,
                              gui_rect(pane.x, pane.y + pane.h / 2 - 10, pane.w, 20),
                              "Select a message",
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_DISABLED), 1));
    }
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

i32 axma_click(axma_app_t *app, i32 x, i32 y)
{
    if (!app) return 0;
    gui_rect_t rail = gui_rect(app->rect.x, app->rect.y, AXMA_FOLDER_WIDTH,
                               app->rect.h);
    if (gui_rect_contains(rail, x, y)) {
        if (y >= rail.y + rail.h - 52) {
            axma_compose_open(app);
            return 1;
        }
        i32 row = (y - rail.y - 16) / 40;
        if (row >= 0 && row < 5) {
            axma_set_folder(app, (axma_folder_t)row);
            return 2;
        }
    }
    gui_rect_t list = gui_rect(app->rect.x + AXMA_FOLDER_WIDTH, app->rect.y,
                               AXMA_LIST_WIDTH, app->rect.h);
    i32 indices[AXMA_MAX_MESSAGES];
    i32 count = axma_list(app, indices);
    if (gui_rect_contains(list, x, y)) {
        i32 row = (y - list.y) / AXMA_ROW_HEIGHT + app->scroll;
        if (row < 0 || row >= count) return 0;
        app->selected = indices[row];
        app->messages[indices[row]].unread = (bool_t)0;
        axma_recount_unread(app);
        return 10 + row;
    }
    if (app->compose.open) {
        gui_rect_t send = gui_rect(app->rect.x + app->rect.w - 108,
                                   app->rect.y + app->rect.h - 48, 92, 34);
        if (gui_rect_contains(send, x, y) && app->compose.sendable) {
            axma_compose_close(app);
            return 100;
        }
        return 5;
    }
    return 0;
}

void axma_hover(axma_app_t *app, i32 x, i32 y)
{
    if (!app) return;
    gui_rect_t list = gui_rect(app->rect.x + AXMA_FOLDER_WIDTH, app->rect.y,
                               AXMA_LIST_WIDTH, app->rect.h);
    app->hover = -1;
    if (!gui_rect_contains(list, x, y)) return;
    app->hover = (y - list.y) / AXMA_ROW_HEIGHT + app->scroll;
}

void axma_scroll(axma_app_t *app, i32 rows)
{
    if (!app) return;
    i32 indices[AXMA_MAX_MESSAGES];
    i32 count = axma_list(app, indices);
    i32 visible = (app->rect.h - 8) / AXMA_ROW_HEIGHT;
    app->scroll += rows;
    if (app->scroll < 0) app->scroll = 0;
    if (app->scroll > count - visible && count > visible) {
        app->scroll = count - visible;
    }
}

void axma_toggle_star(axma_app_t *app, i32 index)
{
    if (!app || index < 0 || index >= app->count) return;
    app->messages[index].starred = (bool_t)!app->messages[index].starred;
}

void axma_search_type(axma_app_t *app, char ch)
{
    if (!app) return;
    if (ch == 0x08) {
        i32 length = (i32)ax_strlen(app->search);
        if (length > 0) app->search[length - 1] = 0;
        return;
    }
    if (ch < 32 || ch > 126) return;
    i32 length = (i32)ax_strlen(app->search);
    if (length + 1 < (i32)sizeof(app->search)) {
        app->search[length] = ch;
        app->search[length + 1] = 0;
    }
}

i32 axma_unread(const axma_app_t *app)
{
    if (!app) return 0;
    i32 count = 0;
    for (i32 i = 0; i < app->count; i++) {
        if (app->messages[i].unread) count++;
    }
    return count;
}

void axma_dump(const axma_app_t *app)
{
    if (!app) return;
    printk("mail: %d messages, folder=%d, selected=%d, unread=%d, composing=%d\n",
           app->count, app->folder, app->selected, axma_unread(app),
           app->compose.open);
}
