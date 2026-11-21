/* AxiomOS - Aurora: remote session viewer.
 *
 * A thin client for the kernel's remote framebuffer protocol: it decodes tile
 * updates into the local surface, tracks latency and bandwidth and offers the
 * usual session controls (scale, clipboard mode, input forwarding).
 */

#include "gui.h"

#define AXRM_MAX_SESSIONS   4
#define AXRM_TILE           32
#define AXRM_MAX_TILES      (40 * 24)
#define AXRM_MAX_STREAMS    2

#define AXRM_C_BG      GUI_RGB(0x12, 0x14, 0x1A)
#define AXRM_C_TOOLBAR GUI_RGB(0x1E, 0x22, 0x2A)
#define AXRM_C_TEXT    GUI_RGB(0xDE, 0xE4, 0xEE)
#define AXRM_C_DIM     GUI_RGB(0x84, 0x8C, 0x9A)
#define AXRM_C_STROKE  GUI_RGB(0x32, 0x38, 0x44)
#define AXRM_C_OK      GUI_RGB(0x5C, 0xC0, 0x86)
#define AXRM_C_WARN    GUI_RGB(0xE0, 0xB0, 0x58)
#define AXRM_C_ERR     GUI_RGB(0xE0, 0x60, 0x58)

enum axrm_quality
{
    AXRM_QUALITY_LOSSLESS = 0,
    AXRM_QUALITY_BALANCED,
    AXRM_QUALITY_SPEED
};

typedef struct axrm_session
{
    char host[40];
    u16  port;
    u16  width;
    u16  height;
    u8   quality;
    u8   scale;
    u32  latency_ms;
    u32  bytes_in;
    u32  bytes_out;
    u32  frames;
    u32  dropped_tiles;
    u32  painted_tiles;
    u8   pending_tiles;
    bool_t connected;
    bool_t clipboard_shared;
    bool_t audio_forward;
    bool_t input_capture;
    u8   buffer[AXRM_TILE * AXRM_TILE * 4];
} axrm_session_t;

typedef struct axrm_client
{
    axrm_session_t sessions[AXRM_MAX_SESSIONS];
    u8   count;
    u8   active;
    u32  clock;
    bool_t ready;
} axrm_client_t;

static axrm_client_t axrm_client;

static void axrm_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axrm_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static i32 axrm_content_width(const axrm_session_t *session)
{
    return (i32)session->width * (session->scale == 0 ? 1 : session->scale) / 100;
}

static i32 axrm_content_height(const axrm_session_t *session)
{
    return (i32)session->height * (session->scale == 0 ? 1 : session->scale) / 100;
}

static void axrm_paint_toolbar(gui_surface_t *surface, const axrm_session_t *session,
                               gui_rect_t bar)
{
    gui_fill_rect(surface, bar, AXRM_C_TOOLBAR);
    gui_text(surface, bar.x + 16, bar.y + 12, session->host, AXRM_C_TEXT, 1);
    gui_text(surface, bar.x + 16, bar.y + 28, "port", AXRM_C_DIM, 1);
    {
        char digits[8];
        u16 port = session->port;
        u32 count = 0;
        u16 tmp = port;
        if (tmp == 0)
            digits[count++] = '0';
        while (tmp > 0)
        {
            digits[count++] = (char)('0' + (tmp % 10));
            tmp /= 10;
        }
        while (count > 0)
            gui_fill_rect(surface, gui_rect(0, 0, 0, 0), GUI_RGB(0, 0, 0));
        AXIOM_UNUSED_VAR(count);
    }
    gui_text(surface, bar.x + 48, bar.y + 28,
             session->connected ? "connected" : "offline",
             session->connected ? AXRM_C_OK : AXRM_C_ERR, 1);
    gui_text(surface, bar.x + bar.w - 260, bar.y + 12, "clipboard", AXRM_C_DIM, 1);
    gui_text(surface, bar.x + bar.w - 180, bar.y + 12, "audio", AXRM_C_DIM, 1);
    gui_text(surface, bar.x + bar.w - 100, bar.y + 12, "input", AXRM_C_DIM, 1);
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y + bar.h - 1, AXRM_C_STROKE);
}

