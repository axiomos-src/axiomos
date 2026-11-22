/* AxiomOS - multi level feedback queue scheduling policy.
 *
 * The base scheduler (sched.c) is round robin.  This module provides the
 * policy layer used when the kernel runs in "priority" mode: threads are
 * classified into five bands, each band owns a decayed credit counter and a
 * quantum measured in timer ticks.  Bands are scanned from the highest
 * priority downwards; a band is skipped when its credit pool is empty, which
 * keeps interactive threads responsive without starving background work.
 *
 * Additional pieces: run queue insertion by deadline, priority inheritance
 * for mutex style locks, and load accounting for the system monitor.
 */

#include <axiom/kernel.h>

#define AXPR_BANDS          5
#define AXPR_MAX_ENTRIES    128
#define AXPR_MAX_LOCKS      32
#define AXPR_NAME_LENGTH    24

enum axsp_class
{
    AXPR_CLASS_REALTIME = 0,
    AXPR_CLASS_INTERACTIVE,
    AXPR_CLASS_NORMAL,
    AXPR_CLASS_BATCH,
    AXPR_CLASS_IDLE
};

typedef struct axsp_entry
{
    u64 pid;
    u8  band;           /* 0 highest .. 4 lowest */
    u8  class_of_service;
    u8  state;          /* 0 empty, 1 queued, 2 running, 3 blocked */
    u16 quantum;        /* ticks remaining in this slice */
    u16 quantum_base;
    u32 credit;         /* decayed credit, refilled per epoch */
    u32 cpu_ticks;
    u32 wakeups;
    u32 migrations;
    u64 deadline;       /* absolute tick for real time entries */
    u64 blocked_since;
    char name[AXPR_NAME_LENGTH];
} axsp_entry_t;

typedef struct axsp_lock
{
    u64 owner;
    u64 waiter[8];
    u8  waiter_count;
    u8  active;
    u32 acquisitions;
    u32 contentions;
    u32 boosts;
} axsp_lock_t;

typedef struct axsp_policy
{
    axsp_entry_t entries[AXPR_MAX_ENTRIES];
    u32          count;
    axsp_lock_t  locks[AXPR_MAX_LOCKS];
    u32          lock_count;
    u32          band_credit[AXPR_BANDS];
    u32          band_quantum[AXPR_BANDS];
    u64          tick;
    u64          switches;
    u64          expiries;
    u64          boosts;
    u64          starvation_guard;
    u8           current_band;
    u8           current_entry;
    bool_t       initialised;
} axsp_policy_t;

static void axsp_zero(void *ptr, u32 size);
static void axsp_copy(char *dst, const char *src);
static u32  axsp_find(const axsp_policy_t *policy, u64 pid);
static u8   axsp_band_for(u8 class_of_service);
static u32  axsp_quantum_for(u8 band);
static u8   axsp_highest_nonempty(const axsp_policy_t *policy);
static void axsp_refill(axsp_policy_t *policy, u8 band);
static void axsp_decay(axsp_policy_t *policy);
static void axsp_insert(axsp_policy_t *policy, u64 pid, u8 class_of_service,
                        const char *name);
static void axsp_remove(axsp_policy_t *policy, u64 pid);
static u64  axsp_pick(const axsp_policy_t *policy);
static void axsp_charge(axsp_policy_t *policy, u32 ticks);
static u8   axsp_demote(u8 band);
static u8   axsp_promote(u8 band);
static void axsp_inherit(axsp_policy_t *policy, u64 owner);
static u64  axsp_lock_acquire(axsp_policy_t *policy, u32 lock, u64 pid);
static void axsp_lock_release(axsp_policy_t *policy, u32 lock, u64 pid);
static u32  axsp_load_permille(const axsp_policy_t *policy, u8 band);
static const char *axsp_band_name(u8 band);

static void axsp_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axsp_copy(char *dst, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < AXPR_NAME_LENGTH)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static u32 axsp_find(const axsp_policy_t *policy, u64 pid)
{
    u32 i;
    for (i = 0; i < policy->count; i++)
    {
        if (policy->entries[i].pid == pid)
            return i;
    }
    return 0xFFFFFFFFu;
}

static u8 axsp_band_for(u8 class_of_service)
{
    switch (class_of_service)
    {
    case AXPR_CLASS_REALTIME: return 0;
    case AXPR_CLASS_INTERACTIVE: return 1;
    case AXPR_CLASS_NORMAL: return 2;
    case AXPR_CLASS_BATCH: return 3;
    default: return 4;
    }
    return 2;
}

static u32 axsp_quantum_for(u8 band)
{
    switch (band)
    {
    case 0: return 4;
    case 1: return 8;
    case 2: return 12;
    case 3: return 20;
    default: return 32;
    }
}

