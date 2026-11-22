/* AxiomOS - real time clock (CMOS) driver.
 *
 * Reads the MC146818-compatible CMOS clock through ports 0x70/0x71.  The
 * century register is used when the firmware left a usable value, otherwise
 * 20 is assumed for two digit years.  Values are returned in binary, not BCD,
 * and converted to a UNIX timestamp with a self-contained civil-date
 * algorithm so the kernel does not depend on libc's time functions.
 */

#include <axiom/kernel.h>

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

#define CMOS_SECOND   0x00
#define CMOS_MINUTE   0x02
#define CMOS_HOUR     0x04
#define CMOS_DAY      0x07
#define CMOS_MONTH    0x08
#define CMOS_YEAR     0x09
#define CMOS_CENTURY  0x32
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B

static u64 g_boot_unix;
static bool_t g_rtc_ready;

extern u8  io_inb(u16 port);
extern void io_outb(u16 port, u8 value);

static u8 cmos_read(u8 reg)
{
    io_outb(CMOS_ADDRESS, reg);
    return io_inb(CMOS_DATA);
}

static void cmos_write(u8 reg, u8 value)
{
    io_outb(CMOS_ADDRESS, reg);
    io_outb(CMOS_DATA, value);
}

static bool_t update_in_progress(void)
{
    return (cmos_read(CMOS_STATUS_A) & 0x80) != 0;
}

static u8 bcd_to_binary(u8 value)
{
    return (u8)((value & 0x0F) + ((value >> 4) * 10));
}

/* Convert a civil date to a UNIX timestamp (days since 1970-01-01). */
u64 unix_from_civil(u16 year, u8 month, u8 day, u8 hour, u8 minute, u8 second)
{
    /* Howard Hinnant's days_from_civil. */
    i64 y = (i64)year;
    i64 m = (i64)month;
    i64 d = (i64)day;
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    u64 yoe = (u64)(y - era * 400);
    u64 doy = (u64)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    u64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    i64 days = era * 146097 + (i64)doe - 719468;

    u64 seconds = (u64)days * 86400ULL + (u64)hour * 3600ULL +
                  (u64)minute * 60ULL + (u64)second;
    return seconds;
}

void rtc_init(void)
{
    u16 year;
    u8 month, day, hour, minute, second;

    /* Wait for a stable read (the update-in-progress flag protects the
     * register set, but the flag itself can flip mid-read). */
    for (u32 attempt = 0; attempt < 4; attempt++) {
        while (update_in_progress())
            cpu_relax();
        second = cmos_read(CMOS_SECOND);
        minute = cmos_read(CMOS_MINUTE);
        hour = cmos_read(CMOS_HOUR);
        day = cmos_read(CMOS_DAY);
        month = cmos_read(CMOS_MONTH);
        year = (u16)cmos_read(CMOS_YEAR);

        u8 status_b = cmos_read(CMOS_STATUS_B);
        if (!(status_b & 0x04)) {
            /* BCD encoded */
            second = bcd_to_binary(second);
            minute = bcd_to_binary(minute);
            hour = bcd_to_binary((u8)(hour & 0x7F)) | (hour & 0x80);
            day = bcd_to_binary(day);
            month = bcd_to_binary(month);
            year = bcd_to_binary((u8)(year & 0xFF));
        }
        if (!(status_b & 0x02)) {
            /* 12-hour mode: bit 7 of the hour byte means PM. */
            u8 pm = (u8)(hour & 0x80);
            hour = (u8)(hour & 0x7F);
            if (pm && hour < 12)
                hour = (u8)(hour + 12);
            if (!pm && hour == 12)
                hour = 0;
        }
        u16 century = (u16)cmos_read(CMOS_CENTURY);
        if (status_b & 0x04)
            century = 20;
        else
            century = bcd_to_binary((u8)century);
        if (century < 19 || century > 99) {
            century = 20;
            if (year > 70)
                century = 19;
        }
        year = century * 100 + year;

        if (year >= 1980 && year <= 2200 && month >= 1 && month <= 12 &&
            day >= 1 && day <= 31)
            break;
    }

    g_boot_unix = unix_from_civil(year, month, day, hour, minute, second);
    g_rtc_ready = true;
    printk("rtc: %04u-%02u-%02u %02u:%02u:%02u (unix %u)\n", (u64)year,
           (u64)month, (u64)day, (u64)hour, (u64)minute, (u64)second,
           g_boot_unix);
}

void rtc_read(u16 *year, u8 *month, u8 *day, u8 *hour, u8 *minute, u8 *second)
{
    u8 s = cmos_read(CMOS_SECOND);
    u8 mi = cmos_read(CMOS_MINUTE);
    u8 h = cmos_read(CMOS_HOUR);
    u8 d = cmos_read(CMOS_DAY);
    u8 mo = cmos_read(CMOS_MONTH);
    u8 y = cmos_read(CMOS_YEAR);
    u8 status_b = cmos_read(CMOS_STATUS_B);

    if (!(status_b & 0x04)) {
        s = bcd_to_binary(s);
        mi = bcd_to_binary(mi);
        h = bcd_to_binary((u8)(h & 0x7F));
        d = bcd_to_binary(d);
        mo = bcd_to_binary(mo);
        y = bcd_to_binary(y);
    }
    if (year) *year = (u16)(2000 + y);
    if (month) *month = mo;
    if (day) *day = d;
    if (hour) *hour = (u8)(h & 0x7F);
    if (minute) *minute = mi;
    if (second) *second = s;
}

u64 boot_time_unix(void)
{
    return g_boot_unix;
}

/* Milliseconds since midnight, used by the clock widget. */
u64 rtc_time_of_day_ms(void)
{
    u8 hour, minute, second;
    rtc_read(NULL_PTR, NULL_PTR, NULL_PTR, &hour, &minute, &second);
    return ((u64)hour * 3600ULL + (u64)minute * 60ULL + (u64)second) * 1000ULL;
}

void rtc_format(char *buffer, u32 capacity)
{
    u16 year;
    u8 month, day, hour, minute, second;
    rtc_read(&year, &month, &day, &hour, &minute, &second);
    if (capacity < 17)
        return;
    u32 n = 0;
    buffer[n++] = (char)('0' + (year / 1000) % 10);
    buffer[n++] = (char)('0' + (year / 100) % 10);
    buffer[n++] = (char)('0' + (year / 10) % 10);
    buffer[n++] = (char)('0' + year % 10);
    buffer[n++] = '-';
    buffer[n++] = (char)('0' + month / 10);
    buffer[n++] = (char)('0' + month % 10);
    buffer[n++] = '-';
    buffer[n++] = (char)('0' + day / 10);
    buffer[n++] = (char)('0' + day % 10);
    buffer[n++] = ' ';
    buffer[n++] = (char)('0' + hour / 10);
    buffer[n++] = (char)('0' + hour % 10);
    buffer[n++] = ':';
    buffer[n++] = (char)('0' + minute / 10);
    buffer[n++] = (char)('0' + minute % 10);
    buffer[n] = 0;
    AXIOM_UNUSED_VAR(second);
}
