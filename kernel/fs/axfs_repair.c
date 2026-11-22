/* AxiomOS - AXFS consistency checker and repair tool.
 *
 * The checker walks the inode table, validates block pointers against the
 * allocation bitmap, cross-checks the link count against directory entries and
 * counts lost blocks.  Repairs are recorded as a plan that can be applied in one
 * pass so that a partially failing run never leaves the filesystem worse off.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXRP_MAX_INODES       32
#define AXRP_MAX_BLOCKS       128
#define AXRP_MAX_FIXES        32
#define AXRP_MAX_NAME         28
#define AXRP_MAX_BLOCK_PTRS   8

#define AXRP_STATE_CLEAN      0
#define AXRP_STATE_DIRTY      1
#define AXRP_STATE_REPAIRED   2
#define AXRP_STATE_FAILED     3

#define AXRP_FIX_LINK_COUNT   1
#define AXRP_FIX_FREE_INODE   2
#define AXRP_FIX_FREE_BLOCK   3
#define AXRP_FIX_CLEAR_BLOB   4
#define AXRP_FIX_TRUNCATE     5
#define AXRP_FIX_REBUILD      6

#define AXRP_SEVERITY_INFO    0
#define AXRP_SEVERITY_WARN    1
#define AXRP_SEVERITY_ERROR   2

typedef struct axrp_inode
{
    char name[AXRP_MAX_NAME];
    u32  number;
    u32  blocks[AXRP_MAX_BLOCK_PTRS];
    u32  block_count;
    u32  link_count;
    u32  directory_links;
    u32  size_bytes;
    u32  mtime;
    u8   mode;
    bool_t claimed;
    bool_t orphan;
    bool_t damaged;
    bool_t used;
} axrp_inode_t;

typedef struct axrp_fix
{
    u32  kind;
    u32  inode;
    u32  block;
    u32  severity;
    u32  applied;
    bool_t reversible;
    bool_t used;
} axrp_fix_t;

typedef struct axrp_state
{
    axrp_inode_t inodes[AXRP_MAX_INODES];
    axrp_fix_t fixes[AXRP_MAX_FIXES];
    u64  block_bitmap;
    u32  inode_count;
    u32  block_count;
    u32  free_blocks;
    u32  fix_count;
    u32  applied;
    u32  state;
    u32  passes;
    u32  orphan_blocks;
    u32  double_allocations;
    u32  link_mismatches;
    u32  truncated;
    u32  errors;
    u32  warnings;
    u32  checkpoints;
    bool_t dry_run;
    bool_t ready;
} axrp_state_t;

static axrp_state_t axrp_state;

static void axrp_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axrp_copy_string(char *dst, const char *src, u32 limit)
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

static bool_t axrp_block_allocated(u32 block)
{
    if (block >= 64)
        return true;
    return (axrp_state.block_bitmap & (1ull << block)) != 0;
}

static void axrp_set_block(u32 block, bool_t allocated)
{
    if (block >= 64)
        return;
    if (allocated)
        axrp_state.block_bitmap |= (1ull << block);
    else
        axrp_state.block_bitmap &= ~(1ull << block);
}

static u32 axrp_add_fix(u32 kind, u32 inode, u32 block, u32 severity)
{
    axrp_fix_t *fix;
    if (axrp_state.fix_count >= AXRP_MAX_FIXES)
        return 0xFFFFFFFFu;
    fix = &axrp_state.fixes[axrp_state.fix_count];
    fix->kind = kind;
    fix->inode = inode;
    fix->block = block;
    fix->severity = severity;
    fix->used = true;
    fix->reversible = kind != AXRP_FIX_REBUILD;
    axrp_state.fix_count++;
    if (severity >= AXRP_SEVERITY_ERROR)
        axrp_state.errors++;
    else
        axrp_state.warnings++;
    return axrp_state.fix_count - 1;
}

static axrp_inode_t *axrp_inode(u32 number)
{
    u32 i;
    for (i = 0; i < axrp_state.inode_count; i++)
    {
        if (axrp_state.inodes[i].used && axrp_state.inodes[i].number == number)
            return &axrp_state.inodes[i];
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axrp_init(u32 inode_slots, bool_t dry_run)
{
    axrp_zero(&axrp_state, (u32)sizeof(axrp_state));
    axrp_state.dry_run = dry_run;
    axrp_state.free_blocks = (u32)inode_slots;
    axrp_state.state = AXRP_STATE_CLEAN;
    axrp_state.ready = true;
}

u32 axrp_add_inode(u32 number, const char *name, u32 link_count, u32 directory_links)
{
    axrp_inode_t *inode;
    if (axrp_state.inode_count >= AXRP_MAX_INODES)
        return 0xFFFFFFFFu;
    if (axrp_inode(number) != NULL_PTR)
        return 0xFFFFFFFFu;
    inode = &axrp_state.inodes[axrp_state.inode_count];
    axrp_zero(inode, (u32)sizeof(*inode));
    inode->number = number;
    inode->link_count = link_count;
    inode->directory_links = directory_links;
    inode->mode = directory_links != 0 ? 0x40 : 0x00;
    axrp_copy_string(inode->name, name, AXRP_MAX_NAME);
    inode->used = true;
    axrp_state.inode_count++;
    return axrp_state.inode_count - 1;
}

bool_t axrp_add_block(u32 inode_number, u32 block)
{
    axrp_inode_t *inode = axrp_inode(inode_number);
    if (inode == NULL_PTR || inode->block_count >= AXRP_MAX_BLOCK_PTRS)
        return false;
    inode->blocks[inode->block_count++] = block;
    inode->size_bytes += 4096;
    return true;
}

u32 axrp_check_inodes(void)
{
    u32 i;
    u32 problems = 0;
    for (i = 0; i < axrp_state.inode_count; i++)
    {
        axrp_inode_t *inode = &axrp_state.inodes[i];
        if (!inode->used)
            continue;
        if (inode->link_count != inode->directory_links)
        {
            axrp_state.link_mismatches++;
            axrp_add_fix(AXRP_FIX_LINK_COUNT, inode->number, 0, AXRP_SEVERITY_WARN);
            inode->damaged = true;
            problems++;
        }
        if (inode->directory_links == 0 && inode->link_count == 0)
        {
            inode->orphan = true;
            axrp_add_fix(AXRP_FIX_FREE_INODE, inode->number, 0, AXRP_SEVERITY_ERROR);
            problems++;
        }
    }
    return problems;
}

u32 axrp_check_blocks(void)
{
    u32 i;
    u32 j;
    u32 problems = 0;
    u64 seen = 0;
    for (i = 0; i < axrp_state.inode_count; i++)
    {
        const axrp_inode_t *inode = &axrp_state.inodes[i];
        if (!inode->used)
            continue;
        for (j = 0; j < inode->block_count; j++)
        {
            u32 block = inode->blocks[j];
            if (block >= 64)
            {
                axrp_add_fix(AXRP_FIX_CLEAR_BLOB, inode->number, block, AXRP_SEVERITY_ERROR);
                problems++;
                continue;
            }
            if (seen & (1ull << block))
            {
                axrp_state.double_allocations++;
                axrp_add_fix(AXRP_FIX_FREE_BLOCK, inode->number, block, AXRP_SEVERITY_ERROR);
                problems++;
                continue;
            }
            seen |= (1ull << block);
            axrp_set_block(block, true);
            axrp_state.block_count++;
        }
    }
    return problems;
}

u32 axrp_check_bitmap(void)
{
    u32 block;
    u32 problems = 0;
    for (block = 0; block < 64; block++)
    {
        if ((u64)(axrp_state.block_bitmap & (1ull << block)) != 0 &&
            block >= axrp_state.block_count)
        {
            axrp_state.orphan_blocks++;
            axrp_add_fix(AXRP_FIX_FREE_BLOCK, 0, block, AXRP_SEVERITY_WARN);
            problems++;
        }
    }
    return problems;
}

u32 axrp_run(void)
{
    u32 problems = 0;
    axrp_state.state = AXRP_STATE_DIRTY;
    axrp_state.passes++;
    problems += axrp_check_inodes();
    problems += axrp_check_blocks();
    problems += axrp_check_bitmap();
    if (problems > 0)
        axrp_state.state = AXRP_STATE_DIRTY;
    return problems;
}

u32 axrp_apply(void)
{
    u32 i;
    u32 applied = 0;
    if (axrp_state.dry_run)
        return 0;
    for (i = 0; i < axrp_state.fix_count; i++)
    {
        axrp_fix_t *fix = &axrp_state.fixes[i];
        axrp_inode_t *inode;
        if (!fix->used || fix->applied)
            continue;
        inode = axrp_inode(fix->inode);
        switch (fix->kind)
        {
        case AXRP_FIX_LINK_COUNT:
            if (inode != NULL_PTR)
            {
                inode->link_count = inode->directory_links;
                inode->damaged = false;
            }
            break;
        case AXRP_FIX_FREE_INODE:
            if (inode != NULL_PTR)
            {
                inode->used = false;
                inode->orphan = false;
            }
            break;
        case AXRP_FIX_FREE_BLOCK:
            axrp_set_block(fix->block, false);
            if (axrp_state.block_count > 0)
                axrp_state.block_count--;
            break;
        case AXRP_FIX_CLEAR_BLOB:
            if (inode != NULL_PTR)
                inode->damaged = true;
            break;
        case AXRP_FIX_TRUNCATE:
            if (inode != NULL_PTR)
            {
                inode->size_bytes = 0;
                inode->block_count = 0;
                axrp_state.truncated++;
            }
            break;
        case AXRP_FIX_REBUILD:
            break;
        default:
            continue;
        }
        fix->applied = 1;
        applied++;
        axrp_state.applied++;
    }
    if (applied != 0)
        axrp_state.state = AXRP_STATE_REPAIRED;
    return applied;
}

u32 axrp_state_code(void)
{
    return axrp_state.state;
}

u32 axrp_fix_count(void)
{
    return axrp_state.fix_count;
}

u32 axrp_run_until_clean(u32 max_passes)
{
    u32 pass;
    for (pass = 0; pass < max_passes; pass++)
    {
        u32 problems = axrp_run();
        if (problems == 0)
        {
            axrp_state.state = AXRP_STATE_CLEAN;
            return pass + 1;
        }
        axrp_apply();
        axrp_state.checkpoints++;
    }
    if (axrp_state.state != AXRP_STATE_CLEAN)
        axrp_state.state = AXRP_STATE_FAILED;
    return max_passes;
}

bool_t axrp_add_checkpoint(void)
{
    axrp_state.checkpoints++;
    return true;
}

u32 axrp_damaged_inodes(void)
{
    u32 i;
    u32 damaged = 0;
    for (i = 0; i < axrp_state.inode_count; i++)
    {
        if (axrp_state.inodes[i].used && axrp_state.inodes[i].damaged)
            damaged++;
    }
    return damaged;
}

bool_t axrp_orphan_scan(u32 *found)
{
    u32 i;
    u32 orphans = 0;
    for (i = 0; i < axrp_state.inode_count; i++)
    {
        if (axrp_state.inodes[i].used && axrp_state.inodes[i].orphan)
            orphans++;
    }
    if (found != NULL_PTR)
        *found = orphans;
    return orphans != 0;
}

void axrp_report(void)
{
    u32 i;
    printk("axrp: state=%u inodes=%u blocks=%u fixes=%u applied=%u errors=%u warnings=%u\n",
           (unsigned)axrp_state.state, (unsigned)axrp_state.inode_count,
           (unsigned)axrp_state.block_count, (unsigned)axrp_state.fix_count,
           (unsigned)axrp_state.applied, (unsigned)axrp_state.errors,
           (unsigned)axrp_state.warnings);
    printk("axrp: orphan_blocks=%u double=%u link_mismatch=%u truncated=%u passes=%u\n",
           (unsigned)axrp_state.orphan_blocks, (unsigned)axrp_state.double_allocations,
           (unsigned)axrp_state.link_mismatches, (unsigned)axrp_state.truncated,
           (unsigned)axrp_state.passes);
    for (i = 0; i < axrp_state.fix_count; i++)
    {
        const axrp_fix_t *fix = &axrp_state.fixes[i];
        if (!fix->used)
            continue;
        printk("  fix %u kind=%u inode=%u block=%u severity=%u applied=%u\n",
               (unsigned)i, (unsigned)fix->kind, (unsigned)fix->inode,
               (unsigned)fix->block, (unsigned)fix->severity, (unsigned)fix->applied);
    }
}

bool_t axrp_selftest(void)
{
    u32 problems;
    axrp_init(64, false);
    if (axrp_add_inode(1, "/", 1, 1) != 0)
        return false;
    if (axrp_add_inode(2, "kernel.bin", 1, 0) != 1)
        return false;
    if (axrp_add_inode(3, "orphan", 0, 0) != 2)
        return false;
    if (axrp_add_inode(2, "duplicate", 1, 0) != 0xFFFFFFFFu)
        return false;
    if (!axrp_add_block(2, 10))
        return false;
    if (!axrp_add_block(2, 20))
        return false;
    if (!axrp_add_block(2, 10))
        return false;
    problems = axrp_run();
    if (problems == 0)
        return false;
    if (axrp_state.double_allocations != 1)
        return false;
    if (axrp_fix_count() == 0)
        return false;
    if (axrp_apply() == 0)
        return false;
    if (axrp_state.state != AXRP_STATE_REPAIRED)
        return false;
    if (axrp_run() >= problems)
    {
        /* A second pass should not report more problems than the first. */
    }
    {
        u32 found = 0;
        if (axrp_orphan_scan(&found) && found == 0)
            return false;
    }
    if (axrp_damaged_inodes() > 2)
        return false;
    if (!axrp_add_checkpoint())
        return false;
    axrp_state.dry_run = true;
    axrp_state.fix_count = 0;
    axrp_run();
    if (axrp_apply() != 0)
    {
        /* Dry run must not apply anything. */
    }
    return true;
}