static u8 axsp_highest_nonempty(const axsp_policy_t *policy)
{
    u8 band;
    for (band = 0; band < AXPR_BANDS; band++)
    {
        u32 i;
        for (i = 0; i < policy->count; i++)
        {
            if (policy->entries[i].state == 1 && policy->entries[i].band == band)
                return band;
        }
    }
    return AXPR_BANDS;
}

static void axsp_refill(axsp_policy_t *policy, u8 band)
{
    u32 i;
    u32 queued = 0;
    for (i = 0; i < policy->count; i++)
    {
        if (policy->entries[i].band == band && policy->entries[i].state != 0)
            queued++;
    }
    policy->band_credit[band] = queued * policy->band_quantum[band];
}

static void axsp_decay(axsp_policy_t *policy)
{
    u8 band;
    for (band = 0; band < AXPR_BANDS; band++)
    {
        policy->band_credit[band] = (policy->band_credit[band] * 7) / 8;
        axsp_refill(policy, band);
    }
}

static void axsp_insert(axsp_policy_t *policy, u64 pid, u8 class_of_service,
                        const char *name)
{
    u32 index;
    axsp_entry_t *entry;
    if (axsp_find(policy, pid) != 0xFFFFFFFFu)
        return;
    if (policy->count >= AXPR_MAX_ENTRIES)
        return;
    index = policy->count;
    entry = &policy->entries[index];
    axsp_zero(entry, (u32)sizeof(*entry));
    entry->pid = pid;
    entry->band = axsp_band_for(class_of_service);
    entry->class_of_service = class_of_service;
    entry->state = 1;
    entry->quantum = (u16)axsp_quantum_for(entry->band);
    entry->quantum_base = entry->quantum;
    entry->credit = 64;
    axsp_copy(entry->name, name);
    policy->count++;
    axsp_refill(policy, entry->band);
}

static void axsp_remove(axsp_policy_t *policy, u64 pid)
{
    u32 index = axsp_find(policy, pid);
    u32 i;
    if (index == 0xFFFFFFFFu)
        return;
    for (i = index; i + 1 < policy->count; i++)
        policy->entries[i] = policy->entries[i + 1];
    policy->count--;
}

static u64 axsp_pick(const axsp_policy_t *policy)
{
    u8 band = axsp_highest_nonempty(policy);
    u64 best = 0;
    u32 best_credit = 0;
    u32 i;
    if (band >= AXPR_BANDS)
        return 0;
    for (i = 0; i < policy->count; i++)
    {
        const axsp_entry_t *entry = &policy->entries[i];
        if (entry->state != 1 || entry->band != band)
            continue;
        if (best == 0)
        {
            best = entry->pid;
            best_credit = entry->credit;
            continue;
        }
        if (entry->deadline != 0)
        {
            if (best_credit == 0)
            {
                best = entry->pid;
                best_credit = entry->credit;
            }
            continue;
        }
        if (entry->credit < best_credit)
        {
            best = entry->pid;
            best_credit = entry->credit;
        }
    }
    return best;
}

static void axsp_charge(axsp_policy_t *policy, u32 ticks)
{
    u8 band = policy->current_band;
    if (band >= AXPR_BANDS)
        return;
    if (policy->band_credit[band] > ticks)
        policy->band_credit[band] -= ticks;
    else
        policy->band_credit[band] = 0;
}

static u8 axsp_demote(u8 band)
{
    if (band + 1 < AXPR_BANDS)
        return (u8)(band + 1);
    return band;
}

static u8 axsp_promote(u8 band)
{
    if (band > 0)
        return (u8)(band - 1);
    return band;
}

static void axsp_inherit(axsp_policy_t *policy, u64 owner)
{
    u32 index = axsp_find(policy, owner);
    if (index == 0xFFFFFFFFu)
        return;
    if (policy->entries[index].band == 0)
        return;
    policy->entries[index].band = axsp_promote(policy->entries[index].band);
    policy->entries[index].quantum = (u16)axsp_quantum_for(policy->entries[index].band);
    policy->entries[index].quantum_base = policy->entries[index].quantum;
    policy->boosts++;
}

static u64 axsp_lock_acquire(axsp_policy_t *policy, u32 lock, u64 pid)
{
    axsp_lock_t *entry;
    if (lock >= policy->lock_count)
        return 0;
    entry = &policy->locks[lock];
    if (!entry->active)
    {
        entry->active = 1;
        entry->owner = pid;
        entry->acquisitions++;
        return pid;
    }
    if (entry->owner == pid)
        return pid;
    entry->contentions++;
    if (entry->waiter_count < 8)
    {
        entry->waiter[entry->waiter_count] = pid;
        entry->waiter_count++;
    }
    axsp_inherit(policy, entry->owner);
    {
        u32 index = axsp_find(policy, pid);
        if (index != 0xFFFFFFFFu)
        {
            policy->entries[index].state = 3;
            policy->entries[index].blocked_since = policy->tick;
        }
    }
    return entry->owner;
}

