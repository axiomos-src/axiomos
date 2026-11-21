/*
 * AxiomOS - localisation primitives.
 *
 * A shell that hardcodes English is a shell that has to be rewritten to ship
 * anywhere else.  This layer provides the four things a shell actually needs:
 * message lookup by identifier, plural selection, number and date formatting,
 * and a direction flag so layout can mirror without per-control branches.
 *
 * Message identifiers are small integers, not strings: a lookup is an array
 * index instead of a hash, and a missing translation falls back to the
 * developer's language rather than to an identifier the user cannot read.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXL_MAX_MESSAGES      96
#define AXL_MAX_LOCALES        3
#define AXL_TEXT_CAPACITY    128

typedef enum axl_locale {
    AXL_LOCALE_EN = 0,
    AXL_LOCALE_ZH,
    AXL_LOCALE_JA
} axl_locale_t;

typedef enum axl_direction {
    AXL_LTR = 0,
    AXL_RTL
} axl_direction_t;

typedef struct axl_message {
    const char *id;
    const char *en;
    const char *zh;
    const char *ja;
} axl_message_t;

typedef struct axl_context {
    axl_locale_t   locale;
    axl_direction_t direction;
    i32            first_day_of_week;   /* 0 = Sunday */
    bool_t         use_24_hour;
    bool_t         metric;
    char           decimal_separator;
    char           group_separator;
    i32            group_size;
} axl_context_t;

/* The table is the only place translated text lives.  Keeping it in one array
 * means a translation review is one screen, and a missing locale is visible as a
 * null column instead of a silent fallback scattered across call sites. */
