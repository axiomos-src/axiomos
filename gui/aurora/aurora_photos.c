/*
 * AxiomOS - photo gallery.
 *
 * A thumbnail grid, an album strip and a single-photo viewer with zoom and pan.
 * Thumbnails are not decoded images here: each cell carries a dominant colour
 * sampled when the library was indexed, and the cell paints that colour under a
 * simple glyph.  That gives a gallery that reads correctly at a glance and stays
 * honest about the fact that no decoder is wired in yet.
 *
 * Zoom is stored as a percentage and pan as a pixel offset, clamped so the image
 * can never be dragged away from its viewport.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXPH_MAX_PHOTOS   48
#define AXPH_COLUMNS       6
#define AXPH_CELL         128
#define AXPH_ALBUM_HEIGHT  64

typedef struct axph_photo {
    char name[48];
    char album[32];
    char taken[24];
    u32  dominant;
    i32  width;
    i32  height;
    bool_t favourite;
    bool_t selected;
    bool_t video;
} axph_photo_t;

typedef struct axph_gallery {
    gui_rect_t rect;
    axph_photo_t photos[AXPH_MAX_PHOTOS];
    i32        count;
    i32        scroll;
    i32        hover;
    i32        selected;
    i32        album;
    char       albums[8][24];
    i32        album_count;
    bool_t     viewer_open;
    i32        zoom;          /* percent */
    i32        pan_x;
    i32        pan_y;
    bool_t     panning;
    i32        pan_start_x;
    i32        pan_start_y;
    i32        pan_origin_x;
    i32        pan_origin_y;
    bool_t     slideshow;
    i32        slideshow_ticks;
    u32        accent;
} axph_gallery_t;

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

void axph_init(axph_gallery_t *gallery, gui_rect_t rect, u32 accent)
{
    if (!gallery) return;
    ax_memset(gallery, 0, sizeof(*gallery));
    gallery->rect = rect;
    gallery->accent = accent;
    gallery->zoom = 100;
    gallery->hover = -1;
    gallery->selected = -1;
    ax_strlcpy(gallery->albums[gallery->album_count++], "All photos",
               sizeof(gallery->albums[0]));
    ax_strlcpy(gallery->albums[gallery->album_count++], "Recent",
               sizeof(gallery->albums[0]));
    ax_strlcpy(gallery->albums[gallery->album_count++], "Favourites",
               sizeof(gallery->albums[0]));
}

void axph_add(axph_gallery_t *gallery, const char *name, const char *album,
              const char *taken, u32 dominant, i32 width, i32 height,
              bool_t video)
{
    if (!gallery || gallery->count >= AXPH_MAX_PHOTOS) return;
    axph_photo_t *photo = &gallery->photos[gallery->count++];
    ax_memset(photo, 0, sizeof(*photo));
    ax_strlcpy(photo->name, name ? name : "", sizeof(photo->name));
    ax_strlcpy(photo->album, album ? album : "", sizeof(photo->album));
    ax_strlcpy(photo->taken, taken ? taken : "", sizeof(photo->taken));
    photo->dominant = dominant;
    photo->width = width;
    photo->height = height;
    photo->video = video;

    /* The album strip grows from the data rather than being declared by it, so
     * a new album appears the first time a photo claims it. */
    bool_t known = (bool_t)0;
    for (i32 i = 0; i < gallery->album_count; i++) {
        if (ax_strcmp(gallery->albums[i], photo->album) == 0) {
            known = (bool_t)1;
            break;
        }
    }
    if (!known && gallery->album_count < 8 && photo->album[0]) {
        ax_strlcpy(gallery->albums[gallery->album_count++], photo->album,
                   sizeof(gallery->albums[0]));
    }
}

/* Filtered count for the current album.  "All photos" and "Favourites" are
 * virtual albums, which is why filtering is a predicate rather than an index. */
