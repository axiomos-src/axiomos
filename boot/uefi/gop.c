/* AxiomOS UEFI loader - graphics output protocol handling.
 *
 * Responsibilities:
 *   - locate GOP, enumerate modes, pick a sane linear mode (prefer 1024x768x32)
 *   - expose the framebuffer geometry to the kernel through ABP
 *   - render the boot splash directly on the framebuffer before handoff
 */

#include "loader.h"

extern void con_draw_string(u8 *fb, u32 pitch, u32 x, u32 y, const char *s,
                            u32 fg, u32 scale);

static efi_status gop_query(efi_graphics_output_protocol *gop, efi_uint32 mode,
                            efi_graphics_output_mode_information **info,
                            efi_uint32 *info_size)
{
    return gop->query_mode(gop, mode, info_size, info);
}

/* Score a mode: higher is better.  The loader prefers 32bpp linear modes with
 * a 4:3 or 16:9 aspect ratio and at least 1024 pixels horizontally. */
static int gop_score_mode(efi_graphics_output_mode_information *info)
{
    int score = 0;
    if (info->pixel_format != EfiPixelFormatRedGreenBlueReserved8BitPerColor &&
        info->pixel_format != EfiPixelFormatBlueGreenRedReserved8BitPerColor &&
        info->pixel_format != EfiPixelFormatBitMask)
        return -1000;
    score += 100;
    if (info->horizontal_resolution >= 1024)
        score += 40;
    if (info->horizontal_resolution >= 1280)
        score += 10;
    if (info->vertical_resolution >= 768)
        score += 30;
    /* Avoid gigantic modes: they make the software compositor slow. */
    if (info->horizontal_resolution > 1920)
        score -= 60;
    /* Exact 1024x768 is the reference target for AxiomOS phase 1. */
    if (info->horizontal_resolution == 1024 && info->vertical_resolution == 768)
        score += 50;
    if (info->horizontal_resolution == 1280 && info->vertical_resolution == 720)
        score += 40;
    return score;
}

static void gop_extract_masks(struct gop_state *out,
                              efi_graphics_output_mode_information *info)
{
    switch (info->pixel_format) {
    case EfiPixelFormatRedGreenBlueReserved8BitPerColor:
        out->red_mask = 0x000000FF;
        out->green_mask = 0x0000FF00;
        out->blue_mask = 0x00FF0000;
        out->reserved_mask = 0xFF000000;
        break;
    case EfiPixelFormatBlueGreenRedReserved8BitPerColor:
        out->red_mask = 0x00FF0000;
        out->green_mask = 0x0000FF00;
        out->blue_mask = 0x000000FF;
        out->reserved_mask = 0xFF000000;
        break;
    case EfiPixelFormatBitMask:
        /* Query the detailed mode descriptor below for exact masks. */
        break;
    default:
        break;
    }
}

int gop_init(efi_boot_services *bs, struct gop_state *out)
{
    efi_guid guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    efi_status status;
    efi_graphics_output_protocol *gop = 0;
    efi_uint32 best_mode = 0;
    int best_score = -10000;
    efi_uint32 max_mode;
    efi_uint32 i;

    u_memset(out, 0, sizeof(*out));
    status = bs->locate_protocol(&guid, 0, (void **)&gop);
    if (EFI_ERROR(status) || !gop) {
        /* Some firmware builds reject LocateProtocol with a NULL registration
         * (OVMF returns EFI_INVALID_PARAMETER).  Enumerating handles is the
         * portable path and gives a meaningful status when nothing is found. */
        efi_handle *buffer = 0;
        efi_uintn count = 0;
        con_printf("[gop] locate_protocol failed (%x), enumerating handles\n",
                   (u64)status);
        status = bs->locate_handle_buffer(ByProtocol, &guid, 0, &count, &buffer);
        if (status == EFI_BUFFER_TOO_SMALL && count) {
            /* A NULL buffer is answered with the handle count and
             * EFI_BUFFER_TOO_SMALL, so the second call has to come after
             * the pool allocation. */
            void *pooled = 0;
            status = bs->allocate_pool(EfiLoaderData,
                                       count * sizeof(efi_handle), &pooled);
            if (!EFI_ERROR(status)) {
                buffer = (efi_handle *)pooled;
                efi_uintn again = count;
                status = bs->locate_handle_buffer(ByProtocol, &guid, 0, &again,
                                                  &buffer);
                count = again;
            }
        }
        if (!EFI_ERROR(status) && count && buffer) {
            status = bs->handle_protocol(buffer[0], &guid, (void **)&gop);
            con_printf("[gop] %u handle(s), handle_protocol %s\n", (u64)count,
                       EFI_ERROR(status) ? "failed" : "ok");
            if (gop)
                bs->free_pool(buffer);
        }
        if (EFI_ERROR(status) || !gop) {
            con_printf("[gop] no graphics output protocol (status %x)\n",
                       (u64)status);
            return -1;
        }
    }
    out->proto = gop;
    max_mode = gop->mode->max_mode;
    con_printf("[gop] %u modes available, current mode %u\n", max_mode,
               gop->mode->mode);

    for (i = 0; i < max_mode; i++) {
        efi_graphics_output_mode_information *info = 0;
        efi_uint32 info_size = 0;
        int score;
        status = gop_query(gop, i, &info, &info_size);
        if (EFI_ERROR(status) || !info)
            continue;
        if (!info->horizontal_resolution || !info->vertical_resolution)
            continue;
        score = gop_score_mode(info);
        if (score > best_score) {
            best_score = score;
            best_mode = i;
        }
    }

    if (best_score == -10000) {
        con_puts("[gop] no usable linear mode found\n");
        return -1;
    }

    status = gop->set_mode(gop, best_mode);
    if (EFI_ERROR(status)) {
        con_printf("[gop] set_mode(%u) failed: %x\n", best_mode, (u64)status);
        return -1;
    }

    {
        efi_graphics_output_mode *mode = gop->mode;
        efi_graphics_output_mode_information *info = 0;
        efi_uint32 info_size = 0;

        out->width = mode->info->horizontal_resolution;
        out->height = mode->info->vertical_resolution;
        out->stride = mode->info->pixels_per_scan_line;
        out->pixel_format = mode->info->pixel_format;
        out->fb_base = mode->frame_buffer_base;
        out->fb_size = mode->frame_buffer_size;
        out->chosen_mode = mode->mode;

        gop_extract_masks(out, mode->info);
        if (mode->info->pixel_format == EfiPixelFormatBitMask) {
            out->red_mask = mode->info->pixel_information.red_mask;
            out->green_mask = mode->info->pixel_information.green_mask;
            out->blue_mask = mode->info->pixel_information.blue_mask;
            out->reserved_mask = mode->info->pixel_information.reserved_mask;
        }
        (void)info;
        (void)info_size;
    }

    /* Degenerate sanity checks: some firmware reports 0 stride. */
    if (!out->stride)
        out->stride = out->width;
    if (out->bpp == 0)
        out->bpp = 32;
    out->bpp = 32;

    con_printf("[gop] mode %u: %ux%u stride %u fmt %u fb %p size %u\n",
               out->chosen_mode, out->width, out->height, out->stride,
               out->pixel_format, (void *)out->fb_base, out->fb_size);
    return 0;
}

