/* AxiomOS - CRC-32 table and helpers.
 *
 * Several on-disk and on-wire formats use CRC-32 (PNG chunks in the image
 * decoder, ACPI tables, some filesystem checksums).  The table is built once and
 * both the bitwise and slicing-by-one forms are exposed so callers can pick the
 * cheaper path for small buffers.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXCR_POLY        0xEDB88320u
#define AXCR_TABLE_SIZE  256

static u32 axcr_table[AXCR_TABLE_SIZE];
static u32 axcr_built;
static u32 axcr_calls;
static u32 axcr_bytes;

static void axcr_build(void)
{
    u32 i;
    if (axcr_built)
        return;
    for (i = 0; i < AXCR_TABLE_SIZE; i++)
    {
        u32 value = i;
        u32 bit;
        for (bit = 0; bit < 8; bit++)
            value = (value & 1) ? (value >> 1) ^ AXCR_POLY : (value >> 1);
        axcr_table[i] = value;
    }
    axcr_built = 1;
}

u32 axcr32(const u8 *data, u32 length)
{
    u32 crc = 0xFFFFFFFFu;
    u32 i;
    axcr_build();
    axcr_calls++;
    axcr_bytes += length;
    for (i = 0; i < length; i++)
        crc = axcr_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

u32 axcr32_update(u32 crc, const u8 *data, u32 length)
{
    u32 i;
    axcr_build();
    for (i = 0; i < length; i++)
        crc = axcr_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

bool_t axcr32_verify(const u8 *data, u32 length, u32 expected)
{
    return axcr32(data, length) == expected;
}

u32 axcr32_calls(void)
{
    return axcr_calls;
}

u32 axcr32_bytes(void)
{
    return axcr_bytes;
}

bool_t axcr32_selftest(void)
{
    /* "123456789" has the well known CRC-32 value 0xCBF43926. */
    if (axcr32((const u8 *)"123456789", 9) != 0xCBF43926u)
        return false;
    if (axcr32((const u8 *)"", 0) != 0)
        return false;
    if (!axcr32_verify((const u8 *)"123456789", 9, 0xCBF43926u))
        return false;
    return true;
}
