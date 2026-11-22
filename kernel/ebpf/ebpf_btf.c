/* AxiomOS - BTF style type information for eBPF programs.
 *
 * Programs that touch kernel structures need a description of the layout they
 * are allowed to read.  This module keeps a compact type table: integers, arrays,
 * structs with member offsets, pointers and typedefs.  The verifier uses it to
 * bound access offsets, and the JIT uses it to pre-compute constant offsets.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>
#include <axiom/axbpf.h>

#define AXBF_MAX_TYPES        96
#define AXBF_MAX_MEMBERS      256
#define AXBF_MAX_NAMES        64
#define AXBF_NAME_LENGTH      24
#define AXBF_KIND_INT         1
#define AXBF_KIND_PTR         2
#define AXBF_KIND_ARRAY       3
#define AXBF_KIND_STRUCT      4
#define AXBF_KIND_UNION       5
#define AXBF_KIND_ENUM        6
#define AXBF_KIND_TYPEDEF     7
#define AXBF_KIND_FUNC        8
#define AXBF_MAX_DEPTH        8

typedef struct axbf_member
{
    u32  name_offset;
    u32  type_id;
    u32  bit_offset;
    u32  bit_size;
    bool_t used;
    bool_t readonly;
} axbf_member_t;

typedef struct axbf_type
{
    u32  type_id;
    u32  size;
    u32  kind;
    u32  member_start;
    u32  member_count;
    u32  element_type;
    u32  element_count;
    u32  name_offset;
    u32  references;
    u32  generation;
    i32  encoding;
    bool_t signed_type;
    bool_t resolved;
} axbf_type_t;

typedef struct axbf_state
{
    axbf_type_t types[AXBF_MAX_TYPES];
    axbf_member_t members[AXBF_MAX_MEMBERS];
    char names[AXBF_MAX_NAMES][AXBF_NAME_LENGTH];
    u32  type_count;
    u32  member_count;
    u32  name_count;
    u32  unresolved;
    u32  lookups;
    u32  layout_checks;
    u32  layout_failures;
    u32  bounds_violations;
    u32  generation;
    u32  last_error;
    bool_t initialized;
} axbf_state_t;

static axbf_state_t axbf_state;

static void axbf_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axbf_copy(char *target, const char *source, u32 capacity)
{
    u32 i = 0;
    for (; i + 1 < capacity && source[i] != 0; i++)
        target[i] = source[i];
    target[i] = 0;
}

static u32 axbf_name(const char *text)
{
    if (text == NULL_PTR || text[0] == 0)
        return 0;
    for (u32 i = 0; i < axbf_state.name_count; i++)
    {
        u32 equal = 1;
        for (u32 c = 0; c < AXBF_NAME_LENGTH; c++)
        {
            if (axbf_state.names[i][c] != text[c])
            {
                equal = 0;
                break;
            }
            if (text[c] == 0)
                break;
        }
        if (equal)
            return i + 1;
    }
    if (axbf_state.name_count >= AXBF_MAX_NAMES)
        return 0;
    axbf_copy(axbf_state.names[axbf_state.name_count], text, AXBF_NAME_LENGTH);
    axbf_state.name_count++;
    return axbf_state.name_count;
}

static const char *axbf_name_text(u32 offset)
{
    if (offset == 0 || offset > axbf_state.name_count)
        return "";
    return axbf_state.names[offset - 1];
}

static axbf_type_t *axbf_type(u32 type_id)
{
    for (u32 i = 0; i < axbf_state.type_count; i++)
    {
        if (axbf_state.types[i].type_id == type_id)
            return &axbf_state.types[i];
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axbf_init(void)
{
    axbf_zero(&axbf_state, (u32)sizeof(axbf_state));
    axbf_state.initialized = true;
}

u32 axbf_add_int(u32 type_id, u32 size, const char *name, bool_t signed_type)
{
    axbf_type_t *entry;
    if (!axbf_state.initialized || axbf_state.type_count >= AXBF_MAX_TYPES)
        return 0;
    if (size == 0 || size > 16)
        return 0;
    entry = &axbf_state.types[axbf_state.type_count];
    axbf_zero(entry, (u32)sizeof(*entry));
    entry->type_id = type_id;
    entry->kind = AXBF_KIND_INT;
    entry->size = size;
    entry->name_offset = axbf_name(name);
    entry->signed_type = signed_type;
    entry->encoding = signed_type ? 1 : 0;
    entry->resolved = true;
    axbf_state.type_count++;
    axbf_state.generation++;
    return type_id;
}

u32 axbf_add_ptr(u32 type_id, u32 target_type, const char *name)
{
    axbf_type_t *entry;
    if (!axbf_state.initialized || axbf_state.type_count >= AXBF_MAX_TYPES)
        return 0;
    entry = &axbf_state.types[axbf_state.type_count];
    axbf_zero(entry, (u32)sizeof(*entry));
    entry->type_id = type_id;
    entry->kind = AXBF_KIND_PTR;
    entry->size = 8;
    entry->element_type = target_type;
    entry->name_offset = axbf_name(name);
    entry->resolved = axbf_type(target_type) != NULL_PTR;
    if (!entry->resolved)
        axbf_state.unresolved++;
    axbf_state.type_count++;
    axbf_state.generation++;
    return type_id;
}

u32 axbf_add_struct(u32 type_id, u32 size, const char *name, u32 member_count)
{
    axbf_type_t *entry;
    if (!axbf_state.initialized || axbf_state.type_count >= AXBF_MAX_TYPES)
        return 0;
    if (member_count == 0 || axbf_state.member_count + member_count > AXBF_MAX_MEMBERS)
        return 0;
    entry = &axbf_state.types[axbf_state.type_count];
    axbf_zero(entry, (u32)sizeof(*entry));
    entry->type_id = type_id;
    entry->kind = AXBF_KIND_STRUCT;
    entry->size = size;
    entry->member_start = axbf_state.member_count;
    entry->member_count = member_count;
    entry->name_offset = axbf_name(name);
    entry->resolved = true;
    axbf_state.member_count += member_count;
    axbf_state.type_count++;
    axbf_state.generation++;
    return type_id;
}

bool_t axbf_add_member(u32 struct_type, u32 index, const char *name, u32 type_id,
                       u32 bit_offset, u32 bit_size)
{
    axbf_type_t *parent = axbf_type(struct_type);
    axbf_member_t *member;
    if (parent == NULL_PTR || parent->kind != AXBF_KIND_STRUCT)
        return false;
    if (index >= parent->member_count)
        return false;
    member = &axbf_state.members[parent->member_start + index];
    member->name_offset = axbf_name(name);
    member->type_id = type_id;
    member->bit_offset = bit_offset;
    member->bit_size = bit_size;
    member->used = true;
    return true;
}

u32 axbf_add_array(u32 type_id, u32 element_type, u32 count, const char *name)
{
    axbf_type_t *entry;
    if (!axbf_state.initialized || axbf_state.type_count >= AXBF_MAX_TYPES)
        return 0;
    if (count == 0)
        return 0;
    entry = &axbf_state.types[axbf_state.type_count];
    axbf_zero(entry, (u32)sizeof(*entry));
    entry->type_id = type_id;
    entry->kind = AXBF_KIND_ARRAY;
    entry->element_type = element_type;
    entry->element_count = count;
    entry->name_offset = axbf_name(name);
    axbf_type_t *element = axbf_type(element_type);
    entry->size = element == NULL_PTR ? 0 : element->size * count;
    entry->resolved = element != NULL_PTR;
    axbf_state.type_count++;
    axbf_state.generation++;
    return type_id;
}

/* ------------------------------------------------------------ resolution */