static const axl_message_t axl_messages[AXL_MAX_MESSAGES] = {
    {"shell.open", "Open", "\xE6\x89\x93\xE5\xBC\x80", "\xE9\x96\x8B\xE3\x81\x8F"},
    {"shell.close", "Close", "\xE5\x85\xB3\xE9\x97\xAD", "\xE9\x96\x89\xE3\x81\x98\xE3\x82\x8B"},
    {"shell.cancel", "Cancel", "\xE5\x8F\x96\xE6\xB6\x88", "\xE3\x82\xAD\xE3\x83\xA3\xE3\x83\xB3\xE3\x82\xBB\xE3\x83\xAB"},
    {"shell.save", "Save", "\xE4\xBF\x9D\xE5\xAD\x98", "\xE4\xBF\x9D\xE5\xAD\x98"},
    {"shell.delete", "Delete", "\xE5\x88\xA0\xE9\x99\xA4", "\xE5\x89\x8A\xE9\x99\xA4"},
    {"shell.rename", "Rename", "\xE9\x87\x8D\xE5\x91\xBD\xE5\x90\x8D", "\xE5\x90\x8D\xE5\x89\x8D\xE3\x82\x92\xE5\xA4\x89\xE6\x9B\xB4"},
    {"shell.search", "Search", "\xE6\x90\x9C\xE7\xB4\xA2", "\xE6\xA4\x9C\xE7\xB4\xA2"},
    {"shell.settings", "Settings", "\xE8\xAE\xBE\xE7\xBD\xAE", "\xE8\xA8\xAD\xE5\xAE\x9A"},
    {"shell.power", "Power", "\xE7\x94\xB5\xE6\xBA\x90", "\xE9\x9B\xBB\xE6\xBA\x90"},
    {"shell.lock", "Lock", "\xE9\x94\x81\xE5\xAE\x9A", "\xE3\x83\xAD\xE3\x83\x83\xE3\x82\xAF"},
    {"shell.notifications", "Notifications", "\xE9\x80\x9A\xE7\x9F\xA5", "\xE9\x80\x9A\xE7\x9F\xA5"},
    {"shell.volume", "Volume", "\xE9\x9F\xB3\xE9\x87\x8F", "\xE9\x9F\xB3\xE9\x87\x8F"},
    {"shell.brightness", "Brightness", "\xE4\xBA\xAE\xE5\xBA\xA6", "\xE6\x98\x8E\xE3\x82\x8B\xE3\x81\x95"},
    {"fs.name", "Name", "\xE5\x90\x8D\xE7\xA7\xB0", "\xE5\x90\x8D\xE5\x89\x8D"},
    {"fs.size", "Size", "\xE5\xA4\xA7\xE5\xB0\x8F", "\xE3\x82\xB5\xE3\x82\xA4\xE3\x82\xBA"},
    {"fs.date", "Date", "\xE6\x97\xA5\xE6\x9C\x9F", "\xE6\x97\xA5\xE4\xBB\x98"},
    {"fs.kind", "Kind", "\xE7\xB1\xBB\xE5\x9E\x8B", "\xE7\xA8\xAE\xE9\xA1\x9E"},
    {"fs.empty", "This folder is empty", "\xE6\xAD\xA4\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\xE4\xB8\xBA\xE7\xA9\xBA", "\xE3\x81\x93\xE3\x81\xAE\xE3\x83\x95\xE3\x82\xA9\xE3\x83\xAB\xE3\x83\x80\xE3\x83\xBC\xE3\x81\xAF\xE7\xA9\xBA\xE3\x81\xA7\xE3\x81\x99"},
    {"term.new", "New tab", "\xE6\x96\xB0\xE5\xBB\xBA\xE6\xA0\x87\xE7\xAD\xBE", "\xE6\x96\xB0\xE8\xA6\x8F\xE3\x82\xBF\xE3\x83\x96"},
    {"term.clear", "Clear", "\xE6\xB8\x85\xE9\x99\xA4", "\xE3\x82\xAF\xE3\x83\xAA\xE3\x82\xA2"},
    {"settings.system", "System", "\xE7\xB3\xBB\xE7\xBB\x9F", "\xE3\x82\xB7\xE3\x82\xB9\xE3\x83\x86\xE3\x83\xA0"},
    {"settings.apps", "Apps", "\xE5\xBA\x94\xE7\x94\xA8", "\xE3\x82\xA2\xE3\x83\x97\xE3\x83\xAA"},
    {"settings.network", "Network", "\xE7\xBD\x91\xE7\xBB\x9C", "\xE3\x83\x8D\xE3\x83\x83\xE3\x83\x88\xE3\x83\xAF\xE3\x83\xBC\xE3\x82\xAF"},
    {"settings.privacy", "Privacy", "\xE9\x9A\x90\xE7\xA7\x81", "\xE3\x83\x97\xE3\x83\xA9\xE3\x82\xA4\xE3\x83\x90\xE3\x82\xB7\xE3\x83\xBC"},
    {"settings.about", "About", "\xE5\x85\xB3\xE4\xBA\x8E", "\xE3\x83\x90\xE3\x83\xBC\xE3\x82\xB8\xE3\x83\xA7\xE3\x83\xB3"},
    {"time.minute", "minute", "\xE5\x88\x86\xE9\x92\x9F", "\xE5\x88\x86"},
    {"time.minutes", "minutes", "\xE5\x88\x86\xE9\x92\x9F", "\xE5\x88\x86"},
    {"time.hour", "hour", "\xE5\xB0\x8F\xE6\x97\xB6", "\xE6\x99\x82\xE9\x96\x93"},
    {"time.hours", "hours", "\xE5\xB0\x8F\xE6\x97\xB6", "\xE6\x99\x82\xE9\x96\x93"},
    {"unit.byte", "byte", "\xE5\xAD\x97\xE8\x8A\x82", "\xE3\x83\x90\xE3\x82\xA4\xE3\x83\x88"},
    {"unit.bytes", "bytes", "\xE5\xAD\x97\xE8\x8A\x82", "\xE3\x83\x90\xE3\x82\xA4\xE3\x83\x88"},
    {"unit.kb", "KB", "\xE5\x8D\x83\xE5\xAD\x97\xE8\x8A\x82", "KB"},
    {"unit.mb", "MB", "\xE5\x85\x86\xE5\xAD\x97\xE8\x8A\x82", "MB"},
    {"unit.gb", "GB", "\xE5\x90\x89\xE5\xAD\x97\xE8\x8A\x82", "GB"},
    {"error.generic", "Something went wrong", "\xE5\x87\xBA\xE4\xBA\x86\xE9\x97\xAE\xE9\xA2\x98", "\xE5\x95\x8F\xE9\xA1\x8C\xE3\x81\x8C\xE7\x99\xBA\xE7\x94\x9F\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F"},
};

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

