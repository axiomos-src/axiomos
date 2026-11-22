/* AxiomOS - seccomp style syscall filtering.
 *
 * Every task may attach a filter: an ordered list of rules, each matching a
 * syscall number and an optional argument mask, with an action of allow, deny,
 * kill, trap or log.  Filters are evaluated in constant time per rule and the
 * decision is cached for repeated identical calls so a hot path pays only one
 * comparison after the first fault.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXSC_MAX_FILTERS       8
#define AXSC_MAX_RULES         64
#define AXSC_MAX_ARGUMENTS     6
#define AXSC_MAX_TASKS         32
#define AXSC_CACHE_SLOTS       16

#define AXSC_ACTION_ALLOW      0
#define AXSC_ACTION_DENY       1
#define AXSC_ACTION_KILL       2
#define AXSC_ACTION_TRAP       3
#define AXSC_ACTION_LOG        4
#define AXSC_ACTION_ERRNO      5

#define AXSC_MODE_DISABLED     0
#define AXSC_MODE_ENFORCING    1
#define AXSC_MODE_LEARNING     2

#define AXSC_ARG_MASK_BITS     6

typedef struct axsc_rule
{
    u32  syscall;
    u32  argument_index;
    u64  argument_value;
    u64  argument_mask;
    u32  action;
    u32  errno_value;
    u32  hits;
    u8   argument_count;
    bool_t has_argument;
    bool_t exact_match;
} axsc_rule_t;

typedef struct axsc_filter
{
    axsc_rule_t rules[AXSC_MAX_RULES];
    u32  rule_count;
    u32  total_hits;
    u32  allow_count;
    u32  deny_count;
    u32  kill_count;
    u32  trap_count;
    u16  name;
    u8   mode;
    bool_t has_default_deny;
    bool_t inherited;
} axsc_filter_t;

typedef struct axsc_cache_entry
{
    u32  syscall;
    u32  action;
    u32  filter_generation;
    bool_t valid;
} axsc_cache_entry_t;

typedef struct axsc_state
{
    axsc_filter_t filters[AXSC_MAX_FILTERS];
    axsc_cache_entry_t cache[AXSC_CACHE_SLOTS];
    u32  filter_count;
    u32  attached_tasks;
    u32  evaluations;
    u32  denials;
    u32  kills;
    u32  traps;
    u32  logs;
    u32  cache_hits;
    u32  cache_misses;
    u32  generation;
    u32  pattern_rejects;
    bool_t initialized;
} axsc_state_t;

static axsc_state_t axsc_state;

static void axsc_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axsc_hash(u32 syscall)
{
    u32 value = syscall * 2654435761u;
    return (value >> 16) % AXSC_CACHE_SLOTS;
}

static void axsc_cache_store(u32 syscall, u32 action, u32 generation)
{
    axsc_cache_entry_t *slot = &axsc_state.cache[axsc_hash(syscall)];
    slot->syscall = syscall;
    slot->action = action;
    slot->filter_generation = generation;
    slot->valid = true;
}

static bool_t axsc_cache_lookup(u32 syscall, u32 *action)
{
    axsc_cache_entry_t *slot = &axsc_state.cache[axsc_hash(syscall)];
    if (!slot->valid || slot->syscall != syscall || slot->filter_generation != axsc_state.generation)
        return false;
    *action = slot->action;
    return true;
}

static bool_t axsc_argument_matches(const axsc_rule_t *rule, const u64 *arguments)
{
    u64 value;
    if (!rule->has_argument)
        return true;
    if (rule->argument_index >= AXSC_MAX_ARGUMENTS)
        return false;
    value = arguments == NULL_PTR ? 0 : arguments[rule->argument_index];
    if (rule->exact_match)
        return value == rule->argument_value;
    return (value & rule->argument_mask) == rule->argument_value;
}

static axsc_filter_t *axsc_filter(u32 index)
{
    if (index >= axsc_state.filter_count)
        return NULL_PTR;
    return &axsc_state.filters[index];
}

/* -------------------------------------------------------------- public API */