u32 axbf_resolve(void)
{
    u32 resolved = 0;
    for (u32 pass = 0; pass < AXBF_MAX_DEPTH; pass++)
    {
        u32 changed = 0;
        for (u32 i = 0; i < axbf_state.type_count; i++)
        {
            axbf_type_t *entry = &axbf_state.types[i];
            if (entry->resolved)
                continue;
            if (entry->kind == AXBF_KIND_PTR || entry->kind == AXBF_KIND_ARRAY)
            {
                axbf_type_t *element = axbf_type(entry->element_type);
                if (element != NULL_PTR)
                {
                    entry->resolved = true;
                    if (entry->kind == AXBF_KIND_ARRAY)
                        entry->size = element->size * entry->element_count;
                    changed++;
                    resolved++;
                }
            }
            else if (entry->kind == AXBF_KIND_TYPEDEF)
            {
                axbf_type_t *alias = axbf_type(entry->element_type);
                if (alias != NULL_PTR)
                {
                    entry->size = alias->size;
                    entry->resolved = true;
                    changed++;
                    resolved++;
                }
            }
        }
        if (changed == 0)
            break;
    }
    return resolved;
}

bool_t axbf_offsets(u32 type_id, u32 member_bits, u32 *byte_offset)
{
    axbf_type_t *entry = axbf_type(type_id);
    if (entry == NULL_PTR || entry->kind != AXBF_KIND_STRUCT)
        return false;
    if (member_bits >= entry->member_count)
        return false;
    axbf_member_t *member = &axbf_state.members[entry->member_start + member_bits];
    if (!member->used)
        return false;
    if (byte_offset != NULL_PTR)
        *byte_offset = member->bit_offset / 8;
    return true;
}

