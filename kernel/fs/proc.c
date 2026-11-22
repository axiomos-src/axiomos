/* AxiomOS - procfs: kernel state exposed as small read-only files.
 *
 * Each file is a generator invoked on read; nothing is buffered, so the numbers
 * are always current.  Names are registered as strings and matched linearly,
 * which is fine because the table holds a few dozen entries at most.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXPR_MAX_FILES      24
#define AXPR_NAME_LEN       24
#define AXPR_BUFFER_LEN     256
#define AXPR_MAX_READERS    4

#define AXPR_SCOPE_GLOBAL   0
#define AXPR_SCOPE_PROCESS  1
#define AXPR_SCOPE_DEVICE   2

typedef u32 (*axpr_generator_t)(u8 *buffer, u32 capacity, u32 argument);

typedef struct axpr_file
{
    char name[AXPR_NAME_LEN];
    axpr_generator_t generate;
    u32  scope;
    u32  reads;
    u32  bytes_served;
    u32  last_argument;
    bool_t writable;
    bool_t used;
} axpr_file_t;

typedef struct axpr_state
{
    axpr_file_t files[AXPR_MAX_FILES];
    u32  count;
    u32  total_reads;
    u32  total_bytes;
    u32  unknown_lookups;
    u32  tick;
    u32  mem_free_kb;
    u32  mem_total_kb;
    u32  processes;
    u32  threads;
    u32  uptime_seconds;
    u32  load_permille;
    u32  interrupts;
    bool_t ready;
} axpr_state_t;

static axpr_state_t axpr_state;

static void axpr_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axpr_copy_string(char *dst, const char *src, u32 limit)
{
    u32 i = 0;
    if (dst == NULL_PTR || limit == 0)
        return;
    while (src != NULL_PTR && src[i] != 0 && i + 1 < limit)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

axpr_file_t *axpr_find(const char *name);

static bool_t axpr_name_equal(const char *a, const char *b)
{
    u32 i = 0;
    if (a == NULL_PTR || b == NULL_PTR)
        return false;
    while (a[i] != 0 && b[i] != 0)
    {
        if (a[i] != b[i])
            return false;
        i++;
    }
    return a[i] == b[i];
}

static u32 axpr_format_u32(u8 *buffer, u32 capacity, const char *label, u32 value)
    {
    u32 written = 0;
    u32 digits = 0;
    char number[16];
    while (label[written] != 0 && written < capacity)
    {
        buffer[written] = (u8)label[written];
        written++;
    }
    if (written + 2 >= capacity)
        return written;
    buffer[written++] = ' ';
    if (value == 0)
        number[digits++] = '0';
    while (value != 0 && digits < 15)
    {
        number[digits++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (u32 i = 0; i < digits; i++)
        buffer[written + i] = (u8)number[digits - 1 - i];
    written += digits;
    if (written < capacity)
        buffer[written++] = '\n';
    return written;
}

/* -------------------------------------------------------------- generators */

static u32 axpr_gen_uptime(u8 *buffer, u32 capacity, u32 argument)
{
    AXIOM_UNUSED_VAR(argument);
    return axpr_format_u32(buffer, capacity, "uptime", axpr_state.uptime_seconds);
}

static u32 axpr_gen_meminfo(u8 *buffer, u32 capacity, u32 argument)
{
    u32 written = 0;
    AXIOM_UNUSED_VAR(argument);
    written += axpr_format_u32(buffer + written, capacity - written, "total_kb",
                               axpr_state.mem_total_kb);
    if (written >= capacity)
        return written;
    written += axpr_format_u32(buffer + written, capacity - written, "free_kb",
                               axpr_state.mem_free_kb);
    return written;
}

static u32 axpr_gen_loadavg(u8 *buffer, u32 capacity, u32 argument)
{
    AXIOM_UNUSED_VAR(argument);
    return axpr_format_u32(buffer, capacity, "load_permille", axpr_state.load_permille);
}

static u32 axpr_gen_stat(u8 *buffer, u32 capacity, u32 argument)
    {
    u32 written = 0;
    AXIOM_UNUSED_VAR(argument);
    written += axpr_format_u32(buffer, capacity, "processes", axpr_state.processes);
    written += axpr_format_u32(buffer + written, capacity - written, "threads",
                               axpr_state.threads);
    return written;
}

static u32 axpr_gen_interrupts(u8 *buffer, u32 capacity, u32 argument)
{
    AXIOM_UNUSED_VAR(argument);
    return axpr_format_u32(buffer, capacity, "interrupts", axpr_state.interrupts);
}

static u32 axpr_gen_uname(u8 *buffer, u32 capacity, u32 argument)
{
    const char *text = "AxiomOS 1.0 ax86_64";
    u32 i = 0;
    AXIOM_UNUSED_VAR(argument);
    while (text[i] != 0 && i + 2 < capacity)
    {
        buffer[i] = (u8)text[i];
        i++;
    }
    buffer[i++] = '\n';
    return i;
}

/* -------------------------------------------------------------- public API */

void axpr_init(u32 mem_total_kb)
{
    axpr_zero(&axpr_state, (u32)sizeof(axpr_state));
    axpr_state.mem_total_kb = mem_total_kb;
    axpr_state.mem_free_kb = mem_total_kb / 2;
    axpr_state.ready = true;
}

