/* AxiomOS UEFI loader - boot splash and progress presentation.
 *
 * Drawing happens directly into the linear framebuffer with the GOP state, so
 * the splash survives even when console output is disabled.  The design mirrors
 * the Aurora design language: dark background, a single accent hue, generous
 * spacing and a thin progress rule rather than a spinner.
 */

#include "loader.h"

#define AXSP_BAND_HEIGHT   6
#define AXSP_MAX_BANDS     12
#define AXSP_LOGO_SIZE     96
#define AXSP_TEXT_MARGIN   32

typedef struct axsp_theme
{
    u32 background;
    u32 surface;
    u32 accent;
    u32 text;
    u32 dim;
    u32 track;
} axsp_theme_t;

typedef struct axsp_splash
{
    axsp_theme_t theme;
    u32  width;
    u32  height;
    u32  progress;
    u32  stage;
    int visible;
    int dirty;
    u32  frames;
    char title[40];
    char subtitle[64];
    u8 *framebuffer;
    u32  pitch;
} axsp_splash_t;

static axsp_splash_t axsp_splash;

static void axsp_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axsp_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static u32 axsp_mix(u32 a, u32 b, u32 weight)
{
    u32 ar = (a >> 16) & 0xFF;
    u32 ag = (a >> 8) & 0xFF;
    u32 ab = a & 0xFF;
    u32 br = (b >> 16) & 0xFF;
    u32 bg = (b >> 8) & 0xFF;
    u32 bb = b & 0xFF;
    u32 r = (ar * (256 - weight) + br * weight) / 256;
    u32 g = (ag * (256 - weight) + bg * weight) / 256;
    u32 bl = (ab * (256 - weight) + bb * weight) / 256;
    return (r << 16) | (g << 8) | bl;
}

static void axsp_fill(struct gop_state *gop, u32 x, u32 y, u32 w, u32 h, u32 colour)
{
    if (gop == 0)
        return;
    gop_rect(gop, x, y, w, h, colour);
}

static void axsp_gradient(struct gop_state *gop, u32 x, u32 y, u32 w, u32 h,
                           u32 top, u32 bottom)
{
    u32 row;
    if (gop == 0 || h == 0)
        return;
    for (row = 0; row < h; row++)
    {
        u32 shade = axsp_mix(top, bottom, row * 256 / h);
        axsp_fill(gop, x, y + row, w, 1, shade);
    }
}

static void axsp_logo(struct gop_state *gop, u32 x, u32 y, u32 size, u32 accent)
{
    u32 i;
    u32 j;
    u32 radius = size / 2;
    for (i = 0; i < size; i++)
    {
        for (j = 0; j < size; j++)
        {
            i32 dx = (i32)i - (i32)radius;
            i32 dy = (i32)j - (i32)radius;
            u32 distance = (u32)(dx * dx + dy * dy);
            u32 limit = radius * radius;
            if (distance <= limit && distance > limit / 2)
            {
                u32 shade = axsp_mix(accent, 0xFFFFFF, (limit - distance) / 8);
                axsp_fill(gop, x + j, y + i, 1, 1, shade);
            }
        }
    }
    axsp_fill(gop, x + radius - 3, y + radius - 3, 6, 6, 0xFFFFFF);
}

void axsp_init(struct gop_state *gop, const char *title, const char *subtitle)
{
    axsp_zero(&axsp_splash, sizeof(axsp_splash));
    axsp_splash.theme.background = 0x00101318;
    axsp_splash.theme.surface = 0x00181C24;
    axsp_splash.theme.accent = 0x0059A8FF;
    axsp_splash.theme.text = 0x00E8ECF5;
    axsp_splash.theme.dim = 0x00848997;
    axsp_splash.theme.track = 0x00262C36;
    if (gop != 0)
    {
        axsp_splash.width = gop->width;
        axsp_splash.height = gop->height;
        axsp_splash.framebuffer = (u8 *)gop->fb_base;
        axsp_splash.pitch = gop->stride;
    }
    axsp_copy(axsp_splash.title, 40, title);
    axsp_copy(axsp_splash.subtitle, 64, subtitle);
    axsp_splash.visible = 1;
    axsp_splash.dirty = 1;
}

void axsp_draw(struct gop_state *gop, u32 progress, u8 stage)
{
    const axsp_theme_t *theme = &axsp_splash.theme;
    u32 centre_x;
    u32 logo_y;
    u32 text_y;
    u32 rule_y;
    u32 band;
    if (gop == 0)
        return;
    axsp_splash.width = gop->width;
    axsp_splash.height = gop->height;
    axsp_fill(gop, 0, 0, gop->width, gop->height, theme->background);
    axsp_gradient(gop, 0, 0, gop->width, gop->height / 3, theme->surface,
                  theme->background);
    centre_x = gop->width / 2;
    logo_y = gop->height / 3;
    axsp_logo(gop, centre_x - AXSP_LOGO_SIZE / 2, logo_y, AXSP_LOGO_SIZE,
              theme->accent);
    text_y = logo_y + AXSP_LOGO_SIZE + 28;
    con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                    centre_x - (u32)(u_strlen(axsp_splash.title) * 8),
                    text_y, axsp_splash.title, theme->text, 2);
    con_draw_string((u8 *)gop->fb_base, gop->stride * (gop->bpp / 8),
                    centre_x - (u32)(u_strlen(axsp_splash.subtitle) * 4),
                    text_y + 36, axsp_splash.subtitle, theme->dim, 1);
    rule_y = text_y + 78;
    axsp_fill(gop, centre_x - 200, rule_y, 400, 3, theme->track);
    if (progress > 100)
        progress = 100;
    axsp_fill(gop, centre_x - 200, rule_y, 400 * progress / 100, 3, theme->accent);
    for (band = 0; band < AXSP_MAX_BANDS; band++)
    {
        u32 width = 6;
        u32 offset = centre_x - 200 + (u32)band * 34;
        if ((band * 100 / AXSP_MAX_BANDS) > (i32)progress)
            break;
        axsp_fill(gop, offset, rule_y + 12, width, AXSP_BAND_HEIGHT, theme->dim);
    }
    axsp_splash.progress = progress;
    axsp_splash.stage = stage;
    axsp_splash.frames++;
    axsp_splash.dirty = 1;
}

void axsp_message(const char *subtitle)
{
    axsp_copy(axsp_splash.subtitle, 64, subtitle);
    axsp_splash.dirty = 1;
}

void axsp_hide(struct gop_state *gop)
{
    axsp_splash.visible = 0;
    (void)gop;
}

int axsp_visible(void)
{
    return axsp_splash.visible;
}

u32 axsp_progress(void)
{
    return axsp_splash.progress;
}

u32 axsp_frames(void)
{
    return axsp_splash.frames;
}

int axsp_selftest(void)
{
    axsp_splash_t copy;
    axsp_zero(&copy, sizeof(copy));
    if (copy.progress != 0)
        return 0;
    if (axsp_mix(0x000000, 0xFFFFFF, 128) != 0x7F7F7F)
        return 0;
    return 1;
}