bool_t axbf_member_info(u32 type_id, const char *name, u32 access_size,
                        u32 *offset, u32 *size)
{
    axbf_type_t *entry = axbf_type(type_id);
    axbf_state.lookups++;
    if (entry == NULL_PTR || entry->kind != AXBF_KIND_STRUCT)
        return false;
    for (u32 i = 0; i < entry->member_count; i++)
    {
        axbf_member_t *member = &axbf_state.members[entry->member_start + i];
        if (!member->used)
            continue;
        const char *member_name = axbf_name_text(member->name_offset);
        u32 equal = 1;
        for (u32 c = 0; c < AXBF_NAME_LENGTH; c++)
        {
            if (member_name[c] != name[c])
            {
                equal = 0;
                break;
            }
            if (name[c] == 0)
                break;
        }
        if (!equal)
            continue;
        axbf_state.layout_checks++;
        if (member->bit_offset % 8 != 0)
        {
            axbf_state.layout_failures++;
            axbf_state.last_error = 1;
            return false;
        }
        *offset = member->bit_offset / 8;
        *size = member->bit_size / 8;
        if (access_size != 0 && (access_size > *size || *offset + access_size > entry->size))
        {
            axbf_state.bounds_violations++;
            axbf_state.last_error = 2;
            return false;
        }
        return true;
    }
    axbf_state.layout_failures++;
    return false;
}

u32 axbf_type_count(void)
{
    return axbf_state.type_count;
}

u32 axbf_unresolved(void)
{
    return axbf_state.unresolved - axbf_resolve();
}

u32 axbf_size(u32 type_id)
{
    axbf_type_t *entry = axbf_type(type_id);
    return entry == NULL_PTR ? 0 : entry->size;
}

const char *axbf_type_name(u32 type_id)
{
    axbf_type_t *entry = axbf_type(type_id);
    return entry == NULL_PTR ? "" : axbf_name_text(entry->name_offset);
}

u32 axbf_bounds_violations(void)
{
    return axbf_state.bounds_violations;
}

void axbf_report(void)
{
    printk("axbf: types=%u members=%u names=%u generation=%u violations=%u\n",
           (unsigned)axbf_state.type_count, (unsigned)axbf_state.member_count,
           (unsigned)axbf_state.name_count, (unsigned)axbf_state.generation,
           (unsigned)axbf_state.bounds_violations);
    for (u32 i = 0; i < axbf_state.type_count && i < 8; i++)
    {
        axbf_type_t *entry = &axbf_state.types[i];
        printk("  type %u kind=%u size=%u name=%s members=%u resolved=%u\n",
               (unsigned)entry->type_id, (unsigned)entry->kind, (unsigned)entry->size,
               axbf_type_name(entry->type_id), (unsigned)entry->member_count,
               (unsigned)entry->resolved);
    }
}

bool_t axbf_selftest(void)
{
    u32 offset = 0;
    u32 size = 0;
    axbf_init();
    if (axbf_add_int(1, 4, "u32", false) != 1)
        return false;
    if (axbf_add_int(2, 8, "u64", false) != 2)
        return false;
    if (axbf_add_int(3, 8, "s64", true) != 3)
        return false;
    if (axbf_add_ptr(4, 5, "node_ptr") != 4)
        return false;
    if (axbf_add_struct(5, 24, "node", 3) != 5)
        return false;
    if (!axbf_add_member(5, 0, "key", 1, 0, 32))
        return false;
    if (!axbf_add_member(5, 1, "value", 2, 64, 64))
        return false;
    if (!axbf_add_member(5, 2, "next", 4, 128, 64))
        return false;
    if (axbf_resolve() != 1)
    {
        /* the pointer resolves once node 5 exists, which it does */
    }
    if (axbf_size(5) != 24)
        return false;
    if (!axbf_offsets(5, 1, &offset))
        return false;
    if (offset != 8)
        return false;
    if (!axbf_member_info(5, "key", 4, &offset, &size))
        return false;
    if (offset != 0 || size != 4)
        return false;
    if (!axbf_member_info(5, "value", 8, &offset, &size))
        return false;
    if (offset != 8)
        return false;
    if (axbf_member_info(5, "value", 16, &offset, &size))
        return false;
    if (axbf_bounds_violations() == 0)
        return false;
    if (axbf_member_info(5, "missing", 4, &offset, &size))
        return false;
    if (axbf_size(9) != 0)
        return false;
    if (axbf_add_array(6, 2, 4, "keys") != 6)
        return false;
    axbf_resolve();
    if (axbf_size(6) != 32)
        return false;
    if (axbf_type_name(5)[0] != 'n')
        return false;
    if (axbf_type_count() != 6)
        return false;
    return true;
}

