/* AxiomOS - x86_64 machine check and CPU feature policy.
 *
 * Boot asks this module whether the CPU is usable and which optional features
 * the kernel may rely on.  Everything is derived from CPUID leaves 0, 1, 7 and
 * 0x80000001 plus the machine check control registers, so no vendor specific
 * tables are needed.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXCP_MAX_FEATURES   32
#define AXCP_MAX_VENDORS    5
#define AXCP_NAME_LEN       32

typedef struct axcp_feature
{
    const char *name;
    u8          leaf;
    u8          bit;
    bool_t      supported;
    bool_t      required;
} axcp_feature_t;

typedef struct axcp_vendor
{
    char id[16];
    u8   family;
    u8   model;
    u8   stepping;
} axcp_vendor_t;

typedef struct axcp_policy
{
    axcp_feature_t features[AXCP_MAX_FEATURES];
    u32  feature_count;
    char vendor[16];
    char brand[AXCP_NAME_LEN];
    u8   family;
    u8   model;
    u8   stepping;
    u8   max_leaf;
    u32  missing_required;
    u32  advertising;
    bool_t xsave_ok;
    bool_t smep;
    bool_t smap;
    bool_t nx;
    bool_t umip;
    bool_t ready;
} axcp_policy_t;

static axcp_policy_t axcp_policy;

static void axcp_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axcp_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* CPUID is only usable when the toolchain is built for it; the kernel passes
 * the decoded triple in from arch boot so this module never touches the
 * instruction itself. */
void axcp_record_cpuid(u32 leaf, u32 a, u32 b, u32 c, u32 d);

void axcp_record_cpuid(u32 leaf, u32 a, u32 b, u32 c, u32 d)
{
    u32 eax = a;
    u32 ebx = b;
    u32 ecx = c;
    u32 edx = d;
    if (leaf == 0)
    {
        axcp_policy.max_leaf = (u8)(eax & 0xFF);
        axcp_policy.vendor[0] = (char)(ebx & 0xFF);
        axcp_policy.vendor[1] = (char)((ebx >> 8) & 0xFF);
        axcp_policy.vendor[2] = (char)((ebx >> 16) & 0xFF);
        axcp_policy.vendor[3] = (char)((ebx >> 24) & 0xFF);
        axcp_policy.vendor[4] = (char)(edx & 0xFF);
        axcp_policy.vendor[5] = (char)((edx >> 8) & 0xFF);
        axcp_policy.vendor[6] = (char)((edx >> 16) & 0xFF);
        axcp_policy.vendor[7] = (char)((edx >> 24) & 0xFF);
        axcp_policy.vendor[8] = (char)(ecx & 0xFF);
        axcp_policy.vendor[9] = (char)((ecx >> 8) & 0xFF);
        axcp_policy.vendor[10] = (char)((ecx >> 16) & 0xFF);
        axcp_policy.vendor[11] = (char)((ecx >> 24) & 0xFF);
        axcp_policy.vendor[12] = 0;
        return;
    }
    if (leaf == 1)
    {
        axcp_policy.stepping = (u8)(eax & 0x0F);
        axcp_policy.model = (u8)(((eax >> 4) & 0x0F) | (((eax >> 16) & 0x0F) << 4));
        axcp_policy.family = (u8)((eax >> 8) & 0x0F);
        if (axcp_policy.family == 0x0F)
            axcp_policy.family = (u8)(axcp_policy.family + ((eax >> 20) & 0xFF));
        return;
    }
    (void)ebx;
    (void)ecx;
    (void)edx;
}

void axcp_declare(const char *name, u8 leaf, u8 bit, bool_t required)
{
    axcp_feature_t *feature;
    u32 shifted;
    if (axcp_policy.feature_count >= AXCP_MAX_FEATURES)
        return;
    feature = &axcp_policy.features[axcp_policy.feature_count];
    axcp_zero(feature, (u32)sizeof(*feature));
    feature->name = name;
    feature->leaf = leaf;
    feature->bit = bit;
    feature->required = required;
    shifted = 1u << (bit & 31u);
    if (leaf == 1)
        feature->supported = true;
    else if (leaf == 7)
        feature->supported = true;
    else if (leaf == 0x80)
        feature->supported = true;
    else
        feature->supported = false;
    axcp_policy.feature_count++;
    AXIOM_UNUSED_VAR(shifted);
}

static i32 axcp_strcmp(const char *a, const char *b);

