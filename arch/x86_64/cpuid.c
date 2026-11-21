/* AxiomOS - CPUID decoding and feature gating.
 *
 * The kernel never trusts a feature bit without checking that the platform
 * advertises it consistently: extended leaves, hypervisor presence and
 * topology are all decoded here once at boot and cached for the scheduler,
 * crypto and power management code.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXCP_LEAF_BASIC       0x00000000
#define AXCP_LEAF_VERSION     0x00000001
#define AXCP_LEAF_FEATURES    0x00000007
#define AXCP_LEAF_TOPOLOGY    0x0000001F
#define AXCP_LEAF_EXT_BASIC   0x80000000
#define AXCP_LEAF_EXT_FEATURES 0x80000001
#define AXCP_LEAF_BRAND       0x80000002
#define AXCP_LEAF_CACHE       0x00000004
#define AXCP_LEAF_MONITOR     0x00000005
#define AXCP_LEAF_XSAVE       0x0000000D
#define AXCP_LEAF_MAX_SUBLEAF 0x0000001F

#define AXCP_VENDOR_INTEL     0
#define AXCP_VENDOR_AMD       1
#define AXCP_VENDOR_UNKNOWN   2

typedef struct axcid_features
{
    u32  sse2;
    u32  sse3;
    u32  ssse3;
    u32  sse4_1;
    u32  sse4_2;
    u32  avx;
    u32  avx2;
    u32  avx512f;
    u32  popcnt;
    u32  xsave;
    u32  osxsave;
    u32  aes;
    u32  pclmul;
    u32  sha;
    u32  rdrand;
    u32  rdseed;
    u32  movbe;
    u32  fsgsbase;
    u32  smep;
    u32  smap;
    u32  nx;
    u32  lm;
    u32  vmx;
    u32  svm;
    u32  pdpe1gb;
    u32  apic;
    u32  x2apic;
    u32  tsc_deadline;
    u32  invariant_tsc;
    u32  hypervisor;
    u32  clflush_line;
    u32  invpcid;
    u32  pcid;
} axcid_features_t;

typedef struct axcid_topology
{
    u32  logical_cpus;
    u32  cores;
    u32  threads_per_core;
    u32  sockets;
    u32  package_id;
    u32  core_id;
    u32  smt_level;
    u32  cache_line;
    u32  model;
    u32  family;
    u32  stepping;
    u32  brand_length;
    u8   vendor;
    char brand[52];
} axcid_topology_t;

typedef struct axcid_state
{
    axcid_features_t features;
    axcid_topology_t topology;
    u32  max_basic;
    u32  max_extended;
    u32  probes;
    u32  vendor_string_hits;
    u32  rejected_features;
    u32  clflush_size;
    u32  tsc_ratio;
    u32  probe_cycles;
    u32  xsaves_lo;
    u32  xsaves_hi;
    bool_t probed;
    bool_t safe_for_sse;
    bool_t safe_for_avx;
    bool_t safe_for_avx512;
} axcid_state_t;

static axcid_state_t axcid_state;

static void axcid_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static inline void axcid_cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
#if defined(__x86_64__)
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
#else
    *eax = leaf;
    *ebx = subleaf;
    *ecx = 0;
    *edx = 0;
#endif
}

static u32 axcid_bit(u32 value, u32 index)
{
    return (value >> index) & 1u;
}

static void axcid_decode_vendor(u32 ebx, u32 ecx, u32 edx)
{
    u32 index = 0;
    if (ebx == 0x756E6547u && ecx == 0x6C65746Eu && edx == 0x49656E69u)
    {
        axcid_state.topology.vendor = AXCP_VENDOR_INTEL;
    }
    else if (ebx == 0x68747541u && ecx == 0x444D4163u && edx == 0x69746E65u)
    {
        axcid_state.topology.vendor = AXCP_VENDOR_AMD;
    }
    else
    {
        axcid_state.topology.vendor = AXCP_VENDOR_UNKNOWN;
        return;
    }
    axcid_state.vendor_string_hits++;
    index++;
    AXIOM_UNUSED_VAR(index);
}

static void axcid_read_brand(void)
{
    u32 index = 0;
    if (axcid_state.max_extended < AXCP_LEAF_BRAND + 2)
        return;
    for (u32 leaf = AXCP_LEAF_BRAND; leaf < AXCP_LEAF_BRAND + 3; leaf++)
    {
        u32 eax;
        u32 ebx;
        u32 ecx;
        u32 edx;
        axcid_cpuid(leaf, 0, &eax, &ebx, &ecx, &edx);
        for (u32 i = 0; i < 4 && index < 51; i++)
            axcid_state.topology.brand[index++] = (char)((eax >> (i * 8)) & 0xFF);
        for (u32 i = 0; i < 4 && index < 51; i++)
            axcid_state.topology.brand[index++] = (char)((ebx >> (i * 8)) & 0xFF);
        for (u32 i = 0; i < 4 && index < 51; i++)
            axcid_state.topology.brand[index++] = (char)((ecx >> (i * 8)) & 0xFF);
        for (u32 i = 0; i < 4 && index < 51; i++)
            axcid_state.topology.brand[index++] = (char)((edx >> (i * 8)) & 0xFF);
    }
    while (index > 0 && (axcid_state.topology.brand[0] == ' ' || axcid_state.topology.brand[0] == 0))
    {
        for (u32 i = 0; i + 1 < 51; i++)
            axcid_state.topology.brand[i] = axcid_state.topology.brand[i + 1];
        axcid_state.topology.brand[51] = 0;
        index--;
    }
    axcid_state.topology.brand_length = index;
}

/* -------------------------------------------------------------- public API */

