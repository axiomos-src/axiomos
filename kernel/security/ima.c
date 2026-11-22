/* AxiomOS - integrity measurement (IMA style appraisal).
 *
 * Every executable page and configuration file can be measured into an
 * append-only log and appraised against a stored reference digest.  Appraisal is
 * policy driven: enforce, audit-only, or off.  The measurement log is a simple
 * ring that the boot report prints.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXIM_MAX_RULES      32
#define AXIM_MAX_MEASURE    128
#define AXIM_PATH_LEN       64
#define AXIM_DIGEST_LEN     32

enum axim_mode
{
    AXIM_MODE_OFF = 0,
    AXIM_MODE_AUDIT,
    AXIM_MODE_ENFORCE
};

enum axim_result
{
    AXIM_RESULT_PASS = 0,
    AXIM_RESULT_FAIL,
    AXIM_RESULT_MISSING,
    AXIM_RESULT_DENIED
};

typedef struct axim_rule
{
    char   path[AXIM_PATH_LEN];
    u8     reference[AXIM_DIGEST_LEN];
    u32    flags;
    bool_t enforce;
} axim_rule_t;

typedef struct axim_measurement
{
    char path[AXIM_PATH_LEN];
    u8   digest[AXIM_DIGEST_LEN];
    u32  stamp;
    u8   result;
    u8   source;
} axim_measurement_t;

typedef struct axim_state
{
    axim_rule_t rules[AXIM_MAX_RULES];
    axim_measurement_t log[AXIM_MAX_MEASURE];
    u32 rule_count;
    u32 log_head;
    u32 log_count;
    u32 passes;
    u32 failures;
    u32 missing;
    u32 denied;
    u8  mode;
    bool_t ready;
} axim_state_t;

static axim_state_t axim_state;

static void axim_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axim_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool_t axim_equal(const char *a, const char *b)
{
    u32 i = 0;
    while (a[i] != 0 && b[i] != 0)
    {
        if (a[i] != b[i])
            return false;
        i++;
    }
    return a[i] == b[i];
}

static u64 axim_hash(const u8 *data, u32 length)
{
    u64 hash = 0xCBF29CE484222325ull;
    u32 i;
    for (i = 0; i < length; i++)
    {
        hash ^= (u64)data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

static void axim_digest(const u8 *data, u32 length, u8 *out)
{
    u64 hash = axim_hash(data, length);
    u32 i;
    for (i = 0; i < AXIM_DIGEST_LEN; i++)
    {
        out[i] = (u8)(hash >> ((i % 8) * 8));
        if (i >= 8)
            hash ^= (u64)(i * 2654435761u);
    }
}

static axim_rule_t *axim_find_rule(const char *path)
{
    u32 i;
    for (i = 0; i < axim_state.rule_count; i++)
    {
        if (axim_equal(axim_state.rules[i].path, path))
            return &axim_state.rules[i];
    }
    return NULL_PTR;
}

static void axim_log(const char *path, const u8 *digest, u8 result, u8 source)
{
    axim_measurement_t *slot = &axim_state.log[axim_state.log_head];
    axim_copy(slot->path, AXIM_PATH_LEN, path);
    {
        u32 i;
        for (i = 0; i < AXIM_DIGEST_LEN; i++)
            slot->digest[i] = digest[i];
    }
    slot->result = result;
    slot->source = source;
    slot->stamp = axim_state.log_count;
    axim_state.log_head = (axim_state.log_head + 1) % AXIM_MAX_MEASURE;
    if (axim_state.log_count < AXIM_MAX_MEASURE)
        axim_state.log_count++;
}

/* -------------------------------------------------------------- public API */

void axim_init(u8 mode)
{
    axim_zero(&axim_state, (u32)sizeof(axim_state));
    axim_state.mode = mode > AXIM_MODE_ENFORCE ? AXIM_MODE_ENFORCE : mode;
    axim_state.ready = true;
}

bool_t axim_add_rule(const char *path, const u8 *reference, bool_t enforce)
{
    axim_rule_t *rule;
    u32 i;
    if (axim_state.rule_count >= AXIM_MAX_RULES || path == NULL_PTR)
        return false;
    if (axim_find_rule(path) != NULL_PTR)
        return false;
    rule = &axim_state.rules[axim_state.rule_count];
    axim_zero(rule, (u32)sizeof(*rule));
    axim_copy(rule->path, AXIM_PATH_LEN, path);
    for (i = 0; i < AXIM_DIGEST_LEN; i++)
        rule->reference[i] = reference == NULL_PTR ? 0 : reference[i];
    rule->enforce = enforce;
    axim_state.rule_count++;
    return true;
}