static void axrm_paint_stats(gui_surface_t *surface, const axrm_session_t *session,
                             gui_rect_t bar)
{
    char buffer[32];
    u32 cursor = 0;
    gui_fill_rect(surface, bar, AXRM_C_TOOLBAR);
    buffer[cursor++] = 'l';
    buffer[cursor++] = 'a';
    buffer[cursor++] = 't';
    buffer[cursor++] = ':';
    {
        u32 value = session->latency_ms;
        char digits[8];
        u32 count = 0;
        if (value == 0)
            digits[count++] = '0';
        while (value > 0 && count < 7)
        {
            digits[count++] = (char)('0' + (value % 10));
            value /= 10;
        }
        while (count > 0)
            buffer[cursor++] = digits[--count];
    }
    buffer[cursor++] = 'm';
    buffer[cursor++] = 's';
    buffer[cursor++] = ' ';
    buffer[cursor++] = 'f';
    buffer[cursor++] = '=';
    {
        u32 value = session->frames;
        char digits[8];
        u32 count = 0;
        if (value == 0)
            digits[count++] = '0';
        while (value > 0 && count < 7)
        {
            digits[count++] = (char)('0' + (value % 10));
            value /= 10;
        }
        while (count > 0)
            buffer[cursor++] = digits[--count];
    }
    buffer[cursor] = 0;
    gui_text(surface, bar.x + 12, bar.y + 6, buffer, AXRM_C_DIM, 1);
}

static void axrm_paint_canvas(gui_surface_t *surface, const axrm_session_t *session,
                              gui_rect_t canvas)
{
    i32 width = axrm_content_width(session);
    i32 height = axrm_content_height(session);
    gui_fill_rect(surface, canvas, AXRM_C_BG);
    if (!session->connected)
    {
        gui_text(surface, canvas.x + canvas.w / 2 - 60, canvas.y + canvas.h / 2 - 8,
                 "no session", AXRM_C_DIM, 2);
        return;
    }
    {
        i32 x = canvas.x + (canvas.w - width) / 2;
        i32 y = canvas.y + (canvas.h - height) / 2;
        i32 i;
        i32 j;
        if (width <= 0 || height <= 0)
            return;
        for (i = 0; i < height; i += 8)
        {
            for (j = 0; j < width; j += 8)
            {
                u8 shade = (u8)(0x20 + ((i / 8 + j / 8) % 4) * 6);
                gui_fill_rect(surface, gui_rect(x + j, y + i, 8, 8),
                              GUI_RGB(shade, shade + 2, shade + 8));
            }
        }
    }
}

/* -------------------------------------------------------------- public API */

void axrm_init(axrm_client_t *client)
{
    axrm_zero(client, (u32)sizeof(*client));
    client->clock = 1;
    client->ready = true;
}

u32 axrm_add(axrm_client_t *client, const char *host, u16 port, u16 width, u16 height)
{
    axrm_session_t *session;
    if (client->count >= AXRM_MAX_SESSIONS)
        return 0xFFFFFFFFu;
    session = &client->sessions[client->count];
    axrm_zero(session, (u32)sizeof(*session));
    axrm_copy(session->host, 40, host);
    session->port = port;
    session->width = width;
    session->height = height;
    session->scale = 100;
    session->quality = AXRM_QUALITY_BALANCED;
    client->count++;
    return client->count - 1;
}

bool_t axrm_connect(axrm_client_t *client, u32 index)
{
    if (index >= client->count)
        return false;
    client->sessions[index].connected = true;
    client->sessions[index].bytes_in += 64;
    client->active = (u8)index;
    return true;
}

void axrm_disconnect(axrm_client_t *client, u32 index)
{
    if (index >= client->count)
        return;
    client->sessions[index].connected = false;
    client->sessions[index].pending_tiles = 0;
}

bool_t axrm_apply_tile(axrm_client_t *client, u32 index, u16 x, u16 y,
                       const u8 *payload, u32 length)
{
    axrm_session_t *session;
    if (index >= client->count)
        return false;
    session = &client->sessions[index];
    if (!session->connected)
        return false;
    if (x >= session->width || y >= session->height)
    {
        session->dropped_tiles++;
        return false;
    }
    if (length > AXRM_TILE * AXRM_TILE * 4)
    {
        session->dropped_tiles++;
        return false;
    }
    {
        u32 i;
        for (i = 0; i < length; i++)
            session->buffer[i] = payload[i];
    }
    session->painted_tiles++;
    session->bytes_in += length;
    if (session->pending_tiles > 0)
        session->pending_tiles--;
    return true;
}

void axrm_begin_frame(axrm_client_t *client, u32 index, u8 tiles)
{
    if (index >= client->count)
        return;
    client->sessions[index].pending_tiles = tiles;
}

void axrm_end_frame(axrm_client_t *client, u32 index, u32 latency_ms)
{
    if (index >= client->count)
        return;
    client->sessions[index].latency_ms = latency_ms;
    client->sessions[index].frames++;
    client->sessions[index].bytes_out += 32;
}

void axrm_set_scale(axrm_client_t *client, u32 index, u8 scale)
{
    if (index >= client->count)
        return;
    client->sessions[index].scale = scale < 50 ? 50 : (scale > 200 ? 200 : scale);
}