bool_t axpr_register(const char *name, axpr_generator_t generate, u32 scope)
{
    axpr_file_t *file;
    if (axpr_state.count >= AXPR_MAX_FILES || name == NULL_PTR || generate == NULL_PTR)
        return false;
    if (axpr_find(name) != NULL_PTR)
        return false;
    file = &axpr_state.files[axpr_state.count];
    axpr_zero(file, (u32)sizeof(*file));
    axpr_copy_string(file->name, name, AXPR_NAME_LEN);
    file->generate = generate;
    file->scope = scope;
    file->used = true;
    axpr_state.count++;
    return true;
}

axpr_file_t *axpr_find(const char *name)
{
    u32 i;
    for (i = 0; i < axpr_state.count; i++)
    {
        if (axpr_state.files[i].used && axpr_name_equal(axpr_state.files[i].name, name))
            return &axpr_state.files[i];
    }
    return NULL_PTR;
}

u32 axpr_read(const char *name, u8 *buffer, u32 capacity, u32 argument)
{
    axpr_file_t *file = axpr_find(name);
    u32 produced;
    if (file == NULL_PTR)
    {
        axpr_state.unknown_lookups++;
        return 0;
    }
    if (buffer == NULL_PTR || capacity == 0)
        return 0;
    produced = file->generate(buffer, capacity, argument);
    if (produced > capacity)
        produced = capacity;
    file->reads++;
    file->bytes_served += produced;
    file->last_argument = argument;
    axpr_state.total_reads++;
    axpr_state.total_bytes += produced;
    return produced;
}

u32 axpr_file_count(void)
{
    return axpr_state.count;
}

const char *axpr_name(u32 index)
{
    if (index >= axpr_state.count)
        return NULL_PTR;
    return axpr_state.files[index].name;
}

u32 axpr_reads(u32 index)
{
    if (index >= axpr_state.count)
        return 0;
    return axpr_state.files[index].reads;
}

bool_t axpr_update_memory(u32 free_kb)
{
    if (free_kb > axpr_state.mem_total_kb)
        return false;
    axpr_state.mem_free_kb = free_kb;
    return true;
}

bool_t axpr_update_processes(u32 processes, u32 threads)
{
    axpr_state.processes = processes;
    axpr_state.threads = threads;
    return true;
}

void axpr_tick(u32 elapsed_seconds, u32 interrupts)
{
    axpr_state.uptime_seconds += elapsed_seconds;
    axpr_state.interrupts += interrupts;
    axpr_state.tick++;
    if (axpr_state.mem_total_kb != 0)
        axpr_state.load_permille = (axpr_state.processes * 1000) / (axpr_state.threads + 1);
}

bool_t axpr_snapshot(u32 *total_kb, u32 *free_kb, u32 *processes, u32 *uptime)
{
    if (total_kb != NULL_PTR)
        *total_kb = axpr_state.mem_total_kb;
    if (free_kb != NULL_PTR)
        *free_kb = axpr_state.mem_free_kb;
    if (processes != NULL_PTR)
        *processes = axpr_state.processes;
    if (uptime != NULL_PTR)
        *uptime = axpr_state.uptime_seconds;
    return axpr_state.ready;
}

void axpr_report(void)
{
    u32 i;
    printk("axpr: files=%u reads=%u bytes=%u unknown=%u\n",
           (unsigned)axpr_state.count, (unsigned)axpr_state.total_reads,
           (unsigned)axpr_state.total_bytes, (unsigned)axpr_state.unknown_lookups);
    for (i = 0; i < axpr_state.count; i++)
    {
        const axpr_file_t *file = &axpr_state.files[i];
        printk("  /proc/%s scope=%u reads=%u\n", file->name, (unsigned)file->scope,
               (unsigned)file->reads);
    }
}

bool_t axpr_selftest(void)
{
    u8 buffer[AXPR_BUFFER_LEN];
    u32 length;
    axpr_init(65536);
    if (!axpr_register("uptime", axpr_gen_uptime, AXPR_SCOPE_GLOBAL))
        return false;
    if (!axpr_register("meminfo", axpr_gen_meminfo, AXPR_SCOPE_GLOBAL))
        return false;
    if (!axpr_register("loadavg", axpr_gen_loadavg, AXPR_SCOPE_GLOBAL))
        return false;
    if (!axpr_register("stat", axpr_gen_stat, AXPR_SCOPE_GLOBAL))
        return false;
    if (!axpr_register("interrupts", axpr_gen_interrupts, AXPR_SCOPE_DEVICE))
        return false;
    if (!axpr_register("uname", axpr_gen_uname, AXPR_SCOPE_GLOBAL))
        return false;
    if (axpr_register("uptime", axpr_gen_uptime, AXPR_SCOPE_GLOBAL))
        return false;
    length = axpr_read("uptime", buffer, AXPR_BUFFER_LEN, 0);
    if (length == 0)
        return false;
    if (buffer[0] != 'u')
        return false;
    length = axpr_read("meminfo", buffer, AXPR_BUFFER_LEN, 0);
    if (length == 0)
        return false;
    if (!axpr_update_memory(40000))
        return false;
    if (axpr_update_memory(70000))
        return false;
    if (!axpr_update_processes(12, 34))
        return false;
    axpr_tick(60, 100);
    length = axpr_read("uname", buffer, AXPR_BUFFER_LEN, 0);
    if (length == 0)
        return false;
    if (axpr_read("nonexistent", buffer, 8, 0) != 0)
        return false;
    if (axpr_state.unknown_lookups != 1)
        return false;
    if (axpr_file_count() != 6)
        return false;
    {
        u32 total = 0;
        u32 free_kb = 0;
        u32 processes = 0;
        u32 uptime = 0;
        if (!axpr_snapshot(&total, &free_kb, &processes, &uptime))
            return false;
        if (total != 65536 || free_kb != 40000)
            return false;
    }
    return true;
}