static void axsp_lock_release(axsp_policy_t *policy, u32 lock, u64 pid)
{
    axsp_lock_t *entry;
    u64 next = 0;
    if (lock >= policy->lock_count)
        return;
    entry = &policy->locks[lock];
    if (!entry->active || entry->owner != pid)
        return;
    if (entry->waiter_count > 0)
    {
        u32 i;
        next = entry->waiter[0];
        for (i = 0; i + 1 < entry->waiter_count; i++)
            entry->waiter[i] = entry->waiter[i + 1];
        entry->waiter_count--;
    }
    if (next != 0)
    {
        u32 index = axsp_find(policy, next);
        entry->owner = next;
        entry->acquisitions++;
        if (index != 0xFFFFFFFFu)
        {
            policy->entries[index].state = 1;
            policy->entries[index].credit = 96;
        }
        return;
    }
    entry->active = 0;
    entry->owner = 0;
}

static u32 axsp_load_permille(const axsp_policy_t *policy, u8 band)
{
    u32 total = 0;
    u32 queued = 0;
    u32 i;
    for (i = 0; i < policy->count; i++)
    {
        total += policy->entries[i].cpu_ticks;
        if (policy->entries[i].band == band)
            queued++;
    }
    if (total == 0)
        return 0;
    return queued * 1000 / total;
}

static const char *axsp_band_name(u8 band)
{
    switch (band)
    {
    case 0: return "realtime";
    case 1: return "interactive";
    case 2: return "normal";
    case 3: return "batch";
    default: return "idle";
    }
}

/* -------------------------------------------------------------- public API */

void axsp_init(axsp_policy_t *policy)
{
    u8 band;
    axsp_zero(policy, (u32)sizeof(*policy));
    for (band = 0; band < AXPR_BANDS; band++)
    {
        policy->band_quantum[band] = axsp_quantum_for(band);
        policy->band_credit[band] = policy->band_quantum[band];
    }
    policy->current_band = AXPR_BANDS;
    policy->current_entry = 0xFF;
    policy->initialised = true;
}

u32 axsp_add(axsp_policy_t *policy, u64 pid, u8 class_of_service, const char *name)
{
    axsp_insert(policy, pid, class_of_service, name);
    return policy->count;
}

u32 axsp_remove_entry(axsp_policy_t *policy, u64 pid)
{
    u32 index = axsp_find(policy, pid);
    if (index == 0xFFFFFFFFu)
        return 0;
    axsp_remove(policy, pid);
    return 1;
}

u64 axsp_next(axsp_policy_t *policy)
{
    u64 pid;
    if (policy->count == 0)
        return 0;
    axsp_decay(policy);
    pid = axsp_pick(policy);
    if (pid == 0)
    {
        policy->starvation_guard++;
        return 0;
    }
    {
        u32 index = axsp_find(policy, pid);
        if (index != 0xFFFFFFFFu)
        {
            policy->entries[index].state = 2;
            policy->current_band = policy->entries[index].band;
            policy->current_entry = (u8)index;
            policy->entries[index].quantum = policy->entries[index].quantum_base;
        }
    }
    policy->switches++;
    return pid;
}

void axsp_tick(axsp_policy_t *policy)
{
    policy->tick++;
    if (policy->current_entry == 0xFF)
        return;
    if (policy->current_entry >= policy->count)
    {
        policy->current_entry = 0xFF;
        return;
    }
    axsp_charge(policy, 1);
    {
        axsp_entry_t *entry = &policy->entries[policy->current_entry];
        entry->cpu_ticks++;
        if (entry->quantum > 0)
            entry->quantum--;
        if (entry->quantum == 0)
        {
            policy->expiries++;
            entry->state = 1;
            if (entry->credit > 0)
                entry->credit--;
            entry->band = axsp_demote(entry->band);
            entry->quantum = (u16)axsp_quantum_for(entry->band);
            entry->quantum_base = entry->quantum;
        }
    }
}

void axsp_wake(axsp_policy_t *policy, u64 pid)
{
    u32 index = axsp_find(policy, pid);
    if (index == 0xFFFFFFFFu)
        return;
    policy->entries[index].state = 1;
    policy->entries[index].wakeups++;
    policy->entries[index].credit += 24;
    if (policy->entries[index].credit > 255)
        policy->entries[index].credit = 255;
    if (policy->tick - policy->entries[index].blocked_since > 64)
        policy->entries[index].band = axsp_promote(policy->entries[index].band);
}

void axsp_block(axsp_policy_t *policy, u64 pid)
{
    u32 index = axsp_find(policy, pid);
    if (index == 0xFFFFFFFFu)
        return;
    policy->entries[index].state = 3;
    policy->entries[index].blocked_since = policy->tick;
}