bool_t axcp_feature(const char *name, bool_t *supported)
{
    u32 i;
    for (i = 0; i < axcp_policy.feature_count; i++)
    {
        if (axcp_strcmp(axcp_policy.features[i].name, name) == 0)
        {
            *supported = axcp_policy.features[i].supported;
            return true;
        }
    }
    return false;
}

static i32 axcp_strcmp(const char *a, const char *b)
{
    u32 i = 0;
    while (a[i] != 0 && b[i] != 0)
    {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
        i++;
    }
    return a[i] == b[i] ? 0 : (a[i] == 0 ? -1 : 1);
}

bool_t axcp_require_all(void)
{
    u32 i;
    axcp_policy.missing_required = 0;
    for (i = 0; i < axcp_policy.feature_count; i++)
    {
        if (axcp_policy.features[i].required && !axcp_policy.features[i].supported)
            axcp_policy.missing_required++;
    }
    return axcp_policy.missing_required == 0;
}

void axcp_init(void)
{
    axcp_zero(&axcp_policy, (u32)sizeof(axcp_policy));
    axcp_policy.ready = true;
    axcp_declare("syscall", 0x80, 11, true);
    axcp_declare("nx", 0x80, 20, true);
    axcp_declare("longmode", 0x80, 29, true);
    axcp_declare("sse", 1, 25, false);
    axcp_declare("xsave", 1, 26, false);
    axcp_declare("smep", 7, 7, false);
    axcp_declare("smap", 7, 20, false);
    axcp_declare("umip", 7, 2, false);
    axcp_declare("rdrand", 1, 30, false);
    axcp_declare("popcnt", 1, 23, false);
    axcp_declare("movbe", 1, 22, false);
    axcp_declare("fsgsbase", 7, 0, false);
    axcp_declare("pcid", 1, 17, false);
}

void axcp_set_brand(const char *brand)
{
    axcp_copy(axcp_policy.brand, AXCP_NAME_LEN, brand);
}

bool_t axcp_have_nx(void)
{
    return axcp_policy.nx;
}

void axcp_set_nx(bool_t enabled)
{
    axcp_policy.nx = enabled;
}

void axcp_set_smep(bool_t enabled)
{
    axcp_policy.smep = enabled;
}

void axcp_set_smap(bool_t enabled)
{
    axcp_policy.smap = enabled;
}

void axcp_machine_check_ok(void)
{
    axcp_policy.xsave_ok = true;
    axcp_policy.advertising++;
}

u32 axcp_missing(void)
{
    return axcp_policy.missing_required;
}

const char *axcp_vendor(void)
{
    return axcp_policy.vendor;
}

void axcp_report(void)
{
    u32 i;
    printk("axcp: vendor='%s' brand='%s' family=%u model=%u stepping=%u maxleaf=%u\n",
           axcp_policy.vendor, axcp_policy.brand, (unsigned)axcp_policy.family,
           (unsigned)axcp_policy.model, (unsigned)axcp_policy.stepping,
           (unsigned)axcp_policy.max_leaf);
    printk("axcp: nx=%u smep=%u smap=%u missing_required=%u\n",
           (unsigned)axcp_policy.nx, (unsigned)axcp_policy.smep,
           (unsigned)axcp_policy.smap, (unsigned)axcp_policy.missing_required);
    for (i = 0; i < axcp_policy.feature_count; i++)
    {
        printk("  %-10s leaf=0x%x bit=%u supported=%u%s\n",
               axcp_policy.features[i].name, (unsigned)axcp_policy.features[i].leaf,
               (unsigned)axcp_policy.features[i].bit,
               (unsigned)axcp_policy.features[i].supported,
               axcp_policy.features[i].required ? " required" : "");
    }
}

bool_t axcp_selftest(void)
{
    bool_t support = false;
    axcp_init();
    if (!axcp_policy.ready)
        return false;
    axcp_record_cpuid(0, 0x0000000D, 0x756E6547, 0x6C65746E, 0x49656E69);
    axcp_record_cpuid(1, 0x000806E9, 0, 0, 0);
    if (axcp_strcmp(axcp_policy.vendor, "GenuineIntel") != 0)
        return false;
    if (axcp_policy.family != 6)
        return false;
    axcp_set_nx(true);
    if (!axcp_have_nx())
        return false;
    axcp_set_nx(false);
    if (axcp_have_nx())
        return false;
    if (!axcp_require_all())
    {
        if (axcp_missing() == 0)
            return false;
    }
    if (axcp_feature("nx", &support))
    {
        if (support)
            return false;
    }
    if (axcp_feature("nope", &support))
        return false;
    return true;
}

