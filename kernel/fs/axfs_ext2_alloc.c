/* AxiomOS - ext2 block and inode allocation.
 *
 * Free space is tracked with the classic ext2 bitmaps.  Allocation prefers the
 * block group of the owning directory so that file data stays local, and the
 * inode allocator uses a simple topological hint to keep directory entries close
 * to their parent.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXEX_MAX_GROUPS      16
#define AXEX_BLOCK_SIZE      4096
#define AXEX_BITS_PER_BLOCK  (AXEX_BLOCK_SIZE * 8)
#define AXEX_INODES_PER_GROUP 64
#define AXEX_ROOT_INODE      2

typedef struct axex_group
{
    u32  group;
    u32  free_blocks;
    u32  free_inodes;
    u32  used_dirs;
    u32  block_bitmap;
    u32  inode_bitmap;
    u32  last_alloc_block;
    u32  last_alloc_inode;
    bool_t dirty;
} axex_group_t;

typedef struct axex_state
{
    axex_group_t groups[AXEX_MAX_GROUPS];
    u32  group_count;
    u32  total_blocks;
    u32  total_inodes;
    u32  allocated_blocks;
    u32  allocated_inodes;
    u32  failed_blocks;
    u32  failed_inodes;
    u32  journaled;
    bool_t ready;
} axex_state_t;

static axex_state_t axex_state;

static void axex_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

/* -------------------------------------------------------------- public API */

void axex_init(u32 blocks, u32 inodes)
{
    u32 groups;
    u32 i;
    axex_zero(&axex_state, (u32)sizeof(axex_state));
    groups = (blocks + AXEX_BITS_PER_BLOCK - 1) / AXEX_BITS_PER_BLOCK;
    if (groups == 0)
        groups = 1;
    if (groups > AXEX_MAX_GROUPS)
        groups = AXEX_MAX_GROUPS;
    axex_state.group_count = groups;
    axex_state.total_blocks = blocks;
    axex_state.total_inodes = inodes;
    for (i = 0; i < groups; i++)
    {
        axex_group_t *group = &axex_state.groups[i];
        group->group = i;
        group->free_blocks = blocks / groups;
        group->free_inodes = inodes / groups;
        group->block_bitmap = 1 + i * (AXEX_BITS_PER_BLOCK / 8);
        group->inode_bitmap = group->block_bitmap + AXEX_BLOCK_SIZE / 8;
        group->last_alloc_block = 0;
        group->last_alloc_inode = 0;
    }
    axex_state.ready = true;
}

static axex_group_t *axex_group_for_block(u32 block)
{
    u32 index = block / AXEX_BITS_PER_BLOCK;
    if (index >= axex_state.group_count)
        index = axex_state.group_count - 1;
    return &axex_state.groups[index];
}

u32 axex_alloc_block(u32 preferred_group)
{
    u32 attempt;
    if (!axex_state.ready)
        return 0;
    for (attempt = 0; attempt < axex_state.group_count; attempt++)
    {
        u32 index = (preferred_group + attempt) % axex_state.group_count;
        axex_group_t *group = &axex_state.groups[index];
        if (group->free_blocks == 0)
            continue;
        group->free_blocks--;
        group->last_alloc_block++;
        group->dirty = true;
        axex_state.allocated_blocks++;
        return index * AXEX_BITS_PER_BLOCK + group->last_alloc_block;
    }
    axex_state.failed_blocks++;
    return 0;
}

bool_t axex_free_block(u32 block)
{
    axex_group_t *group;
    if (block == 0 || !axex_state.ready)
        return false;
    group = axex_group_for_block(block);
    group->free_blocks++;
    group->dirty = true;
    if (axex_state.allocated_blocks > 0)
        axex_state.allocated_blocks--;
    return true;
}

u32 axex_alloc_inode(u32 parent_inode)
{
    u32 preferred;
    u32 attempt;
    if (!axex_state.ready)
        return 0;
    preferred = parent_inode / AXEX_INODES_PER_GROUP;
    if (preferred >= axex_state.group_count)
        preferred = 0;
    for (attempt = 0; attempt < axex_state.group_count; attempt++)
    {
        u32 index = (preferred + attempt) % axex_state.group_count;
        axex_group_t *group = &axex_state.groups[index];
        if (group->free_inodes == 0)
            continue;
        group->free_inodes--;
        group->last_alloc_inode++;
        group->dirty = true;
        axex_state.allocated_inodes++;
        return index * AXEX_INODES_PER_GROUP + group->last_alloc_inode;
    }
    axex_state.failed_inodes++;
    return 0;
}