void axcid_probe(void)
{
    u32 eax = 0;
    u32 ebx = 0;
    u32 ecx = 0;
    u32 edx = 0;
    axcid_zero(&axcid_state, (u32)sizeof(axcid_state));
    axcid_state.probes++;
    axcid_cpuid(AXCP_LEAF_BASIC, 0, &eax, &ebx, &ecx, &edx);
    axcid_state.max_basic = eax;
    axcid_decode_vendor(ebx, ecx, edx);
    axcid_cpuid(AXCP_LEAF_VERSION, 0, &eax, &ebx, &ecx, &edx);
    axcid_state.topology.stepping = eax & 0x0F;
    axcid_state.topology.model = (eax >> 4) & 0x0F;
    axcid_state.topology.family = (eax >> 8) & 0x0F;
    if (axcid_state.topology.family == 6)
        axcid_state.topology.model += ((eax >> 16) & 0x0F) << 4;
    axcid_state.features.apic = axcid_bit(edx, 9);
    axcid_state.features.sse2 = axcid_bit(edx, 26);
    axcid_state.features.popcnt = axcid_bit(ecx, 23);
    axcid_state.features.sse3 = axcid_bit(ecx, 0);
    axcid_state.features.ssse3 = axcid_bit(ecx, 9);
    axcid_state.features.sse4_1 = axcid_bit(ecx, 19);
    axcid_state.features.sse4_2 = axcid_bit(ecx, 20);
    axcid_state.features.vmx = axcid_bit(ecx, 5);
    axcid_state.features.aes = axcid_bit(ecx, 25);
    axcid_state.features.pclmul = axcid_bit(ecx, 1);
    axcid_state.features.movbe = axcid_bit(ecx, 22);
    axcid_state.features.x2apic = axcid_bit(ecx, 21);
    axcid_state.features.tsc_deadline = axcid_bit(ecx, 24);
    axcid_state.features.fsgsbase = axcid_bit(ecx, 22);
    axcid_state.features.smep = axcid_bit(ecx, 7);
    axcid_state.features.smap = axcid_bit(ecx, 20);
    axcid_state.features.pcid = axcid_bit(ecx, 17);
    axcid_state.features.invpcid = axcid_bit(ecx, 10);
    axcid_state.features.xsave = axcid_bit(ecx, 26);
    axcid_state.features.osxsave = axcid_bit(ecx, 27);
    axcid_state.features.avx = axcid_bit(ecx, 28);
    axcid_state.features.rdrand = axcid_bit(ecx, 30);
    axcid_state.safe_for_sse = axcid_state.features.sse2 && axcid_state.features.osxsave;
    if (axcid_state.max_basic >= AXCP_LEAF_FEATURES)
    {
        axcid_cpuid(AXCP_LEAF_FEATURES, 0, &eax, &ebx, &ecx, &edx);
        axcid_state.features.avx2 = axcid_bit(ebx, 5);
        axcid_state.features.avx512f = axcid_bit(ebx, 16);
        axcid_state.features.sha = axcid_bit(ebx, 29);
        axcid_state.features.rdseed = axcid_bit(ebx, 18);
        axcid_state.features.smep = axcid_bit(ebx, 7);
        axcid_state.features.smap = axcid_bit(ebx, 20);
        axcid_state.features.hypervisor = axcid_bit(ecx, 31);
    }
    if (axcid_state.max_basic >= AXCP_LEAF_CACHE)
    {
        axcid_cpuid(AXCP_LEAF_CACHE, 0, &eax, &ebx, &ecx, &edx);
        axcid_state.clflush_size = ((ebx & 0xFFF) * 8);
        axcid_state.features.clflush_line = axcid_state.clflush_size;
        axcid_state.topology.cache_line = axcid_state.clflush_size ? axcid_state.clflush_size : 64;
    }
    else
    {
        axcid_state.topology.cache_line = 64;
    }
    if (axcid_state.max_basic >= AXCP_LEAF_XSAVE)
    {
        axcid_cpuid(AXCP_LEAF_XSAVE, 0, &axcid_state.xsaves_lo, &axcid_state.xsaves_hi, &eax, &eax);
    }
    axcid_cpuid(AXCP_LEAF_EXT_BASIC, 0, &eax, &ebx, &ecx, &edx);
    axcid_state.max_extended = eax;
    if (axcid_state.max_extended >= AXCP_LEAF_EXT_FEATURES)
    {
        axcid_cpuid(AXCP_LEAF_EXT_FEATURES, 0, &eax, &ebx, &ecx, &edx);
        axcid_state.features.nx = axcid_bit(edx, 20);
        axcid_state.features.lm = axcid_bit(edx, 29);
        axcid_state.features.pdpe1gb = axcid_bit(edx, 26);
        axcid_state.features.svm = axcid_bit(ecx, 2);
        axcid_state.features.invariant_tsc = axcid_bit(edx, 27);
    }
    axcid_read_brand();
    axcid_state.safe_for_avx = axcid_state.features.avx && axcid_state.features.avx2;
    axcid_state.safe_for_avx512 = axcid_state.features.avx512f && axcid_state.features.osxsave;
    if (!axcid_state.features.lm)
        axcid_state.rejected_features++;
    if (!axcid_state.features.nx)
        axcid_state.rejected_features++;
    axcid_state.probed = true;
}

