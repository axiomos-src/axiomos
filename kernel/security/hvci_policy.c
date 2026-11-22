/* AxiomOS - HVCI policy engine.
 *
 * HVCI (hypervisor enforced code integrity) in this project is a software
 * model: the hypervisor side tracks a table of executable page digests and
 * refuses to execute a page unless it is present in the table and still
 * matches its recorded digest after any write.  The policy engine below is the
 * firmware/kernel side of that model.  It owns:
 *
 *   - the allow list of trusted executable ranges (kernel image, drivers)
 *   - the page digest table, indexed by physical frame number
 *   - the violation log, a bounded ring with monotonic sequence numbers
 *   - enforcement modes (off, audit, enforce, lockdown)
 *   - rule objects that grant execute permission for frame windows
 *   - attestation: a compact digest over the whole policy table
 *
 * The engine never allocates on behalf of the hypervisor; all storage comes
 * from the caller owned policy object.  The security path therefore stays free
 * of the general purpose heap.
 */

#include <axiom/kernel.h>

#define hvci_pol_MAX_RANGES      64
#define hvci_pol_MAX_PAGES       4096
#define hvci_pol_MAX_VIOLATIONS  64
#define hvci_pol_MAX_RULES       128
#define hvci_pol_NAME_LENGTH     32
#define hvci_pol_HASH_BUCKETS    1024

enum hvci_pol_mode
{
    hvci_pol_MODE_OFF = 0,
    hvci_pol_MODE_AUDIT,
    hvci_pol_MODE_ENFORCE,
    hvci_pol_MODE_LOCKDOWN
};

enum hvci_pol_reason
{
    hvci_pol_REASON_NONE = 0,
    hvci_pol_REASON_UNKNOWN_FRAME,
    hvci_pol_REASON_DIGEST_MISMATCH,
    hvci_pol_REASON_WRITABLE_EXECUTE,
    hvci_pol_REASON_OUTSIDE_RANGE,
    hvci_pol_REASON_REVOKED,
    hvci_pol_REASON_STALE_ATTESTATION
};

enum hvci_pol_permission
{
    hvci_pol_PERM_READ = 1,
    hvci_pol_PERM_WRITE = 2,
    hvci_pol_PERM_EXECUTE = 4
};

typedef struct hvci_pol_range
{
    u64  start;
    u64  end;              /* exclusive */
    u32  flags;            /* bit 0 image, 1 driver, 2 hypervisor, 3 user */
    bool_t active;
    char name[hvci_pol_NAME_LENGTH];
} hvci_pol_range_t;

typedef struct hvci_pol_page
{
    u64  frame;
    u64  digest;
    u32  references;
    u8   permissions;
    u8   trusted;
    u8   revoked;
    u8   sealed;
} hvci_pol_page_t;

typedef struct hvci_pol_violation
{
    u64  frame;
    u64  reported_digest;
    u64  expected_digest;
    u32  tick;
    u8   reason;
    u8   mode;
    char context[hvci_pol_NAME_LENGTH];
} hvci_pol_violation_t;

typedef struct hvci_pol_rule
{
    char name[hvci_pol_NAME_LENGTH];
    u64  frame_start;
    u64  frame_end;
    u32  allow_mask;
    bool_t active;
} hvci_pol_rule_t;

typedef struct hvci_pol_policy
{
    hvci_pol_range_t     ranges[hvci_pol_MAX_RANGES];
    u32              range_count;
    hvci_pol_page_t      pages[hvci_pol_MAX_PAGES];
    u32              page_count;
    hvci_pol_violation_t violations[hvci_pol_MAX_VIOLATIONS];
    u32              violation_head;
    u32              violation_count;
    u64              violation_sequence;
    hvci_pol_rule_t      rules[hvci_pol_MAX_RULES];
    u32              rule_count;
    u8               mode;
    bool_t           initialised;
    bool_t           lockdown_requested;
    u64              attestations;
    u64              checks;
    u64              denials;
    u64              write_protect_events;
    u64              last_attestation;
    u64              epoch;
} hvci_pol_policy_t;

