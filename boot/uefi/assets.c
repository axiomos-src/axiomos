/* AxiomOS UEFI loader - boot assets and embedded resource table.
 *
 * The loader ships its splash art, the vector wordmark and the boot font as
 * compact blobs appended to the loader image.  This module validates the asset
 * container, resolves names against the table, and decodes the RLE encoded
 * bitmaps directly into the GOP framebuffer without pulling in a decoders
 * library.  Missing assets are reported as non fatal so a headless boot with a
 * minimal image still succeeds.
 */

#include "loader.h"

#define AXAS_MAGIC            0x41585341u /* "AXSA" */
#define AXAS_VERSION          2
#define AXAS_MAX_ASSETS       48
#define AXAS_NAME_LENGTH      24
#define AXAS_MAX_BLOB         0x10000
#define AXAS_RLE_ESCAPE       0x5A
#define AXAS_DEFAULT_SCALE    2

#define AXAS_KIND_BITMAP      0
#define AXAS_KIND_FONT        1
#define AXAS_KIND_STRING      2
#define AXAS_KIND_PALETTE     3
#define AXAS_KIND_SOUND       4

#define AXAS_STATUS_IDLE      0
#define AXAS_STATUS_LOADED    1
#define AXAS_STATUS_MISSING   2
#define AXAS_STATUS_CORRUPT   3

struct axas_header
{
    u32 magic;
    u16 version;
    u16 asset_count;
    u32 table_offset;
    u32 blob_offset;
    u32 blob_length;
    u32 checksum;
};

struct axas_entry
{
    char name[AXAS_NAME_LENGTH];
    u16 kind;
    u16 flags;
    u32 width;
    u32 height;
    u32 offset;
    u32 length;
    u32 checksum;
};

struct axas_asset
{
    struct axas_entry entry;
    u32  status;
    u32  decoded_pixels;
    u32  decode_errors;
    u32  uses;
    int  loaded;
};

struct axas_state
{
    struct axas_asset assets[AXAS_MAX_ASSETS];
    u32  asset_count;
    u32  loaded_count;
    u32  missing_count;
    u32  corrupt_count;
    u32  bytes_located;
    u32  decode_runs;
    u32  draws;
    int  header_valid;
    int  container_version;
};

static struct axas_state axas_state;

static void axas_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axas_sum(const u8 *data, u32 length)
{
    u32 sum = 0x9E3779B9u;
    for (u32 i = 0; i < length; i++)
        sum = ((sum << 5) - sum) + data[i];
    return sum;
}

static int axas_names_equal(const char *left, const char *right)
{
    for (u32 i = 0; i < AXAS_NAME_LENGTH; i++)
    {
        if (left[i] != right[i])
            return 0;
        if (left[i] == 0)
            return 1;
    }
    return 1;
}

static void axas_copy_name(char *target, const char *source)
{
    u32 i = 0;
    for (; i + 1 < AXAS_NAME_LENGTH && source[i] != 0; i++)
        target[i] = source[i];
    target[i] = 0;
}

/* ---------------------------------------------------------------- lifetime */

int axas_init(const u8 *container, u32 length)
{
    const struct axas_header *header;
    axas_zero(&axas_state, (u32)sizeof(axas_state));
    if (container == 0 || length < (u32)sizeof(struct axas_header))
        return 0;
    header = (const struct axas_header *)container;
    if (header->magic != AXAS_MAGIC || header->version != AXAS_VERSION)
        return 0;
    if (header->asset_count == 0 || header->asset_count > AXAS_MAX_ASSETS)
        return 0;
    if (header->table_offset + header->asset_count * (u32)sizeof(struct axas_entry) > length)
        return 0;
    axas_state.header_valid = 1;
    axas_state.container_version = (int)header->version;
    for (u32 i = 0; i < header->asset_count; i++)
    {
        const struct axas_entry *entry =
            (const struct axas_entry *)(container + header->table_offset +
                                        i * (u32)sizeof(struct axas_entry));
        struct axas_asset *asset = &axas_state.assets[i];
        asset->entry = *entry;
        asset->status = AXAS_STATUS_IDLE;
        if (entry->length > AXAS_MAX_BLOB)
        {
            asset->status = AXAS_STATUS_CORRUPT;
            axas_state.corrupt_count++;
            continue;
        }
        if (entry->offset + entry->length > length)
        {
            asset->status = AXAS_STATUS_MISSING;
            axas_state.missing_count++;
            continue;
        }
        asset->status = AXAS_STATUS_LOADED;
        asset->loaded = 1;
        axas_state.loaded_count++;
        axas_state.bytes_located += entry->length;
    }
    axas_state.asset_count = header->asset_count;
    return 1;
}

struct axas_asset *axas_find(const char *name)
{
    for (u32 i = 0; i < axas_state.asset_count; i++)
    {
        if (axas_state.assets[i].loaded == 0)
            continue;
        if (axas_names_equal(axas_state.assets[i].entry.name, name))
            return &axas_state.assets[i];
    }
    return 0;
}