void axsc_init(void)
{
    axsc_zero(&axsc_state, (u32)sizeof(axsc_state));
    axsc_state.initialized = true;
}

u32 axsc_create(u16 name, u8 mode)
{
    axsc_filter_t *filter;
    if (!axsc_state.initialized || axsc_state.filter_count >= AXSC_MAX_FILTERS)
        return 0xFFFFFFFFu;
    filter = &axsc_state.filters[axsc_state.filter_count];
    axsc_zero(filter, (u32)sizeof(*filter));
    filter->name = name;
    filter->mode = mode > AXSC_MODE_LEARNING ? AXSC_MODE_ENFORCING : mode;
    filter->has_default_deny = false;
    axsc_state.filter_count++;
    axsc_state.generation++;
    return (u32)(filter - axsc_state.filters);
}

bool_t axsc_rule(u32 filter_index, u32 syscall, u32 action, u32 errno_value)
{
    axsc_filter_t *filter = axsc_filter(filter_index);
    axsc_rule_t *rule;
    if (filter == NULL_PTR || filter->rule_count >= AXSC_MAX_RULES)
        return false;
    if (action > AXSC_ACTION_ERRNO)
        return false;
    rule = &filter->rules[filter->rule_count];
    axsc_zero(rule, (u32)sizeof(*rule));
    rule->syscall = syscall;
    rule->action = action;
    rule->errno_value = errno_value;
    rule->exact_match = false;
    filter->rule_count++;
    axsc_state.generation++;
    switch (action)
    {
    case AXSC_ACTION_ALLOW: filter->allow_count++; break;
    case AXSC_ACTION_DENY: filter->deny_count++; break;
    case AXSC_ACTION_KILL: filter->kill_count++; break;
    case AXSC_ACTION_TRAP: filter->trap_count++; break;
    default: break;
    }
    return true;
}

bool_t axsc_rule_argument(u32 filter_index, u32 syscall, u8 argument_index,
                          u64 mask, u64 expected, u32 action)
{
    axsc_filter_t *filter = axsc_filter(filter_index);
    axsc_rule_t *rule;
    if (filter == NULL_PTR || filter->rule_count >= AXSC_MAX_RULES)
        return false;
    if (argument_index >= AXSC_MAX_ARGUMENTS)
        return false;
    rule = &filter->rules[filter->rule_count];
    axsc_zero(rule, (u32)sizeof(*rule));
    rule->syscall = syscall;
    rule->action = action;
    rule->argument_index = argument_index;
    rule->argument_value = expected;
    rule->argument_mask = mask == 0 ? 0xFFFFFFFFFFFFFFFFull : mask;
    rule->argument_count = 1;
    rule->has_argument = true;
    rule->exact_match = mask == 0;
    filter->rule_count++;
    axsc_state.generation++;
    return true;
}

bool_t axsc_default_deny(u32 filter_index)
{
    axsc_filter_t *filter = axsc_filter(filter_index);
    if (filter == NULL_PTR)
        return false;
    filter->has_default_deny = true;
    axsc_state.generation++;
    return true;
}