void axsp_set_class(axsp_policy_t *policy, u64 pid, u8 class_of_service)
{
    u32 index = axsp_find(policy, pid);
    if (index == 0xFFFFFFFFu)
        return;
    policy->entries[index].class_of_service = class_of_service;
    policy->entries[index].band = axsp_band_for(class_of_service);
    policy->entries[index].quantum = (u16)axsp_quantum_for(policy->entries[index].band);
    policy->entries[index].quantum_base = policy->entries[index].quantum;
}

u32 axsp_add_lock(axsp_policy_t *policy)
{
    if (policy->lock_count >= AXPR_MAX_LOCKS)
        return 0xFFFFFFFFu;
    axsp_zero(&policy->locks[policy->lock_count], (u32)sizeof(axsp_lock_t));
    policy->lock_count++;
    return policy->lock_count - 1;
}

u32 axsp_acquire(axsp_policy_t *policy, u32 lock, u64 pid)
{
    axsp_lock_acquire(policy, lock, pid);
    if (lock < policy->lock_count)
        return policy->locks[lock].contentions;
    return 0;
}

void axsp_release(axsp_policy_t *policy, u32 lock, u64 pid)
{
    axsp_lock_release(policy, lock, pid);
}

u8 axsp_band_of(const axsp_policy_t *policy, u64 pid)
{
    u32 index = axsp_find(policy, pid);
    if (index == 0xFFFFFFFFu)
        return 0xFF;
    return policy->entries[index].band;
}

u32 axsp_cpu_ticks(const axsp_policy_t *policy, u64 pid)
{
    u32 index = axsp_find(policy, pid);
    if (index == 0xFFFFFFFFu)
        return 0;
    return policy->entries[index].cpu_ticks;
}

u32 axsp_balance(axsp_policy_t *policy)
{
    u32 i;
    u32 moved = 0;
    u8 lowest_full = AXPR_BANDS;
    u8 band;
    if (policy->count == 0)
        return 0;
    for (band = 0; band < AXPR_BANDS; band++)
    {
        if (axsp_load_permille(policy, band) > 400)
        {
            lowest_full = band;
            break;
        }
    }
    if (lowest_full == AXPR_BANDS)
        return 0;
    for (i = 0; i < policy->count; i++)
    {
        if (policy->entries[i].band != lowest_full)
            continue;
        if (policy->entries[i].state != 1)
            continue;
        policy->entries[i].band = axsp_promote(policy->entries[i].band);
        policy->entries[i].migrations++;
        moved++;
        if (moved >= 4)
            break;
    }
    return moved;
}

u32 axsp_promote_channel(axsp_policy_t *policy, u64 pid)
{
    u32 index = axsp_find(policy, pid);
    if (index == 0xFFFFFFFFu)
        return 0;
    policy->entries[index].band = axsp_promote(policy->entries[index].band);
    return policy->entries[index].band;
}

void axsp_report(const axsp_policy_t *policy)
{
    u8 band;
    printk("axpr: entries=%u switches=%llu expiries=%llu boosts=%llu\n",
           (unsigned)policy->count, (unsigned long long)policy->switches,
           (unsigned long long)policy->expiries,
           (unsigned long long)policy->boosts);
    for (band = 0; band < AXPR_BANDS; band++)
    {
        printk("axpr: band %-11s credit=%u quantum=%u\n",
               axsp_band_name(band), (unsigned)policy->band_credit[band],
               (unsigned)policy->band_quantum[band]);
    }
}

bool_t axsp_selftest(void)
{
    static axsp_policy_t policy;
    u32 lock;
    axsp_init(&policy);
    axsp_add(&policy, 1, AXPR_CLASS_INTERACTIVE, "shell");
    axsp_add(&policy, 2, AXPR_CLASS_BATCH, "indexer");
    axsp_add(&policy, 3, AXPR_CLASS_REALTIME, "audio");
    if (policy.count != 3)
        return false;
    if (axsp_band_of(&policy, 3) != 0)
        return false;
    if (axsp_band_of(&policy, 2) != 3)
        return false;
    if (axsp_next(&policy) != 3)
        return false;
    lock = axsp_add_lock(&policy);
    if (lock == 0xFFFFFFFFu)
        return false;
    if (axsp_acquire(&policy, lock, 1) != 0)
        return false;
    if (axsp_acquire(&policy, lock, 2) == 0)
        return false;
    axsp_release(&policy, lock, 1);
    if (policy.locks[lock].owner != 2)
        return false;
    {
        u32 i;
        for (i = 0; i < 8; i++)
            axsp_tick(&policy);
    }
    if (axsp_cpu_ticks(&policy, 3) == 0)
        return false;
    axsp_remove_entry(&policy, 2);
    if (policy.count != 2)
        return false;
    return true;
}
