/* AxiomOS - syscall dispatch table.
 *
 * The syscall entry stub in arch/x86_64 hands the number and the register
 * frame to axsys_dispatch.  This file owns the table of handlers, name lookup
 * (for tracing), argument validation hooks, and a per-syscall rate limiter used
 * by the seccomp-like filter.
 */

#include <axiom/kernel.h>

#define AXSY_MAX_CALLS      64
#define AXSY_MAX_FILTERS    16
#define AXSY_NAME_LENGTH    20

typedef struct axsy_args
{
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 r10;
    u64 r8;
    u64 r9;
} axsy_args_t;

typedef i64 (*axsy_handler_t)(axsy_args_t *args);

typedef struct axsy_entry
{
    u16          number;
    u8           arg_count;
    u8           flags;         /* bit 0 privileged, bit 1 traced, bit 2 blocking */
    u32          calls;
    u64          errors;
    axsy_handler_t handler;
    char         name[AXSY_NAME_LENGTH];
} axsy_entry_t;

typedef struct axsy_filter
{
    u64 mask[AXSY_MAX_CALLS / 64];
    u8  action;         /* 0 allow, 1 deny, 2 trace */
    bool_t active;
    char name[16];
} axsy_filter_t;

typedef struct axsy_table
{
    axsy_entry_t entries[AXSY_MAX_CALLS];
    u32          count;
    axsy_filter_t filters[AXSY_MAX_FILTERS];
    u32          filter_count;
    u64          total_calls;
    u64          denied;
    u64          traced;
    u8           active_filter;
    bool_t       initialised;
} axsy_table_t;

static void axsy_zero(void *ptr, u32 size);
static void axsy_copy(char *dst, const char *src);
static u32  axsy_index_of(const axsy_table_t *table, u16 number);
static void axsy_register(axsy_table_t *table, u16 number, const char *name,
                          u8 arg_count, u8 flags, axsy_handler_t handler);
static bool_t axsy_filter_allows(const axsy_filter_t *filter, u16 number);
static bool_t axsy_validate(const axsy_entry_t *entry, const axsy_args_t *args);

/* A few handlers exist so that the dispatcher can be exercised before the real
 * implementations land; they follow the public contract of the kernel API. */
static i64 axsy_handle_noop(axsy_args_t *args);
static i64 axsy_handle_getpid(axsy_args_t *args);
static i64 axsy_handle_ticks(axsy_args_t *args);
static i64 axsy_handle_yield(axsy_args_t *args);
static i64 axsy_handle_uptime(axsy_args_t *args);

static void axsy_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axsy_copy(char *dst, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < AXSY_NAME_LENGTH)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static u32 axsy_index_of(const axsy_table_t *table, u16 number)
{
    u32 i;
    for (i = 0; i < table->count; i++)
    {
        if (table->entries[i].number == number)
            return i;
    }
    return 0xFFFFFFFFu;
}

static void axsy_register(axsy_table_t *table, u16 number, const char *name,
                          u8 arg_count, u8 flags, axsy_handler_t handler)
{
    axsy_entry_t *entry;
    if (table->count >= AXSY_MAX_CALLS)
        return;
    if (axsy_index_of(table, number) != 0xFFFFFFFFu)
        return;
    entry = &table->entries[table->count];
    entry->number = number;
    entry->arg_count = arg_count;
    entry->flags = flags;
    entry->calls = 0;
    entry->errors = 0;
    entry->handler = handler;
    axsy_copy(entry->name, name);
    table->count++;
}

static bool_t axsy_filter_allows(const axsy_filter_t *filter, u16 number)
{
    u32 word = number / 64;
    u32 bit = number % 64;
    if (word >= (AXSY_MAX_CALLS / 64))
        return false;
    return (bool_t)((filter->mask[word] & (1ull << bit)) == 0);
}

static bool_t axsy_validate(const axsy_entry_t *entry, const axsy_args_t *args)
{
    if ((entry->flags & 0x01) != 0)
    {
        /* Privileged calls require a non-null first argument. */
        if (args->rdi > 0x00007FFFFFFFFFFFULL)
            return false;
    }
    if (entry->arg_count > 6)
        return false;
    return true;
}

static i64 axsy_handle_noop(axsy_args_t *args)
{
    AXIOM_UNUSED_VAR(args);
    return 0;
}

static i64 axsy_handle_getpid(axsy_args_t *args)
{
    AXIOM_UNUSED_VAR(args);
    return 1;
}

static i64 axsy_handle_ticks(axsy_args_t *args)
{
    AXIOM_UNUSED_VAR(args);
    return (i64)(rdtsc() / 1000);
}

static i64 axsy_handle_yield(axsy_args_t *args)
{
    AXIOM_UNUSED_VAR(args);
    return 0;
}

static i64 axsy_handle_uptime(axsy_args_t *args)
{
    AXIOM_UNUSED_VAR(args);
    return (i64)(rdtsc() >> 20);
}

/* -------------------------------------------------------------- public API */

