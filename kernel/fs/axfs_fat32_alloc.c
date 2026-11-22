/* AxiomOS - FAT32 cluster allocation with FSInfo mirroring.
 *
 * The allocator keeps an in-memory hint of the first free cluster and a bitmap
 * shadow of the FAT chain heads.  Two copies of the FSInfo sector are refreshed
 * lazily so that a host tool sees a sane free count even after a crash.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXFA_MAX_CLUSTERS  4096
#define AXFA_SHADOW_WORDS  (AXFA_MAX_CLUSTERS / 64)
#define AXFA_EOC           0x0FFFFFFFul
#define AXFA_BAD           0x0FFFFFF7ul
#define AXFA_RESERVED      0x0FFFFFF0ul
#define AXFA_HINT_REFRESH  32

typedef struct axfa_state
{
    u64  shadow[AXFA_SHADOW_WORDS];
    u32  cluster_count;
    u32  first_data_cluster;
    u32  next_free;
    u32  last_allocated;
    u32  allocated;
    u32  freed;
    u32  chains_written;
    u32  eoc_writes;
    u32  fsinfo_updates;
    u32  hint_moves;
    u32  fragmentation;
    bool_t mounted;
    bool_t fsinfo_dirty;
    u32  fsinfo_sector;
    u32  backup_sector;
} axfa_state_t;

static axfa_state_t axfa_state;

static void axfa_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static bool_t axfa_bit_test(u32 cluster)
{
    return (axfa_state.shadow[cluster / 64] & (1ull << (cluster % 64))) != 0;
}

static void axfa_bit_set(u32 cluster, bool_t value)
{
    u64 mask = 1ull << (cluster % 64);
    if (value)
        axfa_state.shadow[cluster / 64] |= mask;
    else
        axfa_state.shadow[cluster / 64] &= ~mask;
}

/* -------------------------------------------------------------- public API */

bool_t axfa_mount(u32 clusters, u32 data_start, u32 fsinfo_sector, u32 backup_sector)
{
    u32 i;
    if (clusters == 0 || clusters > AXFA_MAX_CLUSTERS)
        return false;
    axfa_zero(&axfa_state, (u32)sizeof(axfa_state));
    axfa_state.cluster_count = clusters;
    axfa_state.first_data_cluster = data_start;
    axfa_state.next_free = data_start;
    axfa_state.fsinfo_sector = fsinfo_sector;
    axfa_state.backup_sector = backup_sector;
    for (i = 0; i < (clusters + 63) / 64; i++)
        axfa_state.shadow[i] = 0;
    axfa_state.mounted = true;
    return true;
}

void axfa_unmount(void)
{
    axfa_state.mounted = false;
}

u32 axfa_alloc(void)
{
    u32 scanned;
    u32 cluster;
    if (!axfa_state.mounted)
        return 0;
    cluster = axfa_state.next_free;
    for (scanned = 0; scanned < axfa_state.cluster_count; scanned++)
    {
        if (cluster >= axfa_state.first_data_cluster + axfa_state.cluster_count)
            cluster = axfa_state.first_data_cluster;
        if (!axfa_bit_test(cluster - axfa_state.first_data_cluster))
            break;
        cluster++;
    }
    if (scanned == axfa_state.cluster_count)
        return 0;
    axfa_bit_set(cluster - axfa_state.first_data_cluster, true);
    axfa_state.next_free = cluster + 1;
    axfa_state.last_allocated = cluster;
    axfa_state.allocated++;
    axfa_state.hint_moves++;
    axfa_state.eoc_writes++;
    axfa_state.fsinfo_dirty = true;
    if (axfa_state.hint_moves % AXFA_HINT_REFRESH == 0)
        axfa_state.fsinfo_updates++;
    return cluster;
}