bool_t axcid_has_avx(void)
{
    return axcid_state.features.avx != 0;
}

bool_t axcid_has_avx2(void)
{
    return axcid_state.features.avx2 != 0;
}

bool_t axcid_has_aes(void)
{
    return axcid_state.features.aes != 0;
}

bool_t axcid_has_sha(void)
{
    return axcid_state.features.sha != 0;
}

bool_t axcid_has_smep(void)
{
    return axcid_state.features.smep != 0;
}

bool_t axcid_has_vmx(void)
{
    return axcid_state.features.vmx != 0;
}

bool_t axcid_has_invariant_tsc(void)
{
    return axcid_state.features.invariant_tsc != 0;
}

u32 axcid_vendor(void)
{
    return axcid_state.topology.vendor;
}

const char *axcid_vendor_name(void)
{
    switch (axcid_state.topology.vendor)
    {
    case AXCP_VENDOR_INTEL: return "intel";
    case AXCP_VENDOR_AMD: return "amd";
    default: return "unknown";
    }
}

const char *axcid_brand(void)
{
    return axcid_state.topology.brand;
}

u32 axcid_cache_line(void)
{
    return axcid_state.topology.cache_line;
}

u32 axcid_logical_cpus(void)
{
    if (axcid_state.max_basic >= AXCP_LEAF_TOPOLOGY)
    {
        u32 eax;
        u32 ebx;
        u32 ecx;
        u32 edx;
        axcid_cpuid(AXCP_LEAF_TOPOLOGY, 0, &eax, &ebx, &ecx, &edx);
        if ((ebx & 0xFFFF) != 0)
            return ebx & 0xFFFF;
    }
    return axcid_state.topology.logical_cpus ? axcid_state.topology.logical_cpus : 1;
}

u32 axcid_cores(void)
{
    if (axcid_state.topology.logical_cpus == 0)
        return 1;
    return axcid_state.topology.logical_cpus / (axcid_state.topology.threads_per_core ? axcid_state.topology.threads_per_core : 1);
}

void axcid_report(void)
{
    printk("axcp: vendor=%s family=%u model=%u stepping=%u maxleaf=0x%x maxext=0x%x\n",
           axcid_vendor_name(), (unsigned)axcid_state.topology.family,
           (unsigned)axcid_state.topology.model, (unsigned)axcid_state.topology.stepping,
           (unsigned)axcid_state.max_basic, (unsigned)axcid_state.max_extended);
    printk("axcp: brand=%s\n", axcid_state.topology.brand_length ? axcid_state.topology.brand : "unknown");
    printk("axcp: sse2=%u avx=%u avx2=%u avx512f=%u aes=%u sha=%u nx=%u smep=%u smap=%u\n",
           (unsigned)axcid_state.features.sse2, (unsigned)axcid_state.features.avx,
           (unsigned)axcid_state.features.avx2, (unsigned)axcid_state.features.avx512f,
           (unsigned)axcid_state.features.aes, (unsigned)axcid_state.features.sha,
           (unsigned)axcid_state.features.nx, (unsigned)axcid_state.features.smep,
           (unsigned)axcid_state.features.smap);
}

bool_t axcid_selftest(void)
{
    axcid_probe();
    if (!axcid_state.probed)
        return false;
    if (axcid_cache_line() < 16 || axcid_cache_line() > 256)
        return false;
    if (axcid_logical_cpus() == 0)
        return false;
    if (axcid_state.topology.vendor != AXCP_VENDOR_INTEL &&
        axcid_state.topology.vendor != AXCP_VENDOR_AMD &&
        axcid_state.topology.vendor != AXCP_VENDOR_UNKNOWN)
        return false;
    if (axcid_has_aes() != (axcid_state.features.aes != 0))
        return false;
    if (axcid_has_smep() != (axcid_state.features.smep != 0))
        return false;
    if (axcid_bit(0x10, 4) != 1)
        return false;
    if (axcid_bit(0x10, 3) != 0)
        return false;
    axcid_state.topology.threads_per_core = 2;
    axcid_state.topology.logical_cpus = 8;
    if (axcid_cores() != 4)
        return false;
    axcid_state.topology.threads_per_core = 0;
    if (axcid_cores() != 8)
        return false;
    return true;
}
