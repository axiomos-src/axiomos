/* AxiomOS - filesystem quota accounting.
 *
 * Per-user limits on block usage and inode counts.  The module is deliberately
 * storage agnostic: the filesystem calls axqu_charge() before allocating and
 * axqu_release() when space is freed.  Soft limits produce a warning once, hard
 * limits are refused.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXQU_MAX_USERS     32
#define AXQU_MAX_GROUPS    16
#define AXQU_NAME_LEN      16
#define AXQU_WARN_TICKS    8

typedef struct axqu_limit
{
    u32 id;
    u64 soft_bytes;
    u64 hard_bytes;
    u64 soft_inodes;
    u64 hard_inodes;
    bool_t used;
    char   name[AXQU_NAME_LEN];
} axqu_limit_t;

typedef struct axqu_usage
{
    u32 id;
    u64 bytes;
    u64 inodes;
    u64 peak_bytes;
    u32 refusals;
    u32 warnings;
    u32 last_warn_tick;
    bool_t warned;
} axqu_usage_t;

typedef struct axqu_quota
{
    axqu_limit_t user_limits[AXQU_MAX_USERS];
    axqu_usage_t user_usage[AXQU_MAX_USERS];
    u32  user_count;
    u64  total_bytes;
    u64  total_inodes;
    u64  global_hard_bytes;
    u64  global_soft_bytes;
    u32  global_refusals;
    bool_t enforce;
    bool_t ready;
} axqu_quota_t;

static axqu_quota_t axqu_quota;

static void axqu_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axqu_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static axqu_usage_t *axqu_find_usage(u32 id)
{
    u32 i;
    for (i = 0; i < axqu_quota.user_count; i++)
    {
        if (axqu_quota.user_usage[i].id == id)
            return &axqu_quota.user_usage[i];
    }
    return NULL_PTR;
}

static axqu_limit_t *axqu_find_limit(u32 id)
{
    u32 i;
    for (i = 0; i < axqu_quota.user_count; i++)
    {
        if (axqu_quota.user_limits[i].id == id)
            return &axqu_quota.user_limits[i];
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axqu_init(void)
{
    axqu_zero(&axqu_quota, (u32)sizeof(axqu_quota));
    axqu_quota.enforce = true;
    axqu_quota.ready = true;
}

void axqu_global(u64 soft_bytes, u64 hard_bytes)
{
    axqu_quota.global_soft_bytes = soft_bytes;
    axqu_quota.global_hard_bytes = hard_bytes;
}

bool_t axqu_attach(u32 id, const char *name, u64 soft_bytes, u64 hard_bytes,
                   u64 soft_inodes, u64 hard_inodes)
{
    axqu_limit_t *limit;
    axqu_usage_t *usage;
    if (axqu_quota.user_count >= AXQU_MAX_USERS)
        return false;
    if (axqu_find_usage(id) != NULL_PTR)
        return false;
    limit = &axqu_quota.user_limits[axqu_quota.user_count];
    usage = &axqu_quota.user_usage[axqu_quota.user_count];
    axqu_zero(limit, (u32)sizeof(*limit));
    axqu_zero(usage, (u32)sizeof(*usage));
    limit->id = id;
    limit->soft_bytes = soft_bytes;
    limit->hard_bytes = hard_bytes;
    limit->soft_inodes = soft_inodes;
    limit->hard_inodes = hard_inodes;
    limit->used = true;
    axqu_copy(limit->name, AXQU_NAME_LEN, name);
    usage->id = id;
    axqu_quota.user_count++;
    return true;
}

void axqu_set_enforce(bool_t enforce)
{
    axqu_quota.enforce = enforce;
}

bool_t axqu_charge(u32 id, u64 bytes, u64 inodes)
{
    axqu_usage_t *usage = axqu_find_usage(id);
    axqu_limit_t *limit = axqu_find_limit(id);
    if (!axqu_quota.ready)
        return true;
    if (usage == NULL_PTR || limit == NULL_PTR)
        return !axqu_quota.enforce;
    if (axqu_quota.enforce)
    {
        if (limit->hard_bytes != 0 &&
            usage->bytes + bytes > limit->hard_bytes)
        {
            usage->refusals++;
            axqu_quota.global_refusals++;
            return false;
        }
        if (limit->hard_inodes != 0 &&
            usage->inodes + inodes > limit->hard_inodes)
        {
            usage->refusals++;
            axqu_quota.global_refusals++;
            return false;
        }
        if (axqu_quota.global_hard_bytes != 0 &&
            axqu_quota.total_bytes + bytes > axqu_quota.global_hard_bytes)
        {
            axqu_quota.global_refusals++;
            return false;
        }
    }
    usage->bytes += bytes;
    usage->inodes += inodes;
    axqu_quota.total_bytes += bytes;
    axqu_quota.total_inodes += inodes;
    if (usage->bytes > usage->peak_bytes)
        usage->peak_bytes = usage->bytes;
    if (limit->soft_bytes != 0 && usage->bytes > limit->soft_bytes)
    {
        if (!usage->warned && usage->last_warn_tick + AXQU_WARN_TICKS > usage->warned)
        {
            usage->warned = true;
            usage->warnings++;
        }
    }
    return true;
}

void axqu_release(u32 id, u64 bytes, u64 inodes)
{
    axqu_usage_t *usage = axqu_find_usage(id);
    if (usage == NULL_PTR)
        return;
    usage->bytes = usage->bytes > bytes ? usage->bytes - bytes : 0;
    usage->inodes = usage->inodes > inodes ? usage->inodes - inodes : 0;
    axqu_quota.total_bytes = axqu_quota.total_bytes > bytes ? axqu_quota.total_bytes - bytes : 0;
    axqu_quota.total_inodes = axqu_quota.total_inodes > inodes ? axqu_quota.total_inodes - inodes : 0;
    if (usage->bytes <= 0)
        usage->warned = false;
}

u64 axqu_bytes(u32 id)
{
    axqu_usage_t *usage = axqu_find_usage(id);
    return usage == NULL_PTR ? 0 : usage->bytes;
}

u64 axqu_inodes(u32 id)
{
    axqu_usage_t *usage = axqu_find_usage(id);
    return usage == NULL_PTR ? 0 : usage->inodes;
}

u32 axqu_percent(u32 id)
{
    axqu_usage_t *usage = axqu_find_usage(id);
    axqu_limit_t *limit = axqu_find_limit(id);
    if (usage == NULL_PTR || limit == NULL_PTR || limit->hard_bytes == 0)
        return 0;
    {
        u64 ratio = usage->bytes * 100 / limit->hard_bytes;
        return ratio > 100 ? 100 : (u32)ratio;
    }
}

bool_t axqu_attached(u32 id)
{
    return axqu_find_usage(id) != NULL_PTR;
}

u32 axqu_count(void)
{
    return axqu_quota.user_count;
}

void axqu_report(void)
{
    u32 i;
    printk("axqu: users=%u total=%llu bytes/%llu inodes refusals=%u%s\n",
           (unsigned)axqu_quota.user_count,
           (unsigned long long)axqu_quota.total_bytes,
           (unsigned long long)axqu_quota.total_inodes,
           (unsigned)axqu_quota.global_refusals,
           axqu_quota.enforce ? " enforcing" : " audit");
    for (i = 0; i < axqu_quota.user_count; i++)
    {
        printk("  %s(%u): %llu B inodes=%llu warn=%u refuse=%u\n",
               axqu_quota.user_limits[i].name,
               (unsigned)axqu_quota.user_limits[i].id,
               (unsigned long long)axqu_quota.user_usage[i].bytes,
               (unsigned long long)axqu_quota.user_usage[i].inodes,
               (unsigned)axqu_quota.user_usage[i].warnings,
               (unsigned)axqu_quota.user_usage[i].refusals);
    }
}

bool_t axqu_selftest(void)
{
    axqu_init();
    if (!axqu_attach(1000, "root", 0, 0, 0, 0))
        return false;
    if (!axqu_attach(1001, "user", 1024, 2048, 4, 8))
        return false;
    if (axqu_count() != 2)
        return false;
    if (!axqu_charge(1000, 1 << 20, 100))
        return false;
    if (!axqu_charge(1001, 1000, 2))
        return false;
    if (!axqu_charge(1001, 700, 1))
        return false;
    if (axqu_bytes(1001) != 1700)
        return false;
    if (axqu_percent(1001) != 83)
        return false;
    axqu_release(1001, 1700, 3);
    if (axqu_bytes(1001) != 0)
        return false;
    if (!axqu_charge(1001, 2048, 4))
        return false;
    if (axqu_charge(1001, 1, 1))
        return false;
    if (axqu_inodes(1001) != 4)
        return false;
    axqu_set_enforce(false);
    if (!axqu_charge(1001, 9999, 99))
        return false;
    return true;
}