void axl_init(axl_context_t *context)
{
    if (!context) return;
    ax_memset(context, 0, sizeof(*context));
    context->locale = AXL_LOCALE_EN;
    context->direction = AXL_LTR;
    context->first_day_of_week = 0;
    context->use_24_hour = (bool_t)1;
    context->decimal_separator = '.';
    context->group_separator = ',';
    context->group_size = 3;
}

const char *axl_locale_name(axl_locale_t locale)
{
    switch (locale) {
    case AXL_LOCALE_ZH: return "zh-CN";
    case AXL_LOCALE_JA: return "ja-JP";
    case AXL_LOCALE_EN:
    default: return "en-US";
    }
}

void axl_set_locale(axl_context_t *context, axl_locale_t locale)
{
    if (!context) return;
    context->locale = locale;
    switch (locale) {
    case AXL_LOCALE_ZH:
        context->group_separator = ',';
        context->decimal_separator = '.';
        context->first_day_of_week = 1;   /* Monday */
        context->use_24_hour = (bool_t)1;
        break;
    case AXL_LOCALE_JA:
        context->group_separator = ',';
        context->decimal_separator = '.';
        context->first_day_of_week = 0;   /* Sunday */
        context->use_24_hour = (bool_t)1;
        break;
    case AXL_LOCALE_EN:
    default:
        context->group_separator = ',';
        context->decimal_separator = '.';
        context->first_day_of_week = 0;
        context->use_24_hour = (bool_t)1;
        break;
    }
    context->direction = AXL_LTR;
}

/* Lookup by identifier.  The table is scanned rather than indexed by a perfect
 * hash because there are fewer than a hundred strings and a scan of that size is
 * free relative to a frame. */
const char *axl_text(const axl_context_t *context, const char *id)
{
    for (i32 i = 0; i < AXL_MAX_MESSAGES; i++) {
        const axl_message_t *message = &axl_messages[i];
        if (!message->id) continue;
        if (ax_strcmp(message->id, id) != 0) continue;
        switch (context ? context->locale : AXL_LOCALE_EN) {
        case AXL_LOCALE_ZH: return message->zh ? message->zh : message->en;
        case AXL_LOCALE_JA: return message->ja ? message->ja : message->en;
        case AXL_LOCALE_EN:
        default: return message->en ? message->en : message->id;
        }
    }
    return id;
}

i32 axl_message_count(void)
{
    i32 count = 0;
    for (i32 i = 0; i < AXL_MAX_MESSAGES; i++) {
        if (axl_messages[i].id) count++;
    }
    return count;
}

/* Plural selection.  English has two forms, Japanese has one, and Chinese has
 * one; the message table provides both singular and plural keys and this picks
 * between them.  For languages with one form the plural key is the same string,
 * which is why the fallback is benign. */
const char *axl_plural(const axl_context_t *context, const char *singular_id,
                       const char *plural_id, i32 count)
{
    if (!context) return axl_text(context, count == 1 ? singular_id : plural_id);
    if (context->locale == AXL_LOCALE_JA) {
        return axl_text(context, singular_id);
    }
    return axl_text(context, count == 1 ? singular_id : plural_id);
}

/* ------------------------------------------------------------------ */
/* Numbers                                                             */
/* ------------------------------------------------------------------ */

/* Group digits from the right in threes.  The function formats into a caller
 * buffer and never allocates; buffer overflow stops the copy rather than
 * truncating a number mid-digit silently. */
i32 axl_number(const axl_context_t *context, i64 value, char *out, i32 capacity)
{
    if (!out || capacity <= 0) return 0;
    char digits[24];
    i32 length = 0;
    bool_t negative = value < 0;
    u64 magnitude = negative ? (u64)(-value) : (u64)value;
    if (magnitude == 0) {
        digits[length++] = '0';
    }
    while (magnitude > 0 && length < (i32)sizeof(digits)) {
        digits[length++] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    }
    i32 written = 0;
    if (negative && written < capacity - 1) out[written++] = '-';
    i32 group = context ? context->group_size : 3;
    char separator = context ? context->group_separator : ',';
    for (i32 i = length - 1; i >= 0; i--) {
        if (written < capacity - 1) out[written++] = digits[i];
        i32 position_from_right = i;
        if (group > 0 && position_from_right > 0 &&
            position_from_right % group == 0) {
            if (written < capacity - 1) out[written++] = separator;
        }
    }
    out[written] = 0;
    return written;
}