const struct axas_entry *axas_entry(const char *name)
{
    struct axas_asset *asset = axas_find(name);
    return asset == 0 ? 0 : &asset->entry;
}

/* --------------------------------------------------------------- decoding */

/* Run length decoder: literal run, then escape 0x5A <count> <byte>. */
u32 axas_decode_rle(const u8 *source, u32 length, u8 *target, u32 capacity)
{
    u32 read = 0;
    u32 written = 0;
    while (read < length)
    {
        u8 token = source[read++];
        if (token == AXAS_RLE_ESCAPE && read + 2 <= length)
        {
            u32 count = source[read++];
            u8 value = source[read++];
            for (u32 i = 0; i < count && written < capacity; i++)
                target[written++] = value;
            axas_state.decode_runs++;
        }
        else
        {
            if (written >= capacity)
                break;
            target[written++] = token;
        }
    }
    return written;
}

u32 axas_decode_bitmap(const u8 *container, struct axas_asset *asset,
                       u8 *pixels, u32 capacity)
{
    u32 needed;
    u32 produced;
    if (container == 0 || asset == 0 || pixels == 0)
        return 0;
    needed = asset->entry.width * asset->entry.height * 4;
    if (needed == 0 || needed > capacity)
    {
        asset->decode_errors++;
        return 0;
    }
    produced = axas_decode_rle(container + asset->entry.offset, asset->entry.length,
                               pixels, needed);
    asset->decoded_pixels = produced / 4;
    asset->uses++;
    return produced;
}

/* Scaled blit with nearest neighbour sampling into a 32 bpp framebuffer. */
void axas_blit_scaled(const u8 *pixels, u32 width, u32 height, u8 *fb,
                      u32 pitch, u32 x, u32 y, u32 scale)
{
    if (pixels == 0 || fb == 0 || scale == 0)
        return;
    for (u32 row = 0; row < height; row++)
    {
        for (u32 column = 0; column < width; column++)
        {
            const u8 *source = pixels + (row * width + column) * 4;
            for (u32 dy = 0; dy < scale; dy++)
            {
                u32 target_row = y + row * scale + dy;
                for (u32 dx = 0; dx < scale; dx++)
                {
                    u32 offset = target_row * pitch + (x + column * scale + dx) * 4;
                    fb[offset + 0] = source[2];
                    fb[offset + 1] = source[1];
                    fb[offset + 2] = source[0];
                    fb[offset + 3] = source[3];
                }
            }
        }
    }
    axas_state.draws++;
}

void axas_blit_alpha(const u8 *pixels, u32 width, u32 height, u8 *fb,
                     u32 pitch, u32 x, u32 y, u32 alpha)
{
    if (pixels == 0 || fb == 0)
        return;
    for (u32 row = 0; row < height; row++)
    {
        for (u32 column = 0; column < width; column++)
        {
            const u8 *source = pixels + (row * width + column) * 4;
            u32 offset = (y + row) * pitch + (x + column) * 4;
            u32 a = ((u32)source[3] * alpha) / 255u;
            fb[offset + 0] = (u8)((source[2] * a + fb[offset + 0] * (255 - a)) / 255);
            fb[offset + 1] = (u8)((source[1] * a + fb[offset + 1] * (255 - a)) / 255);
            fb[offset + 2] = (u8)((source[0] * a + fb[offset + 2] * (255 - a)) / 255);
            fb[offset + 3] = 255;
        }
    }
    axas_state.draws++;
}

u32 axas_font_glyph(const u8 *container, struct axas_asset *font,
                    u8 character, u8 *bitmap, u32 capacity)
{
    u32 index;
    u32 offset;
    u32 length;
    if (container == 0 || font == 0 || bitmap == 0)
        return 0;
    if (font->entry.kind != AXAS_KIND_FONT)
        return 0;
    if (character < 32 || character > 126)
        return 0;
    index = (u32)(character - 32);
    offset = font->entry.offset + index * 16;
    length = 16;
    if (offset + length > (u32)(font->entry.offset + font->entry.length))
    {
        font->decode_errors++;
        return 0;
    }
    if (length > capacity)
        return 0;
    for (u32 i = 0; i < length; i++)
        bitmap[i] = container[offset + i];
    font->uses++;
    return length;
}

u32 axas_glyph_row(u8 bitmap_row, u32 y, u32 scale)
{
    u32 offset = 0;
    for (u32 bit = 0; bit < 8; bit++)
    {
        if ((bitmap_row >> (7 - bit)) & 1)
            offset |= 1u << (bit * scale);
    }
    (void)y;
    return offset;
}

/* ------------------------------------------------------------ aggregation */

u32 axas_loaded_count(void)
{
    return axas_state.loaded_count;
}