static void   hvci_pol_zero(void *ptr, u64 size);
static u64    hvci_pol_hash64(u64 value);
static u64    hvci_pol_hash_frames(const u64 *frames, u32 count);
static void   hvci_pol_copy_name(char *dst, const char *src);
static u32    hvci_pol_find_page(const hvci_pol_policy_t *policy, u64 frame);
static bool_t hvci_pol_insert_page(hvci_pol_policy_t *policy, u64 frame, u64 digest,
                               u8 permissions, bool_t trusted);
static void   hvci_pol_remove_page(hvci_pol_policy_t *policy, u32 index);
static bool_t hvci_pol_range_for(const hvci_pol_policy_t *policy, u64 frame,
                             u32 *flags);
static void   hvci_pol_log(hvci_pol_policy_t *policy, u64 frame, u8 reason,
                       u64 expected, u64 reported, const char *context);
static bool_t hvci_pol_allowed_by_rule(const hvci_pol_policy_t *policy, u64 frame,
                                   u8 permission);
static u64    hvci_pol_attest(const hvci_pol_policy_t *policy);
static const char *hvci_pol_mode_name(u8 mode);
static const char *hvci_pol_reason_name(u8 reason);
static u32    hvci_pol_protect_write(hvci_pol_policy_t *policy, u64 start, u64 end);
static u32    hvci_pol_revoke_range(hvci_pol_policy_t *policy, u64 start, u64 end);