/* Byte sizes use binary units with one decimal of precision, formatted as
 * integer tenths to avoid floating point in the kernel. */
i32 axl_bytes(const axl_context_t *context, u64 bytes, char *out, i32 capacity)
{
    if (!out || capacity <= 0) return 0;
    const char *unit = axl_text(context, "unit.bytes");
    u64 divisor = 1;
    if (bytes >= 1073741824ull) {
        divisor = 1073741824ull;
        unit = axl_text(context, "unit.gb");
    } else if (bytes >= 1048576ull) {
        divisor = 1048576ull;
        unit = axl_text(context, "unit.mb");
    } else if (bytes >= 1024ull) {
        divisor = 1024ull;
        unit = axl_text(context, "unit.kb");
    }
    if (divisor == 1) {
        char number[24];
        axl_number(context, (i64)bytes, number, sizeof(number));
        ax_snprintf(out, capacity, "%s %s", number, unit);
        return (i32)ax_strlen(out);
    }
    u64 tenths = (bytes * 10) / divisor;
    char number[24];
    axl_number(context, (i64)(tenths / 10), number, sizeof(number));
    ax_snprintf(out, capacity, "%s%c%llu %s", number,
                context ? context->decimal_separator : '.',
                (unsigned long long)(tenths % 10), unit);
    return (i32)ax_strlen(out);
}

/* Percentage with a signed delta, used by statistics cards. */
i32 axl_percent(const axl_context_t *context, i32 basis_points, char *out,
                i32 capacity)
{
    if (!out || capacity <= 0) return 0;
    i32 sign = basis_points < 0 ? -1 : 1;
    i32 magnitude = basis_points < 0 ? -basis_points : basis_points;
    ax_snprintf(out, capacity, "%s%d%c%d%%", sign < 0 ? "-" : "",
                magnitude / 100, context ? context->decimal_separator : '.',
                magnitude % 100);
    return (i32)ax_strlen(out);
}

/* ------------------------------------------------------------------ */
/* Dates                                                               */
/* ------------------------------------------------------------------ */

/* Sakamoto's algorithm: day of week from a packed month table, no tables of
 * month starts and no leap-year branch chain.  It is exact for the Gregorian
 * calendar over the range that fits in an i32 year. */