void axrm_set_quality(axrm_client_t *client, u32 index, u8 quality)
{
    if (index >= client->count)
        return;
    client->sessions[index].quality = quality > AXRM_QUALITY_SPEED ?
        AXRM_QUALITY_SPEED : quality;
}

void axrm_toggle(axrm_client_t *client, u32 index, u8 what)
{
    if (index >= client->count)
        return;
    switch (what)
    {
    case 0: client->sessions[index].clipboard_shared =
                !client->sessions[index].clipboard_shared; break;
    case 1: client->sessions[index].audio_forward =
                !client->sessions[index].audio_forward; break;
    case 2: client->sessions[index].input_capture =
                !client->sessions[index].input_capture; break;
    default: break;
    }
}

bool_t axrm_key(axrm_client_t *client, u32 index, u16 key, bool_t down)
{
    axrm_session_t *session;
    if (index >= client->count)
        return false;
    session = &client->sessions[index];
    if (!session->connected || !session->input_capture)
        return false;
    session->bytes_out += 6;
    AXIOM_UNUSED_VAR(key);
    AXIOM_UNUSED_VAR(down);
    return true;
}

bool_t axrm_pointer(axrm_client_t *client, u32 index, i32 x, i32 y, bool_t down)
{
    axrm_session_t *session;
    if (index >= client->count)
        return false;
    session = &client->sessions[index];
    if (!session->connected || !session->input_capture)
        return false;
    if (x < 0 || y < 0 || x >= (i32)session->width || y >= (i32)session->height)
        return false;
    session->bytes_out += 8;
    AXIOM_UNUSED_VAR(down);
    return true;
}

void axrm_paint(axrm_client_t *client, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t toolbar = gui_rect(bounds.x, bounds.y, bounds.w, 44);
    gui_rect_t stats = gui_rect(bounds.x, bounds.y + bounds.h - 24, bounds.w, 24);
    gui_rect_t canvas = gui_rect(bounds.x, bounds.y + 44, bounds.w,
                                 bounds.h - 68);
    if (client->count == 0)
    {
        gui_fill_rect(surface, bounds, AXRM_C_BG);
        gui_text(surface, bounds.x + 24, bounds.y + 24, "no sessions", AXRM_C_DIM, 2);
        return;
    }
    if (client->active >= client->count)
        client->active = 0;
    gui_fill_rect(surface, bounds, AXRM_C_BG);
    axrm_paint_toolbar(surface, &client->sessions[client->active], toolbar);
    axrm_paint_canvas(surface, &client->sessions[client->active], canvas);
    axrm_paint_stats(surface, &client->sessions[client->active], stats);
}

void axrm_report(const axrm_client_t *client)
{
    u8 i;
    printk("axrm: sessions=%u active=%u\n", (unsigned)client->count,
           (unsigned)client->active);
    for (i = 0; i < client->count; i++)
    {
        const axrm_session_t *session = &client->sessions[i];
        printk("  %s:%u %ux%u frames=%u in=%u out=%u dropped=%u connected=%u\n",
               session->host, (unsigned)session->port, (unsigned)session->width,
               (unsigned)session->height, (unsigned)session->frames,
               (unsigned)session->bytes_in, (unsigned)session->bytes_out,
               (unsigned)session->dropped_tiles, (unsigned)session->connected);
    }
}

bool_t axrm_selftest(void)
{
    axrm_client_t client;
    u8 tile[64];
    u32 i;
    axrm_init(&client);
    if (axrm_add(&client, "desk.local", 5900, 1920, 1080) != 0)
        return false;
    if (client.count != 1)
        return false;
    if (!axrm_connect(&client, 0))
        return false;
    for (i = 0; i < 64; i++)
        tile[i] = (u8)i;
    if (!axrm_apply_tile(&client, 0, 0, 0, tile, 64))
        return false;
    axrm_begin_frame(&client, 0, 4);
    axrm_end_frame(&client, 0, 18);
    if (client.sessions[0].frames != 1)
        return false;
    if (client.sessions[0].latency_ms != 18)
        return false;
    axrm_set_scale(&client, 0, 250);
    if (client.sessions[0].scale != 200)
        return false;
    axrm_set_quality(&client, 0, 9);
    if (client.sessions[0].quality != AXRM_QUALITY_SPEED)
        return false;
    axrm_toggle(&client, 0, 2);
    if (!axrm_key(&client, 0, 'a', true))
        return false;
    if (axrm_apply_tile(&client, 0, 2000, 0, tile, 64))
        return false;
    axrm_disconnect(&client, 0);
    if (client.sessions[0].connected)
        return false;
    return true;
}
