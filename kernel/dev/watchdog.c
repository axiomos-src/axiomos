/* AxiomOS - hardware watchdog and soft lockup detection.
 *
 * Two independent timers are tracked here: the platform watchdog (which is
 * kicked by the scheduler when it observes forward progress) and the soft
 * lockup watchdog, which watches per-CPU progress counters sampled from the
 * timer interrupt.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXWD_MAX_CPUS        8
#define AXWD_MAX_STAGES      6
#define AXWD_KICK_TIMEOUT    60000
#define AXWD_SOFT_TIMEOUT    22000
#define AXWD_WARN_THRESHOLD  8000

#define AXWD_STAGE_PROBE     0
#define AXWD_STAGE_EARLY     1
#define AXWD_STAGE_MIDDLE    2
#define AXWD_STAGE_LATE      3
#define AXWD_STAGE_FINAL     4
#define AXWD_STAGE_UNKNOWN   5

typedef struct axwd_cpu
{
    u64  progress;
    u64  last_progress;
    u32  stalls;
    u32  kicks;
    u32  soft_warnings;
    u8   stage;
    bool_t online;
    bool_t wedged;
} axwd_cpu_t;

typedef struct axwd_state
{
    axwd_cpu_t cpus[AXWD_MAX_CPUS];
    u32  cpu_count;
    u32  hard_timeout;
    u32  soft_timeout;
    u32  hard_kicks;
    u32  hard_resets;
    u32  soft_resets;
    u32  watchdog_state;
    u32  magic;
    u32  tick;
    bool_t armed;
    bool_t hardware_present;
    bool_t in_panic;
} axwd_state_t;

static axwd_state_t axwd_state;
static u32 axwd_stage_stamp[AXWD_MAX_STAGES];

static void axwd_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

/* -------------------------------------------------------------- public API */

void axwd_init(u32 cpus, bool_t hardware, u32 hard_timeout, u32 soft_timeout)
{
    u32 i;
    axwd_zero(&axwd_state, (u32)sizeof(axwd_state));
    if (cpus > AXWD_MAX_CPUS)
        cpus = AXWD_MAX_CPUS;
    axwd_state.cpu_count = cpus;
    axwd_state.hardware_present = hardware;
    axwd_state.hard_timeout = hard_timeout == 0 ? AXWD_KICK_TIMEOUT : hard_timeout;
    axwd_state.soft_timeout = soft_timeout == 0 ? AXWD_SOFT_TIMEOUT : soft_timeout;
    axwd_state.armed = true;
    axwd_state.magic = 0xA5A5C0DEu;
    for (i = 0; i < cpus; i++)
    {
        axwd_state.cpus[i].online = true;
        axwd_state.cpus[i].stage = AXWD_STAGE_UNKNOWN;
        axwd_state.cpus[i].progress = 1;
        axwd_state.cpus[i].last_progress = 1;
    }
    for (i = 0; i < AXWD_MAX_STAGES; i++)
        axwd_stage_stamp[i] = 0;
}

void axwd_mark_stage(u32 stage, u32 stamp)
{
    if (stage >= AXWD_MAX_STAGES)
        stage = AXWD_STAGE_UNKNOWN;
    axwd_stage_stamp[stage] = stamp;
}

u32 axwd_stage_stamp_value(u32 stage)
{
    if (stage >= AXWD_MAX_STAGES)
        return 0;
    return axwd_stage_stamp[stage];
}

void axwd_progress(u32 cpu)
{
    if (cpu >= axwd_state.cpu_count)
        return;
    axwd_state.cpus[cpu].progress++;
    axwd_state.cpus[cpu].last_progress = axwd_state.tick;
}

bool_t axwd_kick(u32 cpu)
{
    if (!axwd_state.armed || cpu >= axwd_state.cpu_count)
        return false;
    axwd_state.cpus[cpu].kicks++;
    axwd_state.hard_kicks++;
    axwd_state.watchdog_state = 1;
    return true;
}