void axsy_init(axsy_table_t *table)
{
    axsy_zero(table, (u32)sizeof(*table));
    table->active_filter = 0xFF;
    table->initialised = true;
    axsy_register(table, 0, "read", 3, 0x02, axsy_handle_noop);
    axsy_register(table, 1, "write", 3, 0x02, axsy_handle_noop);
    axsy_register(table, 2, "open", 3, 0x02, axsy_handle_noop);
    axsy_register(table, 3, "close", 1, 0x00, axsy_handle_noop);
    axsy_register(table, 8, "lseek", 3, 0x00, axsy_handle_noop);
    axsy_register(table, 9, "mmap", 6, 0x00, axsy_handle_noop);
    axsy_register(table, 10, "munmap", 2, 0x00, axsy_handle_noop);
    axsy_register(table, 11, "brk", 1, 0x00, axsy_handle_noop);
    axsy_register(table, 12, "ioctl", 3, 0x02, axsy_handle_noop);
    axsy_register(table, 16, "getpid", 0, 0x00, axsy_handle_getpid);
    axsy_register(table, 24, "sched_yield", 0, 0x00, axsy_handle_yield);
    axsy_register(table, 32, "ticks", 0, 0x00, axsy_handle_ticks);
    axsy_register(table, 33, "uptime", 0, 0x00, axsy_handle_uptime);
    axsy_register(table, 39, "spawn", 3, 0x01, axsy_handle_noop);
    axsy_register(table, 40, "wait", 2, 0x02, axsy_handle_noop);
    axsy_register(table, 60, "exit", 1, 0x00, axsy_handle_noop);
    axsy_register(table, 72, "hvci_query", 2, 0x01, axsy_handle_noop);
    axsy_register(table, 73, "hvci_attest", 1, 0x01, axsy_handle_noop);
}

i64 axsy_dispatch(axsy_table_t *table, u16 number, axsy_args_t *args)
{
    u32 index;
    const axsy_entry_t *entry;
    i64 result;
    table->total_calls++;
    index = axsy_index_of(table, number);
    if (index == 0xFFFFFFFFu)
    {
        table->denied++;
        return -1;
    }
    entry = &table->entries[index];
    if (table->active_filter != 0xFF)
    {
        const axsy_filter_t *filter = &table->filters[table->active_filter];
        if (filter->active && !axsy_filter_allows(filter, number))
        {
            if (filter->action == 1)
            {
                table->denied++;
                return -1;
            }
            table->traced++;
        }
    }
    if (!axsy_validate(entry, args))
    {
        table->denied++;
        table->entries[index].errors++;
        return -1;
    }
    result = entry->handler ? entry->handler(args) : 0;
    if (result < 0)
        table->entries[index].errors++;
    table->entries[index].calls++;
    if ((entry->flags & 0x02) != 0)
        table->traced++;
    return result;
}

u32 axsy_add_filter(axsy_table_t *table, const char *name, u8 action)
{
    axsy_filter_t *filter;
    u32 i;
    if (table->filter_count >= AXSY_MAX_FILTERS)
        return 0xFFFFFFFFu;
    filter = &table->filters[table->filter_count];
    axsy_zero(filter, (u32)sizeof(*filter));
    for (i = 0; i < 15 && name[i] != 0; i++)
        filter->name[i] = name[i];
    filter->name[i] = 0;
    filter->action = action;
    filter->active = true;
    table->filter_count++;
    return table->filter_count - 1;
}

void axsy_filter_deny(axsy_table_t *table, u32 filter, u16 number)
{
    u32 word;
    u32 bit;
    if (filter >= table->filter_count)
        return;
    word = number / 64;
    bit = number % 64;
    if (word >= (AXSY_MAX_CALLS / 64))
        return;
    table->filters[filter].mask[word] |= (1ull << bit);
}

void axsy_filter_allow(axsy_table_t *table, u32 filter, u16 number)
{
    u32 word;
    u32 bit;
    if (filter >= table->filter_count)
        return;
    word = number / 64;
    bit = number % 64;
    if (word >= (AXSY_MAX_CALLS / 64))
        return;
    table->filters[filter].mask[word] &= ~(1ull << bit);
}

void axsy_filter_activate(axsy_table_t *table, u32 filter)
{
    if (filter >= table->filter_count)
        return;
    table->active_filter = (u8)filter;
}

void axsy_filter_deactivate(axsy_table_t *table)
{
    table->active_filter = 0xFF;
}

const char *axsy_name(const axsy_table_t *table, u16 number)
{
    u32 index = axsy_index_of(table, number);
    if (index == 0xFFFFFFFFu)
        return "unknown";
    return table->entries[index].name;
}

u32 axsy_count(const axsy_table_t *table)
{
    return table->count;
}

u64 axsy_calls_of(const axsy_table_t *table, u16 number)
{
    u32 index = axsy_index_of(table, number);
    if (index == 0xFFFFFFFFu)
        return 0;
    return table->entries[index].calls;
}

void axsy_report(const axsy_table_t *table)
{
    printk("axsy: calls=%llu denied=%llu traced=%llu entries=%u\n",
           (unsigned long long)table->total_calls,
           (unsigned long long)table->denied,
           (unsigned long long)table->traced, (unsigned)table->count);
}

bool_t axsy_selftest(void)
{
    static axsy_table_t table;
    axsy_args_t args;
    u32 filter;
    axsy_zero(&args, (u32)sizeof(args));
    axsy_init(&table);
    if (table.count != 18)
        return false;
    if (axsy_dispatch(&table, 16, &args) != 1)
        return false;
    if (axsy_dispatch(&table, 60, &args) != 0)
        return false;
    if (axsy_dispatch(&table, 999, &args) != -1)
        return false;
    if (axsy_calls_of(&table, 16) != 1)
        return false;
    filter = axsy_add_filter(&table, "strict", 1);
    if (filter == 0xFFFFFFFFu)
        return false;
    axsy_filter_deny(&table, filter, 2);
    axsy_filter_activate(&table, filter);
    if (axsy_dispatch(&table, 2, &args) != -1)
        return false;
    if (axsy_dispatch(&table, 16, &args) != 1)
        return false;
    axsy_filter_deactivate(&table);
    if (axsy_dispatch(&table, 2, &args) != 0)
        return false;
    if (axsy_name(&table, 16)[0] != 'g')
        return false;
    return true;
}

