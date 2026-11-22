/* AxiomOS - HVCI front end (C side).
 *
 * HVCI ("Hypervisor-enforced Code Integrity") in AxiomOS is deliberately
 * modest: the kernel records the hash of every executable region during boot,
 * marks those pages read-only through the page tables, and, when the CPU
 * exposes VMX, hands control to the Rust `axi_hvci` crate which runs the
 * kernel as a guest with second-level (EPT) enforcement of the same policy.
 *
 * If VMX is unavailable (nested virtualisation disabled, TCG emulation,
 * firmware lock) the subsystem degrades to software mode: the same hashes and
 * W^X page permissions, checked at region registration rather than by the
 * hypervisor.  The mode is always reported truthfully in the UI and the log.
 */

#include <axiom/kernel.h>

#define HVCI_MAX_REGIONS 32

struct hvci_region {
    u64  base;
    u64  size;
    u32  flags;         /* 1 = executable, 2 = writable */
    u8   digest[32];
    char name[32];
    bool_t valid;
};

static struct hvci_region g_regions[HVCI_MAX_REGIONS];
static u32  g_region_count;
static hvci_status_t g_status;
static bool_t g_hvci_ready;
static u64  g_manifest_start;
static u64  g_manifest_end;

/* Provided by the Rust crate when it is linked.  Weak symbols let the C side
 * build and boot even without the Rust static library. */
extern u32 rust_hvci_start(void) AXIOM_WEAK;
extern void rust_hvci_stop(void) AXIOM_WEAK;
extern u64 rust_hvci_protected_pages(void) AXIOM_WEAK;
extern u64 rust_hvci_violations(void) AXIOM_WEAK;
extern u64 rust_sha256(const u8 *data, u64 size, u8 out[32]) AXIOM_WEAK;

/* Kernel section boundaries from the linker script. */
extern u8 __rodata_start[];
extern u8 __rodata_end[];
extern u8 __data_start[];
extern u8 __data_end[];
extern u8 __text_end[];

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void describe_mode(u32 mode, const char *detail)
{
    g_status.mode = mode;
    u32 i = 0;
    while (detail[i] && i < sizeof(g_status.detail) - 1) {
        g_status.detail[i] = detail[i];
        i++;
    }
    g_status.detail[i] = 0;
}

static u64 hash_region(const u8 *base, u64 size, u8 out[32])
{
    if (rust_sha256)
        return rust_sha256(base, size, out);
    return sha256_hash(base, size, out);
}

/* ------------------------------------------------------------------ */
/* Region registry                                                    */
/* ------------------------------------------------------------------ */

void hvci_register_region(u64 base, u64 size, const char *name)
{
    if (g_region_count >= HVCI_MAX_REGIONS) {
        printk("hvci: region registry full, ignoring '%s'\n", name);
        return;
    }
    struct hvci_region *r = &g_regions[g_region_count++];
    memset(r, 0, sizeof(*r));
    r->base = base & AXIOM_PAGE_MASK;
    r->size = AXIOM_PAGE_ALIGN((base & ~AXIOM_PAGE_MASK) + size);
    r->flags = 1;
    hash_region((const u8 *)r->base, r->size, r->digest);
    r->valid = true;
    u32 n = 0;
    while (name && name[n] && n < 31) {
        r->name[n] = name[n];
        n++;
    }
    r->name[n] = 0;

    g_status.protected_pages += r->size / AXIOM_PAGE_SIZE;
    printk("hvci: region '%s' %p..%p (%u KiB) digest %02x%02x%02x%02x...\n",
           r->name, (void *)r->base, (void *)(r->base + r->size),
           r->size / 1024, (u64)r->digest[0], (u64)r->digest[1],
           (u64)r->digest[2], (u64)r->digest[3]);
}

u64 hvci_hash_region(u64 base, u64 size)
{
    u8 digest[32];
    hash_region((const u8 *)base, size, digest);
    u64 result = 0;
    for (u32 i = 0; i < 8; i++)
        result = (result << 8) | digest[i];
    return result;
}

/* Recompute every registered region and compare with the boot manifest.  This
 * is the software-mode integrity check; it runs on demand (boot, after a
 * driver load, and periodically from the GUI's security page). */