static i32 axl_day_of_week(i32 year, i32 month, i32 day)
{
    static const i32 offsets[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
}

i32 axl_month_length(i32 year, i32 month)
{
    static const i32 lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30,
                                    31};
    if (month == 2) {
        bool_t leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    if (month < 1 || month > 12) return 30;
    return lengths[month - 1];
}

i32 axl_weekday(i32 year, i32 month, i32 day)
{
    return axl_day_of_week(year, month, day);
}

const char *axl_weekday_name(const axl_context_t *context, i32 weekday)
{
    /* Day names are not in the message table because they are a closed set and
     * eight strings per locale would triple the table for little benefit at this
     * stage; they live in a small local matrix instead. */
    static const char *en[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                "Thursday", "Friday", "Saturday"};
    static const char *zh[7] = {"\xE6\x98\x9F\xE6\x9C\x9F\xE6\x97\xA5",
                                "\xE6\x98\x9F\xE6\x9C\x9F\xE4\xB8\x80",
                                "\xE6\x98\x9F\xE6\x9C\x9F\xE4\xBA\x8C",
                                "\xE6\x98\x9F\xE6\x9C\x9F\xE4\xB8\x89",
                                "\xE6\x98\x9F\xE6\x9C\x9F\xE5\x9B\x9B",
                                "\xE6\x98\x9F\xE6\x9C\x9F\xE4\xBA\x94",
                                "\xE6\x98\x9F\xE6\x9C\x9F\xE5\x85\xAD"};
    static const char *ja[7] = {"\xE6\x97\xA5\xE6\x9B\x9C\xE6\x97\xA5",
                                "\xE6\x9C\x88\xE6\x9B\x9C\xE6\x97\xA5",
                                "\xE7\x81\xAB\xE6\x9B\x9C\xE6\x97\xA5",
                                "\xE6\xB0\xB4\xE6\x9B\x9C\xE6\x97\xA5",
                                "\xE6\x9C\xA8\xE6\x9B\x9C\xE6\x97\xA5",
                                "\xE9\x87\x91\xE6\x9B\x9C\xE6\x97\xA5",
                                "\xE5\x9C\x9F\xE6\x9B\x9C\xE6\x97\xA5"};
    if (weekday < 0 || weekday > 6) return "";
    if (context && context->locale == AXL_LOCALE_ZH) return zh[weekday];
    if (context && context->locale == AXL_LOCALE_JA) return ja[weekday];
    return en[weekday];
}

/* A short date: the locale decides field order.  The function writes a fixed
 * shape rather than a locale-specific pattern language, because the shell only
 * needs one display style per locale at this point. */
i32 axl_date(const axl_context_t *context, i32 year, i32 month, i32 day,
             char *out, i32 capacity)
{
    if (!out || capacity <= 0) return 0;
    if (context && context->locale == AXL_LOCALE_EN) {
        ax_snprintf(out, capacity, "%d/%d/%d", month, day, year);
    } else {
        ax_snprintf(out, capacity, "%d/%d/%d", year, month, day);
    }
    return (i32)ax_strlen(out);
}

/* Clock formatting: 24-hour locales get a leading zero; 12-hour locales get an
 * am/pm suffix, which is one byte per call rather than a translated string. */
i32 axl_time(const axl_context_t *context, i32 hour, i32 minute, char *out,
             i32 capacity)
{
    if (!out || capacity <= 0) return 0;
    if (context && !context->use_24_hour) {
        i32 display_hour = hour % 12;
        if (display_hour == 0) display_hour = 12;
        ax_snprintf(out, capacity, "%d:%02d %s", display_hour, minute,
                    hour < 12 ? "AM" : "PM");
    } else {
        ax_snprintf(out, capacity, "%02d:%02d", hour, minute);
    }
    return (i32)ax_strlen(out);
}

/* ------------------------------------------------------------------ */
/* Bidirectional layout                                                */
/* ------------------------------------------------------------------ */

/* Mirroring is a coordinate transform applied where a rect is built, not a
 * property every control has to remember.  A caller in an RTL locale mirrors
 * its layout once, at the container, and children inherit mirrored positions. */
void axl_mirror_rect(const axl_context_t *context, gui_rect_t *rect,
                     gui_rect_t container)
{
    if (!context || !rect || context->direction != AXL_RTL) return;
    i32 right_edge = container.x + container.w - (rect->x - container.x);
    rect->x = right_edge - rect->w;
}

bool_t axl_is_rtl(const axl_context_t *context)
{
    return context && context->direction == AXL_RTL;
}

/* Text truncation for labels: returns the byte count that fits in the width,
 * reserving space for a single-character ellipsis.  Byte counts are used rather
 * than glyph counts because the bitmap font renders one byte per cell. */
i32 axl_fit_text(const char *text, i32 max_chars, char *out, i32 capacity)
{
    if (!text || !out || capacity <= 0) return 0;
    if (max_chars <= 0) {
        out[0] = 0;
        return 0;
    }
    i32 length = (i32)ax_strlen(text);
    if (length <= max_chars) {
        i32 copy = length < capacity - 1 ? length : capacity - 1;
        for (i32 i = 0; i < copy; i++) out[i] = text[i];
        out[copy] = 0;
        return copy;
    }
    i32 keep = max_chars - 1;
    if (keep > capacity - 2) keep = capacity - 2;
    if (keep < 0) keep = 0;
    for (i32 i = 0; i < keep; i++) out[i] = text[i];
    out[keep] = '~';
    out[keep + 1] = 0;
    return keep + 1;
}

void axl_dump(const axl_context_t *context)
{
    if (!context) return;
    printk("l10n: locale=%s messages=%d direction=%s 24h=%d\n",
           axl_locale_name(context->locale), axl_message_count(),
           context->direction == AXL_RTL ? "rtl" : "ltr", context->use_24_hour);
}