u32 axas_asset_count(void)
{
    return axas_state.asset_count;
}

u32 axas_missing_count(void)
{
    return axas_state.missing_count;
}

u32 axas_corrupt_count(void)
{
    return axas_state.corrupt_count;
}

u32 axas_bytes(void)
{
    return axas_state.bytes_located;
}

void axas_status(char *text, u32 capacity)
{
    if (text == 0 || capacity < 16)
        return;
    text[0] = (char)('0' + axas_state.loaded_count / 10 % 10);
    text[1] = (char)('0' + axas_state.loaded_count % 10);
    text[2] = '/';
    text[3] = (char)('0' + axas_state.asset_count / 10 % 10);
    text[4] = (char)('0' + axas_state.asset_count % 10);
    text[5] = ' ';
    text[6] = 'a';
    text[7] = 's';
    text[8] = 's';
    text[9] = 'e';
    text[10] = 't';
    text[11] = 's';
    text[12] = 0;
    if (capacity > 15)
        text[13] = 0;
}

void axas_report(void)
{
    con_puts("assets: ");
    con_dec(axas_state.loaded_count);
    con_puts("/");
    con_dec(axas_state.asset_count);
    con_puts(" loaded, missing=");
    con_dec(axas_state.missing_count);
    con_puts(" corrupt=");
    con_dec(axas_state.corrupt_count);
    con_puts(" bytes=");
    con_dec(axas_state.bytes_located);
    con_puts(" draws=");
    con_dec(axas_state.draws);
    con_puts("\r\n");
    for (u32 i = 0; i < axas_state.asset_count; i++)
    {
        struct axas_asset *asset = &axas_state.assets[i];
        if (asset->loaded == 0)
            continue;
        con_puts("  ");
        con_puts(asset->entry.name);
        con_puts(" kind=");
        con_dec(asset->entry.kind);
        con_puts(" ");
        con_dec(asset->entry.width);
        con_puts("x");
        con_dec(asset->entry.height);
        con_puts(" bytes=");
        con_dec(asset->entry.length);
        con_puts("\r\n");
    }
}

int axas_selftest(void)
{
    u8 blob[256];
    u8 container[512];
    u8 pixels[64];
    u8 glyph[16];
    struct axas_header *header;
    struct axas_entry *entry;
    struct axas_asset *asset;
    u32 produced;
    axas_zero(blob, 256);
    axas_zero(container, 512);
    /* Two encoded pixels plus one run of six. */
    blob[0] = 0x11;
    blob[1] = 0x22;
    blob[2] = AXAS_RLE_ESCAPE;
    blob[3] = 6;
    blob[4] = 0x33;
    header = (struct axas_header *)container;
    header->magic = AXAS_MAGIC;
    header->version = AXAS_VERSION;
    header->asset_count = 2;
    header->table_offset = (u32)sizeof(struct axas_header);
    entry = (struct axas_entry *)(container + header->table_offset);
    axas_copy_name(entry->name, "splash");
    entry->kind = AXAS_KIND_BITMAP;
    entry->width = 8;
    entry->height = 1;
    entry->offset = 128;
    entry->length = 5;
    for (u32 i = 0; i < 5; i++)
        container[128 + i] = blob[i];
    entry = (struct axas_entry *)(container + header->table_offset + (u32)sizeof(struct axas_entry));
    axas_copy_name(entry->name, "font8");
    entry->kind = AXAS_KIND_FONT;
    entry->width = 8;
    entry->height = 16;
    entry->offset = 140;
    entry->length = 16 * 95;
    if (!axas_init(container, 512))
        return 0;
    if (axas_asset_count() != 2)
        return 0;
    if (axas_loaded_count() != 1)
        return 0;
    if (axas_missing_count() != 1)
        return 0;
    asset = axas_find("splash");
    if (asset == 0)
        return 0;
    produced = axas_decode_bitmap(container, asset, pixels, 64);
    if (produced != 32)
        return 0;
    if (pixels[0] != 0x11 || pixels[3] != 0x22)
        return 0;
    if (pixels[4] != 0x33 || pixels[7] != 0x33)
        return 0;
    if (axas_decode_rle(blob, 5, pixels, 2) != 2)
        return 0;
    asset = axas_find("font8");
    if (asset != 0)
        return 0;
    if (axas_entry("nope") != 0)
        return 0;
    if (axas_glyph_row(0x01, 0, 1) != 0x80000000u)
        return 0;
    if (axas_glyph_row(0x80, 0, 1) != 1)
        return 0;
    axas_state.draws = 0;
    axas_blit_scaled(pixels, 8, 1, container, 64, 0, 0, 1);
    if (axas_state.draws == 0)
        return 0;
    axas_blit_alpha(pixels, 8, 1, container, 64, 0, 0, 128);
    axas_status(container, 16);
    if (container[2] != '/')
        return 0;
    glyph[0] = 0;
    (void)glyph;
    return 1;
}