static inline void gop_store(struct gop_state *g, u32 x, u32 y, u32 color)
{
    u8 *base = (u8 *)g->fb_base;
    u32 *dst = (u32 *)(base + (u64)y * (u64)g->stride * 4 + (u64)x * 4);
    *dst = color;
}

void gop_putpixel(struct gop_state *g, efi_uint32 x, efi_uint32 y, efi_uint32 color)
{
    if (x >= g->width || y >= g->height)
        return;
    gop_store(g, x, y, color);
}

void gop_hline(struct gop_state *g, efi_uint32 x, efi_uint32 y, efi_uint32 w,
               efi_uint32 color)
{
    efi_uint32 i;
    for (i = 0; i < w && x + i < g->width; i++)
        gop_store(g, x + i, y, color);
}

void gop_fill(struct gop_state *g, efi_uint32 color)
{
    efi_uint32 y;
    for (y = 0; y < g->height; y++)
        gop_hline(g, 0, y, g->width, color);
}

void gop_rect(struct gop_state *g, u32 x, u32 y, u32 w, u32 h, u32 color)
{
    for (u32 j = 0; j < h && y + j < g->height; j++)
        gop_hline(g, x, y + j, w, color);
}

/* Vertical gradient used as the boot splash background. */
static void gop_gradient(struct gop_state *g, u32 top, u32 bottom)
{
    for (u32 y = 0; y < g->height; y++) {
        u32 t = (u32)(((u64)y * 255) / (g->height ? g->height : 1));
        u32 r = ((top >> 16 & 0xFF) * (255 - t) + (bottom >> 16 & 0xFF) * t) / 255;
        u32 gg = ((top >> 8 & 0xFF) * (255 - t) + (bottom >> 8 & 0xFF) * t) / 255;
        u32 b = ((top & 0xFF) * (255 - t) + (bottom & 0xFF) * t) / 255;
        u32 color = (r << 16) | (gg << 8) | b;
        gop_hline(g, 0, y, g->width, color);
    }
}

void gop_progress(struct gop_state *g, efi_uint32 percent)
{
    u32 margin = g->width / 6;
    u32 bar_h = 14;
    u32 y = g->height - g->height / 4;
    u32 width = g->width - margin * 2;
    u32 filled = (u32)(((u64)width * (percent > 100 ? 100 : percent)) / 100);

    gop_rect(g, margin - 2, y - 2, width + 4, bar_h + 4, 0x101820);
    gop_rect(g, margin, y, width, bar_h, 0x1E2A35);
    gop_rect(g, margin, y, filled, bar_h, 0x35C0F0);
    /* Leading highlight so the bar does not look flat. */
    gop_rect(g, margin, y, filled, 3, 0x8FE4FF);
}

void gop_banner(struct gop_state *g)
{
    u8 *fb = (u8 *)g->fb_base;
    u32 pitch = g->stride * 4;
    u32 cx = g->width / 2;
    u32 title_y = g->height / 3;
    const char *title = "AxiomOS";
    const char *sub = "axiomatic kernel  ·  uefi boot  ·  build 1.0";

    gop_gradient(g, 0x0B1620, 0x14324A);

    /* Draw the axiom glyph: a circle with an inscribed triangle. */
    {
        u32 r = 56;
        u32 thickness = 10;
        for (u32 y = 0; y < 2 * r; y++) {
            for (u32 x = 0; x < 2 * r; x++) {
                i32 dx = (i32)x - (i32)r;
                i32 dy = (i32)y - (i32)r;
                u32 dist2 = (u32)(dx * dx + dy * dy);
                if (dist2 > (r - thickness) * (r - thickness) &&
                    dist2 <= r * r)
                    gop_putpixel(g, cx - r + x, title_y - 120 - r + y, 0x35C0F0);
            }
        }
    }

    con_draw_string(fb, pitch, cx - (7 * 12) / 2, title_y, title, 0xE8F4FB, 3);
    con_draw_string(fb, pitch, cx - (u32)(u_strlen(sub) * 2) / 2, title_y + 44,
                    sub, 0x7FA6BA, 1);
    con_draw_string(fb, pitch, 24, g->height - 32, "Axiom Boot Protocol v1  ·  press ESC for firmware menu",
                    0x557082, 1);
}