u32 axwd_check(u32 cpu)
{
    axwd_cpu_t *entry;
    u32 elapsed;
    if (!axwd_state.armed || cpu >= axwd_state.cpu_count)
        return 0;
    entry = &axwd_state.cpus[cpu];
    elapsed = axwd_state.tick - (u32)entry->last_progress;
    if (elapsed == 0)
        return 0;
    if (elapsed >= axwd_state.soft_timeout)
    {
        entry->stalls++;
        entry->wedged = true;
        entry->soft_warnings++;
        axwd_state.soft_resets++;
        axwd_mark_stage(AXWD_STAGE_FINAL, axwd_state.tick);
        return elapsed;
    }
    if (elapsed >= AXWD_WARN_THRESHOLD)
    {
        entry->soft_warnings++;
        return elapsed;
    }
    return 0;
}

u32 axwd_tick_all(void)
{
    u32 i;
    u32 problematic = 0;
    axwd_state.tick++;
    for (i = 0; i < axwd_state.cpu_count; i++)
    {
        if (!axwd_state.cpus[i].online)
            continue;
        if (axwd_check(i) != 0)
            problematic++;
    }
    if (axwd_state.watchdog_state == 2)
        problematic++;
    return problematic;
}

bool_t axwd_hard_reset(u32 cpu)
{
    if (cpu >= axwd_state.cpu_count)
        return false;
    axwd_state.hard_resets++;
    axwd_state.watchdog_state = 2;
    axwd_state.cpus[cpu].wedged = true;
    return true;
}

void axwd_panic(void)
{
    axwd_state.in_panic = true;
    axwd_state.armed = false;
}

void axwd_disarm(void)
{
    axwd_state.armed = false;
}

bool_t axwd_armed(void)
{
    return axwd_state.armed;
}

u32 axwd_cpu_online_mask(void)
{
    u32 mask = 0;
    u32 i;
    for (i = 0; i < axwd_state.cpu_count; i++)
    {
        if (axwd_state.cpus[i].online)
            mask |= (1u << i);
    }
    return mask;
}

void axwd_report(void)
{
    u32 i;
    printk("axwd: armed=%u hw=%u cpus=%u hard=%u soft=%u state=%u panic=%u\n",
           (unsigned)axwd_state.armed, (unsigned)axwd_state.hardware_present,
           (unsigned)axwd_state.cpu_count, (unsigned)axwd_state.hard_timeout,
           (unsigned)axwd_state.soft_timeout, (unsigned)axwd_state.watchdog_state,
           (unsigned)axwd_state.in_panic);
    for (i = 0; i < axwd_state.cpu_count; i++)
    {
        const axwd_cpu_t *entry = &axwd_state.cpus[i];
        printk("  cpu %u progress=%lu stalls=%u kicks=%u warns=%u wedged=%u\n",
               (unsigned)i, (unsigned long)entry->progress, (unsigned)entry->stalls,
               (unsigned)entry->kicks, (unsigned)entry->soft_warnings,
               (unsigned)entry->wedged);
    }
}

bool_t axwd_selftest(void)
{
    axwd_init(2, true, 100, 50);
    if (!axwd_armed())
        return false;
    axwd_mark_stage(AXWD_STAGE_EARLY, 10);
    if (axwd_stage_stamp_value(AXWD_STAGE_EARLY) != 10)
        return false;
    if (!axwd_kick(0))
        return false;
    axwd_progress(0);
    if (axwd_check(0) != 0)
        return false;
    axwd_state.tick = 40;
    if (axwd_check(0) == 0)
        return false;
    axwd_state.tick = 80;
    if (axwd_check(0) < 50)
        return false;
    if (!axwd_hard_reset(1))
        return false;
    if (axwd_cpu_online_mask() != 3)
        return false;
    axwd_panic();
    if (axwd_armed())
        return false;
    return true;
}