u32 axsc_evaluate(u32 filter_index, u32 syscall, const u64 *arguments)
{
    axsc_filter_t *filter = axsc_filter(filter_index);
    u32 action = AXSC_ACTION_ALLOW;
    u32 cached = 0;
    if (filter == NULL_PTR)
        return AXSC_ACTION_KILL;
    axsc_state.evaluations++;
    if (axsc_cache_lookup(syscall, &cached))
    {
        axsc_state.cache_hits++;
        action = cached;
    }
    else
    {
        axsc_state.cache_misses++;
        for (u32 i = 0; i < filter->rule_count; i++)
        {
            axsc_rule_t *rule = &filter->rules[i];
            if (rule->syscall != syscall)
                continue;
            if (!axsc_argument_matches(rule, arguments))
                continue;
            rule->hits++;
            filter->total_hits++;
            action = rule->action;
            break;
        }
        if (action == AXSC_ACTION_ALLOW && filter->has_default_deny)
            action = AXSC_ACTION_DENY;
        axsc_cache_store(syscall, action, axsc_state.generation);
    }
    if (filter->mode != AXSC_MODE_ENFORCING)
        return AXSC_ACTION_ALLOW;
    switch (action)
    {
    case AXSC_ACTION_DENY: axsc_state.denials++; break;
    case AXSC_ACTION_KILL: axsc_state.kills++; break;
    case AXSC_ACTION_TRAP: axsc_state.traps++; break;
    case AXSC_ACTION_LOG: axsc_state.logs++; return AXSC_ACTION_ALLOW;
    default: break;
    }
    return action;
}

bool_t axsc_attach(u32 filter_index)
{
    axsc_filter_t *filter = axsc_filter(filter_index);
    if (filter == NULL_PTR)
        return false;
    if (axsc_state.attached_tasks >= AXSC_MAX_TASKS)
        return false;
    axsc_state.attached_tasks++;
    axsc_state.generation++;
    return true;
}

bool_t axsc_detach(u32 filter_index)
{
    axsc_filter_t *filter = axsc_filter(filter_index);
    if (filter == NULL_PTR || axsc_state.attached_tasks == 0)
        return false;
    axsc_state.attached_tasks--;
    filter->mode = AXSC_MODE_DISABLED;
    axsc_state.generation++;
    return true;
}

bool_t axsc_set_mode(u32 filter_index, u8 mode)
{
    axsc_filter_t *filter = axsc_filter(filter_index);
    if (filter == NULL_PTR || mode > AXSC_MODE_LEARNING)
        return false;
    filter->mode = mode;
    axsc_state.generation++;
    return true;
}

u32 axsc_filter_count(void)
{
    return axsc_state.filter_count;
}

u32 axsc_rule_count(u32 filter_index)
{
    axsc_filter_t *filter = axsc_filter(filter_index);
    return filter == NULL_PTR ? 0 : filter->rule_count;
}

bool_t axsc_rule_stats(u32 filter_index, u32 rule_index, u32 *syscall, u32 *hits)
{
    axsc_filter_t *filter = axsc_filter(filter_index);
    if (filter == NULL_PTR || rule_index >= filter->rule_count)
        return false;
    if (syscall != NULL_PTR)
        *syscall = filter->rules[rule_index].syscall;
    if (hits != NULL_PTR)
        *hits = filter->rules[rule_index].hits;
    return true;
}

u32 axsc_denials(void)
{
    return axsc_state.denials;
}

u32 axsc_cache_efficiency(void)
{
    u32 total = axsc_state.cache_hits + axsc_state.cache_misses;
    if (total == 0)
        return 0;
    return (axsc_state.cache_hits * 100) / total;
}

/* A tiny pattern matcher used to validate path style arguments. */
bool_t axsc_glob(const char *pattern, const char *text)
{
    u32 p = 0;
    u32 t = 0;
    u32 star = 0xFFFFFFFFu;
    u32 resume = 0;
    while (text[t] != 0)
    {
        if (pattern[p] == '*')
        {
            star = p;
            resume = t;
            p++;
        }
        else if (pattern[p] == '?' || pattern[p] == text[t])
        {
            p++;
            t++;
        }
        else if (star != 0xFFFFFFFFu)
        {
            resume++;
            t = resume;
            p = star + 1;
        }
        else
        {
            axsc_state.pattern_rejects++;
            return false;
        }
    }
    while (pattern[p] == '*')
        p++;
    return pattern[p] == 0;
}