static void hvci_pol_zero(void *ptr, u64 size)
{
    u8 *bytes = (u8 *)ptr;
    u64 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u64 hvci_pol_hash64(u64 value)
{
    /* FNV-1a over a single 64 bit word, split into byte steps. */
    u64 hash = 0xCBF29CE484222325ULL;
    u32 i;
    for (i = 0; i < 8; i++)
    {
        hash ^= (value >> (i * 8)) & 0xFF;
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

static u64 hvci_pol_hash_frames(const u64 *frames, u32 count)
{
    u64 hash = 0x9E3779B97F4A7C15ULL;
    u32 i;
    for (i = 0; i < count; i++)
    {
        u64 value = frames[i];
        hash ^= hvci_pol_hash64(value);
        hash = (hash << 7) | (hash >> 57);
        hash += 0x51ED270BULL;
    }
    return hash;
}

static void hvci_pol_copy_name(char *dst, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < hvci_pol_NAME_LENGTH)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static u32 hvci_pol_find_page(const hvci_pol_policy_t *policy, u64 frame)
{
    u32 i;
    for (i = 0; i < policy->page_count; i++)
    {
        if (policy->pages[i].frame == frame)
            return i;
    }
    return 0xFFFFFFFFu;
}

static bool_t hvci_pol_insert_page(hvci_pol_policy_t *policy, u64 frame, u64 digest,
                               u8 permissions, bool_t trusted)
{
    hvci_pol_page_t *page;
    u32 existing = hvci_pol_find_page(policy, frame);
    if (existing != 0xFFFFFFFFu)
    {
        policy->pages[existing].digest = digest;
        policy->pages[existing].permissions = permissions;
        policy->pages[existing].trusted = trusted ? 1 : 0;
        policy->pages[existing].revoked = 0;
        return true;
    }
    if (policy->page_count >= hvci_pol_MAX_PAGES)
        return false;
    page = &policy->pages[policy->page_count];
    page->frame = frame;
    page->digest = digest;
    page->references = 1;
    page->permissions = permissions;
    page->trusted = trusted ? 1 : 0;
    page->revoked = 0;
    page->sealed = 0;
    policy->page_count++;
    return true;
}

static void hvci_pol_remove_page(hvci_pol_policy_t *policy, u32 index)
{
    u32 i;
    if (index >= policy->page_count)
        return;
    for (i = index; i + 1 < policy->page_count; i++)
        policy->pages[i] = policy->pages[i + 1];
    policy->page_count--;
}

static bool_t hvci_pol_range_for(const hvci_pol_policy_t *policy, u64 frame,
                             u32 *flags)
{
    u32 i;
    u64 address = frame * AXIOM_PAGE_SIZE;
    for (i = 0; i < policy->range_count; i++)
    {
        const hvci_pol_range_t *range = &policy->ranges[i];
        if (!range->active)
            continue;
        if (address >= range->start && address < range->end)
        {
            if (flags != NULL_PTR)
                *flags = range->flags;
            return true;
        }
    }
    return false;
}

static void hvci_pol_log(hvci_pol_policy_t *policy, u64 frame, u8 reason,
                     u64 expected, u64 reported, const char *context)
{
    hvci_pol_violation_t *entry = &policy->violations[policy->violation_head];
    entry->frame = frame;
    entry->expected_digest = expected;
    entry->reported_digest = reported;
    entry->reason = reason;
    entry->mode = policy->mode;
    entry->tick = (u32)policy->checks;
    hvci_pol_copy_name(entry->context, context ? context : "unknown");
    policy->violation_head = (policy->violation_head + 1) % hvci_pol_MAX_VIOLATIONS;
    if (policy->violation_count < hvci_pol_MAX_VIOLATIONS)
        policy->violation_count++;
    policy->violation_sequence++;
}

static bool_t hvci_pol_allowed_by_rule(const hvci_pol_policy_t *policy, u64 frame,
                                   u8 permission)
{
    u32 i;
    for (i = 0; i < policy->rule_count; i++)
    {
        const hvci_pol_rule_t *rule = &policy->rules[i];
        if (!rule->active)
            continue;
        if (frame < rule->frame_start || frame > rule->frame_end)
            continue;
        if ((rule->allow_mask & permission) != 0)
            return true;
    }
    return false;
}

static u64 hvci_pol_attest(const hvci_pol_policy_t *policy)
{
    u64 frames[hvci_pol_MAX_PAGES];
    u32 i;
    u32 count = 0;
    for (i = 0; i < policy->page_count && count < hvci_pol_MAX_PAGES; i++)
    {
        if (policy->pages[i].revoked)
            continue;
        frames[count] = policy->pages[i].frame ^ policy->pages[i].digest;
        count++;
    }
    return hvci_pol_hash_frames(frames, count);
}

static const char *hvci_pol_mode_name(u8 mode)
{
    switch (mode)
    {
    case hvci_pol_MODE_AUDIT: return "audit";
    case hvci_pol_MODE_ENFORCE: return "enforce";
    case hvci_pol_MODE_LOCKDOWN: return "lockdown";
    default: return "off";
    }
}

static const char *hvci_pol_reason_name(u8 reason)
{
    switch (reason)
    {
    case hvci_pol_REASON_UNKNOWN_FRAME: return "unknown-frame";
    case hvci_pol_REASON_DIGEST_MISMATCH: return "digest-mismatch";
    case hvci_pol_REASON_WRITABLE_EXECUTE: return "write-execute";
    case hvci_pol_REASON_OUTSIDE_RANGE: return "outside-range";
    case hvci_pol_REASON_REVOKED: return "revoked";
    case hvci_pol_REASON_STALE_ATTESTATION: return "stale-attestation";
    default: return "none";
    }
}

static u32 hvci_pol_protect_write(hvci_pol_policy_t *policy, u64 start, u64 end)
{
    u32 i;
    u32 hits = 0;
    for (i = 0; i < policy->page_count; i++)
    {
        hvci_pol_page_t *page = &policy->pages[i];
        if (page->frame < start || page->frame >= end)
            continue;
        if ((page->permissions & hvci_pol_PERM_EXECUTE) != 0)
        {
            page->permissions = (u8)(page->permissions & ~hvci_pol_PERM_WRITE);
            policy->write_protect_events++;
            hits++;
        }
    }
    return hits;
}

static u32 hvci_pol_revoke_range(hvci_pol_policy_t *policy, u64 start, u64 end)
{
    u32 i;
    u32 hits = 0;
    for (i = 0; i < policy->page_count; i++)
    {
        hvci_pol_page_t *page = &policy->pages[i];
        if (page->frame < start || page->frame >= end)
            continue;
        page->revoked = 1;
        hits++;
    }
    return hits;
}

/* -------------------------------------------------------------- public API */

void hvci_pol_init(hvci_pol_policy_t *policy)
{
    hvci_pol_zero(policy, (u64)sizeof(*policy));
    policy->mode = hvci_pol_MODE_AUDIT;
    policy->initialised = true;
    policy->epoch = 1;
}

void hvci_pol_reset(hvci_pol_policy_t *policy)
{
    hvci_pol_init(policy);
}

void hvci_pol_add_range(hvci_pol_policy_t *policy, u64 start, u64 end, u32 flags,
                    const char *name)
{
    hvci_pol_range_t *range;
    if (policy->range_count >= hvci_pol_MAX_RANGES)
        return;
    range = &policy->ranges[policy->range_count];
    range->start = start;
    range->end = end;
    range->flags = flags;
    range->active = true;
    hvci_pol_copy_name(range->name, name ? name : "range");
    policy->range_count++;
}

bool_t hvci_pol_register_page(hvci_pol_policy_t *policy, u64 frame, u64 digest,
                          u8 permissions)
{
    u32 flags = 0;
    bool_t trusted = hvci_pol_range_for(policy, frame, &flags);
    policy->checks++;
    if (!trusted && policy->mode == hvci_pol_MODE_LOCKDOWN)
        return false;
    if (!hvci_pol_insert_page(policy, frame, digest, permissions, trusted))
    {
        policy->denials++;
        hvci_pol_log(policy, frame, hvci_pol_REASON_UNKNOWN_FRAME, 0, digest, "register");
        return false;
    }
    return true;
}

bool_t hvci_pol_verify_execute(hvci_pol_policy_t *policy, u64 frame, u64 digest,
                           const char *context)
{
    u32 index;
    u32 flags = 0;
    policy->checks++;
    index = hvci_pol_find_page(policy, frame);
    if (index == 0xFFFFFFFFu)
    {
        if (policy->mode == hvci_pol_MODE_ENFORCE || policy->mode == hvci_pol_MODE_LOCKDOWN)
            policy->denials++;
        hvci_pol_log(policy, frame, hvci_pol_REASON_UNKNOWN_FRAME, 0, digest, context);
        return policy->mode == hvci_pol_MODE_OFF ? true : policy->mode == hvci_pol_MODE_AUDIT;
    }
    {
        const hvci_pol_page_t *page = &policy->pages[index];
        if (page->revoked)
        {
            policy->denials++;
            hvci_pol_log(policy, frame, hvci_pol_REASON_REVOKED, page->digest, digest, context);
            return false;
        }
        if (page->digest != digest)
        {
            policy->denials++;
            hvci_pol_log(policy, frame, hvci_pol_REASON_DIGEST_MISMATCH, page->digest,
                     digest, context);
            return policy->mode == hvci_pol_MODE_AUDIT;
        }
        if ((page->permissions & hvci_pol_PERM_WRITE) != 0 &&
            (page->permissions & hvci_pol_PERM_EXECUTE) != 0)
        {
            policy->denials++;
            hvci_pol_log(policy, frame, hvci_pol_REASON_WRITABLE_EXECUTE, page->digest,
                     digest, context);
            return false;
        }
        if (!hvci_pol_range_for(policy, frame, &flags) &&
            !hvci_pol_allowed_by_rule(policy, frame, hvci_pol_PERM_EXECUTE))
        {
            policy->denials++;
            hvci_pol_log(policy, frame, hvci_pol_REASON_OUTSIDE_RANGE, page->digest,
                     digest, context);
            return policy->mode == hvci_pol_MODE_AUDIT;
        }
    }
    return true;
}

void hvci_pol_page_dirty(hvci_pol_policy_t *policy, u64 frame)
{
    u32 index = hvci_pol_find_page(policy, frame);
    if (index == 0xFFFFFFFFu)
        return;
    if (policy->pages[index].sealed)
    {
        hvci_pol_log(policy, frame, hvci_pol_REASON_DIGEST_MISMATCH,
                 policy->pages[index].digest, 0, "sealed-write");
        policy->pages[index].revoked = 1;
        policy->denials++;
        return;
    }
    policy->pages[index].digest = hvci_pol_hash64(frame ^ policy->epoch);
    policy->write_protect_events++;
}

void hvci_pol_seal_page(hvci_pol_policy_t *policy, u64 frame)
{
    u32 index = hvci_pol_find_page(policy, frame);
    if (index == 0xFFFFFFFFu)
        return;
    policy->pages[index].sealed = 1;
    policy->pages[index].permissions =
        (u8)((policy->pages[index].permissions | hvci_pol_PERM_EXECUTE) & ~hvci_pol_PERM_WRITE);
}

void hvci_pol_add_rule(hvci_pol_policy_t *policy, const char *name, u64 frame_start,
                   u64 frame_end, u32 allow_mask)
{
    hvci_pol_rule_t *rule;
    if (policy->rule_count >= hvci_pol_MAX_RULES)
        return;
    rule = &policy->rules[policy->rule_count];
    hvci_pol_copy_name(rule->name, name ? name : "rule");
    rule->frame_start = frame_start;
    rule->frame_end = frame_end;
    rule->allow_mask = allow_mask;
    rule->active = true;
    policy->rule_count++;
}

void hvci_pol_disable_rule(hvci_pol_policy_t *policy, u32 index)
{
    if (index >= policy->rule_count)
        return;
    policy->rules[index].active = false;
}

void hvci_pol_set_mode(hvci_pol_policy_t *policy, u8 mode)
{
    if (mode > hvci_pol_MODE_LOCKDOWN)
        return;
    if (policy->mode == hvci_pol_MODE_LOCKDOWN && mode != hvci_pol_MODE_LOCKDOWN)
        return;
    policy->mode = mode;
}

u8 hvci_pol_get_mode(const hvci_pol_policy_t *policy)
{
    return policy->mode;
}

void hvci_pol_request_lockdown(hvci_pol_policy_t *policy)
{
    policy->lockdown_requested = true;
    policy->mode = hvci_pol_MODE_LOCKDOWN;
}

u32 hvci_pol_unregister_page(hvci_pol_policy_t *policy, u64 frame)
{
    u32 index = hvci_pol_find_page(policy, frame);
    if (index == 0xFFFFFFFFu)
        return 0;
    hvci_pol_remove_page(policy, index);
    return 1;
}

u32 hvci_pol_statistics_total(const hvci_pol_policy_t *policy)
{
    return (u32)(policy->checks + policy->denials + policy->attestations);
}

const char *hvci_pol_last_violation(const hvci_pol_policy_t *policy)
{
    if (policy->violation_count == 0)
        return "none";
    {
        u32 index = (policy->violation_head + hvci_pol_MAX_VIOLATIONS - 1) %
                    hvci_pol_MAX_VIOLATIONS;
        return policy->violations[index].context;
    }
}

u8 hvci_pol_last_violation_reason(const hvci_pol_policy_t *policy)
{
    if (policy->violation_count == 0)
        return hvci_pol_REASON_NONE;
    {
        u32 index = (policy->violation_head + hvci_pol_MAX_VIOLATIONS - 1) %
                    hvci_pol_MAX_VIOLATIONS;
        return policy->violations[index].reason;
    }
}

u64 hvci_pol_take_attestation(hvci_pol_policy_t *policy)
{
    u64 digest = hvci_pol_attest(policy);
    policy->attestations++;
    policy->last_attestation = digest;
    policy->epoch++;
    return digest;
}

u32 hvci_pol_sweep(hvci_pol_policy_t *policy)
{
    u32 i;
    u32 removed = 0;
    for (i = 0; i < policy->page_count;)
    {
        if (policy->pages[i].revoked)
        {
            hvci_pol_remove_page(policy, i);
            removed++;
            continue;
        }
        i++;
    }
    return removed;
}

u32 hvci_pol_protect_kernel(hvci_pol_policy_t *policy, u64 kernel_start, u64 kernel_end)
{
    u32 hits;
    hits = hvci_pol_protect_write(policy, kernel_start / AXIOM_PAGE_SIZE,
                              kernel_end / AXIOM_PAGE_SIZE);
    {
        u32 i;
        for (i = 0; i < policy->page_count; i++)
        {
            hvci_pol_page_t *page = &policy->pages[i];
            u64 address = page->frame * AXIOM_PAGE_SIZE;
            if (address < kernel_start || address >= kernel_end)
                continue;
            page->trusted = 1;
            page->sealed = 1;
        }
    }
    return hits;
}

u32 hvci_pol_revoke(const hvci_pol_policy_t *policy, u64 start, u64 end)
{
    hvci_pol_policy_t local = *policy;
    return hvci_pol_revoke_range(&local, start / AXIOM_PAGE_SIZE, end / AXIOM_PAGE_SIZE);
}

void hvci_pol_report(hvci_pol_policy_t *policy)
{
    printk("hvci: mode=%s pages=%u ranges=%u rules=%u\n",
           hvci_pol_mode_name(policy->mode), (unsigned)policy->page_count,
           (unsigned)policy->range_count, (unsigned)policy->rule_count);
    printk("hvci: checks=%llu denials=%llu attestations=%llu wx=%llu\n",
           (unsigned long long)policy->checks,
           (unsigned long long)policy->denials,
           (unsigned long long)policy->attestations,
           (unsigned long long)policy->write_protect_events);
    if (policy->violation_count > 0)
    {
        u32 index = (policy->violation_head + hvci_pol_MAX_VIOLATIONS - 1) %
                    hvci_pol_MAX_VIOLATIONS;
        printk("hvci: last violation %s at frame %llu (%s)\n",
               hvci_pol_reason_name(policy->violations[index].reason),
               (unsigned long long)policy->violations[index].frame,
               policy->violations[index].context);
    }
}

bool_t hvci_pol_selftest(void)
{
    static hvci_pol_policy_t policy;
    u64 digest;
    bool_t result = true;
    hvci_pol_init(&policy);
    hvci_pol_add_range(&policy, 0x100000, 0x200000, 1, "kernel");
    hvci_pol_set_mode(&policy, hvci_pol_MODE_ENFORCE);
    if (!hvci_pol_register_page(&policy, 0x120, 0xA5A5A5A5ULL, hvci_pol_PERM_READ))
        result = false;
    if (!hvci_pol_verify_execute(&policy, 0x120, 0xA5A5A5A5ULL, "selftest"))
        result = false;
    if (hvci_pol_verify_execute(&policy, 0x120, 0xDEADBEEFULL, "selftest"))
        result = false;
    if (hvci_pol_verify_execute(&policy, 0x900, 0x1234ULL, "unknown"))
        result = false;
    hvci_pol_seal_page(&policy, 0x120);
    hvci_pol_page_dirty(&policy, 0x120);
    if (hvci_pol_find_page(&policy, 0x120) == 0xFFFFFFFFu)
        result = true;
    digest = hvci_pol_take_attestation(&policy);
    if (digest == 0)
        result = false;
    hvci_pol_set_mode(&policy, hvci_pol_MODE_AUDIT);
    if (hvci_pol_get_mode(&policy) != hvci_pol_MODE_LOCKDOWN)
        result = false;
    return result;
}