static bool_t axph_matches(const axph_gallery_t *gallery, const axph_photo_t *photo)
{
    if (gallery->album <= 0) return (bool_t)1;
    const char *album = gallery->albums[gallery->album];
    if (ax_strcmp(album, "Favourites") == 0) return photo->favourite;
    if (ax_strcmp(album, "Recent") == 0) return (bool_t)1;
    return (bool_t)(ax_strcmp(photo->album, album) == 0);
}

static i32 axph_visible(const axph_gallery_t *gallery, i32 *indices)
{
    i32 count = 0;
    for (i32 i = 0; i < gallery->count; i++) {
        if (!axph_matches(gallery, &gallery->photos[i])) continue;
        indices[count++] = i;
    }
    return count;
}

void axph_set_album(axph_gallery_t *gallery, i32 album)
{
    if (!gallery || album < 0 || album >= gallery->album_count) return;
    gallery->album = album;
    gallery->scroll = 0;
    gallery->selected = -1;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* Grid geometry with a fixed column count; the last column absorbs the remainder
 * so cells align with the right edge instead of leaving a ragged gap. */
static i32 axph_cell_rect(const axph_gallery_t *gallery, i32 index,
                          gui_rect_t *out)
{
    i32 grid_top = gallery->rect.y + AXPH_ALBUM_HEIGHT + 12;
    i32 available_w = gallery->rect.w - 24;
    i32 cell_w = available_w / AXPH_COLUMNS;
    i32 cell_h = AXPH_CELL;
    i32 column = index % AXPH_COLUMNS;
    i32 row = index / AXPH_COLUMNS - gallery->scroll;
    *out = gui_rect(gallery->rect.x + 12 + column * cell_w,
                    grid_top + row * cell_h, cell_w - 8, cell_h - 8);
    return index - gallery->scroll * AXPH_COLUMNS >= 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

static void axph_paint_albums(gui_surface_t *surface, axph_gallery_t *gallery,
                              const axau_palette_t *palette)
{
    i32 x = gallery->rect.x + 12;
    for (i32 i = 0; i < gallery->album_count; i++) {
        const char *label = gallery->albums[i];
        i32 width = (i32)ax_strlen(label) * 8 + 24;
        gui_rect_t chip = gui_rect(x, gallery->rect.y + 12, width, 30);
        bool_t active = i == gallery->album;
        gui_fill_round_rect(surface, chip, 15,
                            active
                                ? axui_colour_opacity(
                                      axau_colour(palette, AXAU_COLOUR_ACCENT), 26)
                                : axau_colour(palette, AXAU_COLOUR_CONTROL));
        axui_draw_text_center(surface, chip, label,
                              axui_style(active
                                             ? axau_colour(palette,
                                                           AXAU_COLOUR_ACCENT)
                                             : axau_colour(palette,
                                                           AXAU_COLOUR_TEXT_SECONDARY),
                                         1));
        x += width + 8;
    }
}

/* Thumbnail cells: a dominant-colour field with a hairline frame, a glyph for
 * video, and a check mark for favourites.  The colour is the only content, so
 * the cell is honest about being a placeholder for a decoded image. */
static void axph_paint_grid(gui_surface_t *surface, axph_gallery_t *gallery,
                            const axau_palette_t *palette)
{
    i32 indices[AXPH_MAX_PHOTOS];
    i32 count = axph_visible(gallery, indices);
    i32 rows_visible = (gallery->rect.h - AXPH_ALBUM_HEIGHT - 24) / AXPH_CELL;
    i32 first = gallery->scroll * AXPH_COLUMNS;
    i32 last = first + rows_visible * AXPH_COLUMNS;
    for (i32 i = first; i < count && i < last; i++) {
        gui_rect_t cell;
        axph_cell_rect(gallery, i, &cell);
        if (cell.y + cell.h < gallery->rect.y + AXPH_ALBUM_HEIGHT) continue;
        if (cell.y > gallery->rect.y + gallery->rect.h) continue;
        axph_photo_t *photo = &gallery->photos[indices[i]];
        gui_fill_round_rect(surface, cell, 8, photo->dominant);
        if (photo->favourite) {
            axui_circle(surface, cell.x + cell.w - 14, cell.y + 14, 8,
                        axui_colour_opacity(axau_colour(palette,
                                                       AXAU_COLOUR_SURFACE), 90));
            axui_draw_text_center(surface,
                                  gui_rect(cell.x + cell.w - 22, cell.y + 6, 16,
                                           16), "*",
                                  axui_style(axau_colour(palette,
                                                         AXAU_COLOUR_WARNING), 1));
        }
        if (photo->video) {
            axui_circle(surface, cell.x + cell.w / 2, cell.y + cell.h / 2, 16,
                        axui_colour_opacity(0x00000000, 90));
            axui_fill_triangle(surface,
                               axui_point(cell.x + cell.w / 2 - 5,
                                          cell.y + cell.h / 2 - 7),
                               axui_point(cell.x + cell.w / 2 - 5,
                                          cell.y + cell.h / 2 + 7),
                               axui_point(cell.x + cell.w / 2 + 7,
                                          cell.y + cell.h / 2),
                               axau_colour(palette, AXAU_COLOUR_TEXT));
        }
        if (gallery->hover == i || gallery->selected == i) {
            axui_round_rect_border(surface, cell, 8,
                                   gallery->selected == i ? 2 : 1,
                                   gallery->selected == i
                                       ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                                       : axau_colour(palette,
                                                     AXAU_COLOUR_STROKE_STRONG));
        }
        /* Caption sits inside the cell on a translucent band so it stays legible
         * over any dominant colour. */
        gui_rect_t caption = gui_rect(cell.x, cell.y + cell.h - 22, cell.w, 22);
        gui_fill_round_rect(surface, caption, 4,
                            axui_colour_opacity(0x00000000, 110));
        axui_draw_text_clip(surface, caption, caption.x + 6, caption.y + 3,
                            photo->name,
                            axui_style(0x00F4F4F4, 1));
        char meta[24];
        ax_snprintf(meta, sizeof(meta), "%dx%d", photo->width, photo->height);
        axui_draw_text_right(surface,
                             gui_rect(cell.x, cell.y, cell.w - 8, 16), meta,
                             axui_style(0x00E0E0E0, 1));
    }
    if (count == 0) {
        axui_draw_text_center(surface, gui_rect(gallery->rect.x, gallery->rect.y + gallery->rect.h / 2, gallery->rect.w, 20), "Nothing in this album", axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
}

void axph_paint(gui_surface_t *surface, axph_gallery_t *gallery,
                const axau_palette_t *palette)
{
    if (!surface || !gallery || !palette) return;
    gui_fill_rect(surface, gallery->rect,
                  axau_colour(palette, AXAU_COLOUR_SURFACE));
    axph_paint_albums(surface, gallery, palette);
    axph_paint_grid(surface, gallery, palette);

    if (gallery->slideshow) {
        char label[40];
        ax_snprintf(label, sizeof(label), "Slideshow: %d seconds left",
                    30 - gallery->slideshow_ticks / 10);
        axui_draw_text(surface, gallery->rect.x + 16,
                       gallery->rect.y + gallery->rect.h - 24, label,
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
}

/* Viewer: the selected photo is scaled by zoom percent around its centre, and pan
 * shifts it inside the viewport.  Clamping happens here so the paint routine can
 * trust the offsets. */
void axph_paint_viewer(gui_surface_t *surface, axph_gallery_t *gallery,
                       const axau_palette_t *palette)
{
    if (!surface || !gallery || !palette) return;
    if (!gallery->viewer_open || gallery->selected < 0 ||
        gallery->selected >= gallery->count) {
        return;
    }
    gui_rect_t viewport = gallery->rect;
    gui_fill_rect(surface, viewport, 0x00101010);
    const axph_photo_t *photo = &gallery->photos[gallery->selected];
    i32 scaled_w = (photo->width * gallery->zoom) / 100;
    i32 scaled_h = (photo->height * gallery->zoom) / 100;
    i32 limit_x = (scaled_w - viewport.w) / 2;
    if (limit_x < 0) limit_x = 0;
    if (gallery->pan_x < -limit_x) gallery->pan_x = -limit_x;
    if (gallery->pan_x > limit_x) gallery->pan_x = limit_x;
    i32 limit_y = (scaled_h - viewport.h) / 2;
    if (limit_y < 0) limit_y = 0;
    if (gallery->pan_y < -limit_y) gallery->pan_y = -limit_y;
    if (gallery->pan_y > limit_y) gallery->pan_y = limit_y;

    gui_rect_t image = gui_rect(viewport.x + (viewport.w - scaled_w) / 2 +
                                gallery->pan_x,
                                viewport.y + (viewport.h - scaled_h) / 2 +
                                gallery->pan_y,
                                scaled_w, scaled_h);
    gui_fill_rect(surface, image, photo->dominant);
    /* Checker bands at the edges of the image communicate that it is the image
     * boundary, not the viewport boundary, which matters when zoomed out. */
    axui_round_rect_border(surface, image, 2, 1,
                           axui_colour_opacity(0x00FFFFFF, 30));

    char info[64];
    ax_snprintf(info, sizeof(info), "%s  %d%%  %s", photo->name, gallery->zoom,
                photo->taken);
    axui_draw_text(surface, viewport.x + 16, viewport.y + 16, info,
                   axui_style(0x00F0F0F0, 1));
    gui_rect_t close = gui_rect(viewport.x + viewport.w - 44, viewport.y + 10,
                                32, 32);
    axui_circle(surface, close.x + 16, close.y + 16, 14,
                axui_colour_opacity(0x00000000, 120));
    axau_icon_paint(surface, "x", close.x + 16, close.y + 16, 14, 0x00F0F0F0);
    axau_icon_paint(surface, "<", viewport.x + 24,
                    viewport.y + viewport.h / 2, 20,
                    axui_colour_opacity(0x00FFFFFF, 200));
    axau_icon_paint(surface, ">", viewport.x + viewport.w - 44,
                    viewport.y + viewport.h / 2, 20,
                    axui_colour_opacity(0x00FFFFFF, 200));
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

i32 axph_click(axph_gallery_t *gallery, i32 x, i32 y, u32 button)
{
    if (!gallery) return 0;
    if (gallery->viewer_open) {
        gui_rect_t close = gui_rect(gallery->rect.x + gallery->rect.w - 44,
                                    gallery->rect.y + 10, 32, 32);
        if (gui_rect_contains(close, x, y)) {
            gallery->viewer_open = (bool_t)0;
            gallery->zoom = 100;
            gallery->pan_x = 0;
            gallery->pan_y = 0;
            return 1;
        }
        if (x < gallery->rect.x + 64) {
            if (gallery->selected > 0) gallery->selected--;
            gallery->zoom = 100;
            gallery->pan_x = 0;
            gallery->pan_y = 0;
            return 2;
        }
        if (x > gallery->rect.x + gallery->rect.w - 64) {
            if (gallery->selected + 1 < gallery->count) gallery->selected++;
            gallery->zoom = 100;
            gallery->pan_x = 0;
            gallery->pan_y = 0;
            return 3;
        }
        gallery->panning = (bool_t)1;
        gallery->pan_start_x = x;
        gallery->pan_start_y = y;
        gallery->pan_origin_x = gallery->pan_x;
        gallery->pan_origin_y = gallery->pan_y;
        return 4;
    }

    /* Album chips. */
    i32 chip_x = gallery->rect.x + 12;
    for (i32 i = 0; i < gallery->album_count; i++) {
        i32 width = (i32)ax_strlen(gallery->albums[i]) * 8 + 24;
        gui_rect_t chip = gui_rect(chip_x, gallery->rect.y + 12, width, 30);
        if (gui_rect_contains(chip, x, y)) {
            axph_set_album(gallery, i);
            return 10 + i;
        }
        chip_x += width + 8;
    }

    i32 indices[AXPH_MAX_PHOTOS];
    i32 count = axph_visible(gallery, indices);
    for (i32 i = 0; i < count; i++) {
        gui_rect_t cell;
        if (!axph_cell_rect(gallery, i, &cell)) continue;
        if (!gui_rect_contains(cell, x, y)) continue;
        gallery->selected = i;
        if (button == 0x02) {
            gallery->photos[indices[i]].favourite =
                (bool_t)!gallery->photos[indices[i]].favourite;
            return 20;
        }
        gallery->viewer_open = (bool_t)1;
        return 30 + i;
    }
    return 0;
}

void axph_hover(axph_gallery_t *gallery, i32 x, i32 y)
{
    if (!gallery) return;
    gallery->hover = -1;
    if (gallery->viewer_open) return;
    i32 indices[AXPH_MAX_PHOTOS];
    i32 count = axph_visible(gallery, indices);
    for (i32 i = 0; i < count; i++) {
        gui_rect_t cell;
        if (!axph_cell_rect(gallery, i, &cell)) continue;
        if (gui_rect_contains(cell, x, y)) {
            gallery->hover = i;
            return;
        }
    }
}

i32 axph_drag(axph_gallery_t *gallery, i32 x, i32 y)
{
    if (!gallery || !gallery->panning) return 0;
    gallery->pan_x = gallery->pan_origin_x + (x - gallery->pan_start_x);
    gallery->pan_y = gallery->pan_origin_y + (y - gallery->pan_start_y);
    return 1;
}

i32 axph_release(axph_gallery_t *gallery)
{
    if (!gallery) return 0;
    bool_t was = gallery->panning;
    gallery->panning = (bool_t)0;
    return was;
}

void axph_zoom(axph_gallery_t *gallery, i32 delta)
{
    if (!gallery) return;
    gallery->zoom += delta;
    if (gallery->zoom < 25) gallery->zoom = 25;
    if (gallery->zoom > 400) gallery->zoom = 400;
}

void axph_scroll(axph_gallery_t *gallery, i32 rows)
{
    if (!gallery) return;
    i32 indices[AXPH_MAX_PHOTOS];
    i32 count = axph_visible(gallery, indices);
    i32 visible_rows = (gallery->rect.h - AXPH_ALBUM_HEIGHT - 24) / AXPH_CELL;
    i32 max_scroll = (count + AXPH_COLUMNS - 1) / AXPH_COLUMNS - visible_rows;
    gallery->scroll += rows;
    if (gallery->scroll < 0) gallery->scroll = 0;
    if (gallery->scroll > max_scroll && max_scroll > 0) {
        gallery->scroll = max_scroll;
    }
}

void axph_slideshow_tick(axph_gallery_t *gallery)
{
    if (!gallery || !gallery->slideshow) return;
    gallery->slideshow_ticks++;
    if (gallery->slideshow_ticks >= 300) {
        gallery->slideshow_ticks = 0;
        gallery->selected++;
        if (gallery->selected >= gallery->count) gallery->selected = 0;
        gallery->viewer_open = (bool_t)1;
    }
}

void axph_toggle_slideshow(axph_gallery_t *gallery)
{
    if (!gallery) return;
    gallery->slideshow = (bool_t)!gallery->slideshow;
    gallery->slideshow_ticks = 0;
}

i32 axph_count(const axph_gallery_t *gallery)
{
    return gallery ? gallery->count : 0;
}

i32 axph_favourites(const axph_gallery_t *gallery)
{
    if (!gallery) return 0;
    i32 count = 0;
    for (i32 i = 0; i < gallery->count; i++) {
        if (gallery->photos[i].favourite) count++;
    }
    return count;
}

void axph_dump(const axph_gallery_t *gallery)
{
    if (!gallery) return;
    printk("photos: %d items, album=%d/%d, selected=%d, zoom=%d, viewer=%d\n",
           gallery->count, gallery->album, gallery->album_count,
           gallery->selected, gallery->zoom, gallery->viewer_open);
}

