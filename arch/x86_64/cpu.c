/* AxiomOS - CPU identification and feature reporting.
 *
 * Everything here runs before the heap exists, so the module is self contained
 * and only uses static storage.
 */

#include <axiom/kernel.h>

/* Leaf 1 feature bits worth reporting during boot. */
struct feature_bit {
    u32 leaf;
    u32 reg;      /* 0 = eax, 1 = ebx, 2 = ecx, 3 = edx */
    u32 bit;
    const char *name;
};

static const struct feature_bit features[] = {
    { 1, 3, 0,  "fpu" },   { 1, 3, 4,  "tsc" },   { 1, 3, 5,  "msr" },
    { 1, 3, 6,  "pae" },   { 1, 3, 9,  "apic" },  { 1, 3, 11, "sysenter" },
    { 1, 3, 12, "mtrr" },  { 1, 3, 13, "pge" },   { 1, 3, 15, "cmov" },
    { 1, 3, 19, "clflush" },{1, 3, 23, "mmx" },   { 1, 3, 25, "sse" },
    { 1, 3, 26, "sse2" },  { 1, 2, 0,  "sse3" },  { 1, 2, 9,  "ssse3" },
    { 1, 2, 19, "sse4.1" },{ 1, 2, 20, "sse4.2" },{ 1, 2, 25, "aes" },
    { 1, 2, 21, "x2apic" },{ 1, 2, 31, "hypervisor" },
    { 0x80000001, 2, 11, "syscall" }, { 0x80000001, 3, 20, "nx" },
    { 0x80000001, 2, 26, "pdpe1gb" }, { 0x80000001, 2, 29, "lm" },
    { 7, 1, 3, "bmi1" },   { 7, 1, 5, "avx2" },   { 7, 1, 8, "bmi2" },
    { 7, 1, 9, "erms" },   { 7, 1, 16,"avx512f" },
    { 7, 2, 5, "vmx" },    { 7, 2, 6, "smx" },    { 7, 2, 16, "rdtscp" },
};

static u32 g_max_leaf;
static u32 g_max_ext_leaf;
static u32 g_vendor[3];
static char g_brand[49];
static u32 g_cpu_leaf1[4];
static u32 g_cpu_leaf7[4];
static u32 g_cpu_ext1[4];
static bool_t g_cpu_ready;

extern void cpu_cpuid(u32 leaf, u32 subleaf, u32 out[4]);

static void cpuid(u32 leaf, u32 subleaf, u32 out[4])
{
    cpu_cpuid(leaf, subleaf, out);
}

static void detect(void)
{
    u32 out[4];

    if (g_cpu_ready)
        return;

    cpuid(0, 0, out);
    g_max_leaf = out[0];
    g_vendor[0] = out[1];
    g_vendor[1] = out[3];
    g_vendor[2] = out[2];

    cpuid(0x80000000, 0, out);
    g_max_ext_leaf = out[0];

    if (g_max_leaf >= 1)
        cpuid(1, 0, g_cpu_leaf1);
    if (g_max_leaf >= 7)
        cpuid(7, 0, g_cpu_leaf7);
    if (g_max_ext_leaf >= 0x80000001)
        cpuid(0x80000001, 0, g_cpu_ext1);

    /* Brand string: three leaves of four characters, NUL terminated. */
    if (g_max_ext_leaf >= 0x80000004) {
        u32 b[4];
        for (u32 i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, b);
            memcpy(&g_brand[i * 16], b, 16);
        }
        g_brand[48] = 0;
        /* Trim leading spaces. */
        char *start = g_brand;
        while (*start == ' ')
            start++;
        if (start != g_brand)
            memmove(g_brand, start, strlen(start) + 1);
        char *end = g_brand + strlen(g_brand);
        while (end > g_brand && (end[-1] == ' ' || end[-1] == 0))
            *--end = 0;
    } else {
        memcpy(g_brand, "unknown x86_64 processor", 25);
    }

    g_cpu_ready = true;
}

u64 cpu_brand_string(void)
{
    detect();
    return (u64)g_brand;
}

bool_t cpu_has_feature(u32 leaf, u32 bit)
{
    detect();
    if (leaf == 1)
        return (g_cpu_leaf1[bit / 32] & AXIOM_BIT(bit % 32)) != 0;
    if (leaf == 7)
        return (g_cpu_leaf7[bit / 32] & AXIOM_BIT(bit % 32)) != 0;
    if (leaf == 0x80000001)
        return (g_cpu_ext1[bit / 32] & AXIOM_BIT(bit % 32)) != 0;
    return false;
}

/* Convenience wrappers used by the HVCI and APIC code paths. */
bool_t cpu_has_vmx(void)
{
    detect();
    if (g_max_leaf < 7)
        return false;
    return (g_cpu_leaf7[2] & AXIOM_BIT(5)) != 0;   /* ECX bit 5 */
}

bool_t cpu_has_smx(void)
{
    detect();
    if (g_max_leaf < 7)
        return false;
    return (g_cpu_leaf7[2] & AXIOM_BIT(6)) != 0;
}

bool_t cpu_has_x2apic(void)
{
    detect();
    if (g_max_leaf < 1)
        return false;
    return (g_cpu_leaf1[2] & AXIOM_BIT(21)) != 0;
}

bool_t cpu_is_hypervisor(void)
{
    detect();
    return (g_cpu_leaf1[2] & AXIOM_BIT(31)) != 0;
}

u32 cpu_apic_id(void)
{
    detect();
    if (g_max_leaf < 1)
        return 0;
    return g_cpu_leaf1[1] >> 24;   /* EBX[31:24] */
}

u32 cpu_max_leaf(void)
{
    detect();
    return g_max_leaf;
}

void cpu_print_features(void)
{
    detect();
    printk("cpu: vendor %.12s (max leaf %x, ext %x)\n",
           (const char *)g_vendor, (u64)g_max_leaf, (u64)g_max_ext_leaf);

    char line[128];
    u32 used = 0;
    line[0] = 0;
    for (u32 i = 0; i < AXIOM_ARRAY_LEN(features); i++) {
        const struct feature_bit *f = &features[i];
        bool_t present = false;

        if (f->leaf == 1)
            present = (g_cpu_leaf1[f->reg] & AXIOM_BIT(f->bit)) != 0;
        else if (f->leaf == 7)
            present = (g_cpu_leaf7[f->reg] & AXIOM_BIT(f->bit)) != 0;
        else if (f->leaf == 0x80000001)
            present = (g_cpu_ext1[f->reg] & AXIOM_BIT(f->bit)) != 0;
        else
            continue;
        if (!present)
            continue;

        u32 len = (u32)strlen(f->name);
        if (used + len + 2 >= sizeof(line))
            break;
        if (used) {
            line[used++] = ' ';
        }
        memcpy(&line[used], f->name, len);
        used += len;
        line[used] = 0;
    }
    printk("cpu: features: %s\n", line);
}

/* Invoked once from kernel_main through cpu_init_report(): validates that
 * the CPU is actually in long mode with the features AxiomOS requires. */
void cpu_init(void)
{
    detect();

    if (!cpu_has_feature(0x80000001, 29))
        panic("cpu: long mode is not available");

    if (!cpu_has_feature(0x80000001, 20))
        printk("cpu: warning, NX is not available; W^X cannot be enforced\n");

    if (!cpu_has_feature(1, 9))
        printk("cpu: warning, no local APIC; SMP is unavailable\n");

    printk("cpu: %s%s\n", cpu_is_hypervisor() ? "virtual machine" : "bare metal",
           cpu_has_vmx() ? ", vmx capable" : ", no vmx");
}