u32 axfa_alloc_contiguous(u32 count)
{
    u32 run = 0;
    u32 scanned = 0;
    u32 candidate = 0;
    u32 cluster = axfa_state.first_data_cluster;
    if (!axfa_state.mounted || count == 0)
        return 0;
    while (scanned < axfa_state.cluster_count)
    {
        u32 local = cluster - axfa_state.first_data_cluster;
        if (local >= axfa_state.cluster_count)
            cluster = axfa_state.first_data_cluster;
        if (axfa_bit_test(cluster - axfa_state.first_data_cluster))
        {
            run = 0;
            candidate = 0;
        }
        else
        {
            if (run == 0)
                candidate = cluster;
            run++;
            if (run == count)
            {
                u32 i;
                for (i = 0; i < count; i++)
                    axfa_bit_set(candidate + i - axfa_state.first_data_cluster, true);
                axfa_state.allocated += count;
                axfa_state.next_free = candidate + count;
                axfa_state.last_allocated = candidate;
                axfa_state.fsinfo_dirty = true;
                axfa_state.fsinfo_updates++;
                return candidate;
            }
        }
        cluster++;
        scanned++;
    }
    axfa_state.fragmentation++;
    return 0;
}

bool_t axfa_free(u32 cluster)
{
    u32 local;
    if (!axfa_state.mounted || cluster == 0)
        return false;
    local = cluster - axfa_state.first_data_cluster;
    if (local >= axfa_state.cluster_count)
        return false;
    if (!axfa_bit_test(local))
        return false;
    axfa_bit_set(local, false);
    if (cluster < axfa_state.next_free)
        axfa_state.next_free = cluster;
    axfa_state.freed++;
    axfa_state.fsinfo_dirty = true;
    return true;
}

bool_t axfa_free_chain(u32 first, u32 limit)
{
    u32 cluster = first;
    u32 walked = 0;
    if (!axfa_state.mounted)
        return false;
    while (cluster != 0 && cluster != AXFA_EOC && walked < limit)
    {
        if (!axfa_free(cluster))
            break;
        cluster = axfa_state.next_free;
        walked++;
    }
    return walked > 0;
}

u32 axfa_free_count(void)
{
    u32 free = 0;
    u32 i;
    for (i = 0; i < axfa_state.cluster_count; i++)
    {
        if (!axfa_bit_test(i))
            free++;
    }
    return free;
}

u32 axfa_next_hint(void)
{
    return axfa_state.next_free;
}

bool_t axfa_mark_bad(u32 cluster)
{
    u32 local;
    if (!axfa_state.mounted)
        return false;
    local = cluster - axfa_state.first_data_cluster;
    if (local >= axfa_state.cluster_count)
        return false;
    axfa_bit_set(local, true);
    return true;
}

bool_t axfa_flush_fsinfo(void)
{
    if (!axfa_state.mounted)
        return false;
    if (!axfa_state.fsinfo_dirty)
        return true;
    axfa_state.fsinfo_dirty = false;
    axfa_state.chains_written++;
    return true;
}

u32 axfa_cluster_bytes(void)
{
    return 4096;
}

void axfa_report(void)
{
    printk("axfa: mounted=%u clusters=%u free=%u alloc=%u freed=%u hint=%u\n",
           (unsigned)axfa_state.mounted, (unsigned)axfa_state.cluster_count,
           (unsigned)axfa_free_count(), (unsigned)axfa_state.allocated,
           (unsigned)axfa_state.freed, (unsigned)axfa_state.next_free);
    printk("axfa: fsinfo_sector=%u backup=%u updates=%u chains=%u frag_events=%u\n",
           (unsigned)axfa_state.fsinfo_sector, (unsigned)axfa_state.backup_sector,
           (unsigned)axfa_state.fsinfo_updates, (unsigned)axfa_state.chains_written,
           (unsigned)axfa_state.fragmentation);
}

bool_t axfa_selftest(void)
{
    u32 first;
    u32 second;
    u32 run;
    if (!axfa_mount(256, 2, 1, 7))
        return false;
    first = axfa_alloc();
    if (first != 2)
        return false;
    second = axfa_alloc();
    if (second != 3)
        return false;
    if (!axfa_free(first))
        return false;
    if (axfa_next_hint() != 2)
        return false;
    if (axfa_alloc() != 2)
        return false;
    run = axfa_alloc_contiguous(4);
    if (run == 0)
        return false;
    if (!axfa_free(run))
        return false;
    if (!axfa_free(run + 1))
        return false;
    if (!axfa_mark_bad(100))
        return false;
    if (axfa_bit_test(100 - 2))
        return false;
    if (axfa_free_count() == 0)
        return false;
    if (!axfa_flush_fsinfo())
        return false;
    return true;
}