void axsc_report(void)
{
    printk("axsc: filters=%u tasks=%u eval=%u denials=%u kills=%u traps=%u logs=%u\n",
           (unsigned)axsc_state.filter_count, (unsigned)axsc_state.attached_tasks,
           (unsigned)axsc_state.evaluations, (unsigned)axsc_state.denials,
           (unsigned)axsc_state.kills, (unsigned)axsc_state.traps, (unsigned)axsc_state.logs);
    printk("axsc: cache=%u%% hits=%u misses=%u generation=%u\n",
           (unsigned)axsc_cache_efficiency(), (unsigned)axsc_state.cache_hits,
           (unsigned)axsc_state.cache_misses, (unsigned)axsc_state.generation);
    for (u32 i = 0; i < axsc_state.filter_count; i++)
    {
        axsc_filter_t *filter = &axsc_state.filters[i];
        printk("  filter %u name=0x%x rules=%u mode=%u hits=%u default_deny=%u\n",
               (unsigned)i, (unsigned)filter->name, (unsigned)filter->rule_count,
               (unsigned)filter->mode, (unsigned)filter->total_hits,
               (unsigned)filter->has_default_deny);
    }
}

bool_t axsc_selftest(void)
{
    u64 arguments[AXSC_MAX_ARGUMENTS];
    axsc_init();
    u32 filter = axsc_create(0x4242, AXSC_MODE_ENFORCING);
    if (filter == 0xFFFFFFFFu)
        return false;
    if (!axsc_rule(filter, 0, AXSC_ACTION_ALLOW, 0))
        return false;
    if (!axsc_rule(filter, 1, AXSC_ACTION_DENY, 13))
        return false;
    if (!axsc_rule(filter, 2, AXSC_ACTION_KILL, 0))
        return false;
    for (u32 i = 0; i < AXSC_MAX_ARGUMENTS; i++)
        arguments[i] = i;
    if (axsc_evaluate(filter, 0, arguments) != AXSC_ACTION_ALLOW)
        return false;
    if (axsc_evaluate(filter, 1, arguments) != AXSC_ACTION_DENY)
        return false;
    if (axsc_evaluate(filter, 2, arguments) != AXSC_ACTION_KILL)
        return false;
    if (axsc_denials() != 1)
        return false;
    if (axsc_evaluate(filter, 1, arguments) != AXSC_ACTION_DENY)
        return false;
    if (axsc_cache_efficiency() == 0)
        return false;
    if (axsc_evaluate(filter, 9, arguments) != AXSC_ACTION_ALLOW)
        return false;
    if (!axsc_default_deny(filter))
        return false;
    if (axsc_evaluate(filter, 9, arguments) != AXSC_ACTION_DENY)
        return false;
    if (!axsc_rule_argument(filter, 3, 1, 0xFFFF, 0x0002, AXSC_ACTION_DENY))
        return false;
    if (axsc_evaluate(filter, 3, arguments) != AXSC_ACTION_DENY)
        return false;
    if (!axsc_set_mode(filter, AXSC_MODE_DISABLED))
        return false;
    if (axsc_evaluate(filter, 3, arguments) != AXSC_ACTION_ALLOW)
        return false;
    if (!axsc_set_mode(filter, AXSC_MODE_LEARNING))
        return false;
    if (axsc_evaluate(filter, 2, arguments) != AXSC_ACTION_ALLOW)
        return false;
    u32 syscall = 0;
    u32 hits = 0;
    if (!axsc_rule_stats(filter, 2, &syscall, &hits))
        return false;
    if (syscall != 2)
        return false;
    if (!axsc_attach(filter))
        return false;
    if (!axsc_detach(filter))
        return false;
    if (!axsc_glob("lib/*.so", "lib/libc.so"))
        return false;
    if (axsc_glob("lib/*.so", "lib/libc.o"))
        return false;
    if (!axsc_glob("a?c", "abc"))
        return false;
    if (!axsc_glob("**", "anything"))
        return false;
    return true;
}