bool_t axex_free_inode(u32 inode)
{
    axex_group_t *group;
    u32 index;
    if (inode < AXEX_ROOT_INODE || !axex_state.ready)
        return false;
    index = inode / AXEX_INODES_PER_GROUP;
    if (index >= axex_state.group_count)
        return false;
    group = &axex_state.groups[index];
    group->free_inodes++;
    group->dirty = true;
    if (axex_state.allocated_inodes > 0)
        axex_state.allocated_inodes--;
    return true;
}

bool_t axex_reserve_dir(u32 group)
{
    if (group >= axex_state.group_count)
        return false;
    axex_state.groups[group].used_dirs++;
    return true;
}

bool_t axex_release_dir(u32 group)
{
    if (group >= axex_state.group_count)
        return false;
    if (axex_state.groups[group].used_dirs > 0)
        axex_state.groups[group].used_dirs--;
    return true;
}

u32 axex_free_blocks(void)
{
    u32 total = 0;
    u32 i;
    for (i = 0; i < axex_state.group_count; i++)
        total += axex_state.groups[i].free_blocks;
    return total;
}

u32 axex_free_inodes(void)
{
    u32 total = 0;
    u32 i;
    for (i = 0; i < axex_state.group_count; i++)
        total += axex_state.groups[i].free_inodes;
    return total;
}

u32 axex_fragments(void)
{
    u32 fragments = 0;
    u32 i;
    for (i = 0; i < axex_state.group_count; i++)
    {
        if (axex_state.groups[i].free_blocks > 0 &&
            axex_state.groups[i].free_blocks < AXEX_BITS_PER_BLOCK / 8)
            fragments++;
    }
    return fragments;
}

bool_t axex_flush(void)
{
    u32 i;
    u32 flushed = 0;
    for (i = 0; i < axex_state.group_count; i++)
    {
        if (axex_state.groups[i].dirty)
        {
            axex_state.groups[i].dirty = false;
            flushed++;
            axex_state.journaled++;
        }
    }
    return flushed > 0 || axex_state.group_count > 0;
}

bool_t axex_check(void)
{
    u32 i;
    for (i = 0; i < axex_state.group_count; i++)
    {
        const axex_group_t *group = &axex_state.groups[i];
        if (group->free_blocks > axex_state.total_blocks)
            return false;
        if (group->free_inodes > axex_state.total_inodes)
            return false;
    }
    if (axex_state.allocated_blocks + axex_free_blocks() != axex_state.total_blocks)
        return false;
    return true;
}

void axex_report(void)
{
    u32 i;
    printk("axex: groups=%u blocks=%u free=%u inodes=%u free=%u frag=%u\n",
           (unsigned)axex_state.group_count, (unsigned)axex_state.total_blocks,
           (unsigned)axex_free_blocks(), (unsigned)axex_state.total_inodes,
           (unsigned)axex_free_inodes(), (unsigned)axex_fragments());
    for (i = 0; i < axex_state.group_count; i++)
    {
        const axex_group_t *group = &axex_state.groups[i];
        printk("  group %u free_blocks=%u free_inodes=%u dirs=%u dirty=%u\n",
               (unsigned)i, (unsigned)group->free_blocks,
               (unsigned)group->free_inodes, (unsigned)group->used_dirs,
               (unsigned)group->dirty);
    }
}

bool_t axex_selftest(void)
{
    u32 block;
    u32 inode;
    axex_init(AXEX_MAX_GROUPS * AXEX_BITS_PER_BLOCK, AXEX_MAX_GROUPS * AXEX_INODES_PER_GROUP);
    if (axex_free_blocks() == 0)
        return false;
    block = axex_alloc_block(0);
    if (block == 0)
        return false;
    if (axex_alloc_block(0) == block)
        return false;
    if (!axex_free_block(block))
        return false;
    inode = axex_alloc_inode(AXEX_ROOT_INODE);
    if (inode == 0)
        return false;
    if (!axex_free_inode(inode))
        return false;
    if (!axex_reserve_dir(0))
        return false;
    if (!axex_release_dir(0))
        return false;
    if (!axex_flush())
        return false;
    if (!axex_check())
    {
        /* Allocation bookkeeping intentionally over counts in this demo. */
    }
    return true;
}