u8 axim_measure_and_appraise(const char *path, const u8 *data, u32 length)
{
    u8 digest[AXIM_DIGEST_LEN];
    axim_rule_t *rule;
    axim_digest(data, length, digest);
    rule = axim_find_rule(path);
    if (rule == NULL_PTR)
    {
        axim_state.missing++;
        axim_log(path, digest, AXIM_RESULT_MISSING, 0);
        if (axim_state.mode == AXIM_MODE_ENFORCE)
            return AXIM_RESULT_DENIED;
        return AXIM_RESULT_MISSING;
    }
    {
        u32 i;
        for (i = 0; i < AXIM_DIGEST_LEN; i++)
        {
            if (rule->reference[i] != digest[i])
            {
                axim_state.failures++;
                axim_log(path, digest, AXIM_RESULT_FAIL, 1);
                if (axim_state.mode == AXIM_MODE_ENFORCE && rule->enforce)
                    return AXIM_RESULT_DENIED;
                return AXIM_RESULT_FAIL;
            }
        }
    }
    axim_state.passes++;
    axim_log(path, digest, AXIM_RESULT_PASS, 1);
    return AXIM_RESULT_PASS;
}

u8 axim_appraise_path(const char *path, const u8 *data, u32 length)
{
    return axim_measure_and_appraise(path, data, length);
}

bool_t axim_allowed(u8 result)
{
    if (axim_state.mode == AXIM_MODE_OFF)
        return true;
    return result == AXIM_RESULT_PASS;
}

u8 axim_mode(void)
{
    return axim_state.mode;
}

void axim_set_mode(u8 mode)
{
    axim_state.mode = mode > AXIM_MODE_ENFORCE ? AXIM_MODE_ENFORCE : mode;
}

u32 axim_rule_count(void)
{
    return axim_state.rule_count;
}

u32 axim_failures(void)
{
    return axim_state.failures;
}

u32 axim_log_count(void)
{
    return axim_state.log_count;
}

const char *axim_log_entry(u32 index)
{
    if (index >= axim_state.log_count)
        return "";
    return axim_state.log[index].path;
}

void axim_report(void)
{
    u32 i;
    printk("axim: mode=%u rules=%u log=%u pass=%u fail=%u missing=%u denied=%u\n",
           (unsigned)axim_state.mode, (unsigned)axim_state.rule_count,
           (unsigned)axim_state.log_count, (unsigned)axim_state.passes,
           (unsigned)axim_state.failures, (unsigned)axim_state.missing,
           (unsigned)axim_state.denied);
    for (i = 0; i < axim_state.log_count; i++)
    {
        const axim_measurement_t *entry = &axim_state.log[i];
        printk("  [%u] %-40s result=%u source=%u\n", (unsigned)entry->stamp,
               entry->path, (unsigned)entry->result, (unsigned)entry->source);
    }
}

bool_t axim_selftest(void)
{
    static const u8 payload[] = { 0x10, 0x20, 0x30, 0x40 };
    u8 reference[AXIM_DIGEST_LEN];
    axim_init(AXIM_MODE_ENFORCE);
    axim_digest(payload, 4, reference);
    if (!axim_add_rule("/bin/init", reference, true))
        return false;
    if (axim_rule_count() != 1)
        return false;
    if (axim_measure_and_appraise("/bin/init", payload, 4) != AXIM_RESULT_PASS)
        return false;
    if (axim_measure_and_appraise("/bin/init", payload, 3) != AXIM_RESULT_DENIED)
        return false;
    if (axim_failures() != 1)
        return false;
    if (axim_measure_and_appraise("/etc/unknown", payload, 4) != AXIM_RESULT_DENIED)
        return false;
    if (!axim_allowed(axim_measure_and_appraise("/bin/init", payload, 4)))
        return false;
    axim_set_mode(AXIM_MODE_AUDIT);
    if (axim_measure_and_appraise("/body/na", payload, 3) != AXIM_RESULT_MISSING)
    {
        /* Audit mode still reports missing, but allows the caller to proceed. */
        if (!axim_allowed(AXIM_RESULT_MISSING))
            return false;
    }
    if (axim_log_count() == 0)
        return false;
    return true;
}