bool_t hvci_verify_kernel(void)
{
    bool_t all_ok = true;

    for (u32 i = 0; i < g_region_count; i++) {
        struct hvci_region *r = &g_regions[i];
        u8 digest[32];
        if (!r->valid)
            continue;
        hash_region((const u8 *)r->base, r->size, digest);
        if (memcmp(digest, r->digest, 32) != 0) {
            g_status.violations++;
            all_ok = false;
            printk("hvci: integrity failure in '%s' at %p\n", r->name,
                   (void *)r->base);
        }
        g_status.verified_pages += r->size / AXIOM_PAGE_SIZE;
    }

    if (g_status.violations == 0)
        printk("hvci: %u regions verified, %u pages included in the manifest\n",
               g_region_count, g_status.verified_pages);
    else
        printk("hvci: %u integrity violations detected\n", g_status.violations);

    /* Remember the manifest digest so the UI can display a stable value. */
    {
        u8 digest[32];
        hash_region((const u8 *)g_manifest_start,
                    (u64)(g_manifest_end - g_manifest_start), digest);
        g_status.manifest_hash = 0;
        for (u32 i = 0; i < 8; i++)
            g_status.manifest_hash = (g_status.manifest_hash << 8) | digest[i];
    }
    return all_ok;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */

void hvci_init(void)
{
    memset(&g_status, 0, sizeof(g_status));
    g_region_count = 0;
    g_manifest_start = 0x100000;
    g_manifest_end = (u64)__text_end;

    describe_mode(0, "disabled");

    printk("hvci: initialising (vmx %Zb, smx %Zb, hypervisor %Zb)\n",
           cpu_has_vmx(), cpu_has_smx(), cpu_is_hypervisor());

    /* The kernel's own executable and read-only regions are the first entries
     * in the manifest; drivers add theirs when they load. */
    hvci_register_region(0x100000, (u64)__text_end - 0x100000, "kernel-text");
    hvci_register_region((u64)__rodata_start,
                         (u64)__rodata_end - (u64)__rodata_start,
                         "kernel-rodata");

    /* Read-only, executable page permissions for the text region. */
    hvci_protect_readonly(0x100000, (u64)__text_end - 0x100000);

    if (!cpu_has_vmx()) {
        describe_mode(1, "software mode: vmx unavailable, using W^X + hashes");
        g_status.active = 1;
        g_hvci_ready = true;
        printk("hvci: active in SOFTWARE mode (paging + hashes)\n");
        return;
    }

    if (rust_hvci_start) {
        u32 result = rust_hvci_start();
        if (result == 0) {
            describe_mode(2, "vmx mode: kernel running under the axi hypervisor");
            g_status.active = 1;
            g_status.protected_pages += rust_hvci_protected_pages ?
                                        rust_hvci_protected_pages() : 0;
            g_hvci_ready = true;
            printk("hvci: active in VMX mode (EPT enforcement)\n");
            return;
        }
        printk("hvci: vmx launch failed (%u), falling back to software mode\n",
               (u64)result);
    } else {
        printk("hvci: rust hypervisor not linked, software mode only\n");
    }

    describe_mode(1, "software mode: vmx launch unavailable, using W^X + hashes");
    g_status.active = 1;
    g_hvci_ready = true;
    printk("hvci: active in SOFTWARE mode\n");
}

void hvci_get_status(hvci_status_t *out)
{
    if (!out)
        return;
    *out = g_status;
    if (g_hvci_ready && g_status.mode == 2) {
        if (rust_hvci_violations)
            out->violations = rust_hvci_violations();
    }
}

bool_t hvci_active(void)
{
    return g_hvci_ready && g_status.active != 0;
}

void hvci_protect_readonly(u64 base, u64 size)
{
    u64 start = base & AXIOM_PAGE_MASK;
    u64 end = AXIOM_PAGE_ALIGN(base + size);
    for (u64 addr = start; addr < end; addr += AXIOM_PAGE_SIZE) {
        u64 phys = vmm_translate(addr);
        if (!phys)
            continue;
        /* Present + global, no writable bit: reads and executes are allowed,
         * writes fault.  The fault handler will report an integrity violation.
         */
        vmm_map(addr, phys & AXIOM_PAGE_MASK, PTE_PRESENT | PTE_GLOBAL);
    }
}

/* Called from the page fault handler when a write to a protected page is
 * detected: gives a precise diagnostic instead of a generic panic. */
void hvci_handle_write_fault(u64 address)
{
    for (u32 i = 0; i < g_region_count; i++) {
        struct hvci_region *r = &g_regions[i];
        if (address >= r->base && address < r->base + r->size) {
            g_status.violations++;
            panic("hvci: write to protected region '%s' at %p (possible exploit)",
                  r->name, (void *)address);
        }
    }
}

void hvci_dump(void)
{
    printk("hvci: mode %u (%s), active %Zb\n", (u64)g_status.mode,
           g_status.detail, g_status.active);
    printk("hvci: %u regions, %u protected pages, %u verified pages, %u violations\n",
           g_region_count, g_status.protected_pages, g_status.verified_pages,
           g_status.violations);
    for (u32 i = 0; i < g_region_count; i++) {
        if (!g_regions[i].valid)
            continue;
        printk("  '%s' %p +%u KiB\n", g_regions[i].name,
               (void *)g_regions[i].base, g_regions[i].size / 1024);
    }
}
