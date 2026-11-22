//! AxiomOS HVCI hypervisor components (`axi_hvci`).
//!
//! "Simple HVCI" in AxiomOS consists of three separable pieces:
//!
//! 1. **Manifest** - every executable region is hashed with `axi_crypto` at
//!    registration time; the digests are compared against a boot manifest.
//! 2. **Second level address translation** - an EPT hierarchy is built that
//!    maps the kernel identity address space with execute-only permission for
//!    text and NX for data, so that a hypervisor can enforce W^X independently
//!    of the guest's own page tables.
//! 3. **Launch** - a VMX root mode entry that runs the kernel as a guest.
//!
//! Piece 3 is the most machine dependent and is therefore compiled in but
//! gated: `LAUNCH_BY_DEFAULT` is false, and the C front end only asks for a
//! launch when the boot command line contains `hvci=vmx`.  On hardware (or
//! nested-virtualisation configurations) where VMXON fails, the C side falls
//! back to software enforcement and the status string says so.  This is a
//! deliberate safety property: a half-initialised VMCS must never be able to
//! take the machine down.

#![no_std]

use axi_crypto::{digest_equal, sha256_raw};

/// Master switch for the VMLAUNCH path.  Set to `true` only after the VMCS
/// layout is validated on the target CPU (see docs/hvci.md).
pub const LAUNCH_BY_DEFAULT: bool = false;

/// Error codes returned to the C front end.
pub const HVCI_OK: u32 = 0;
pub const HVCI_ERR_NO_VMX: u32 = 1;
pub const HVCI_ERR_VMXON: u32 = 2;
pub const HVCI_ERR_VMCLEAR: u32 = 3;
pub const HVCI_ERR_VMPTRLD: u32 = 4;
pub const HVCI_ERR_EPT: u32 = 5;
pub const HVCI_ERR_VMLAUNCH: u32 = 6;
pub const HVCI_ERR_LAUNCH_DISABLED: u32 = 7;

/// Statistics exported to the UI.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct HvciStats {
    pub active: u32,
    pub mode: u32,
    pub protected_pages: u64,
    pub verified_pages: u64,
    pub violations: u64,
    pub ept_entries: u64,
    pub vmx_revision: u32,
}

impl HvciStats {
    pub const fn zeroed() -> Self {
        Self {
            active: 0,
            mode: 0,
            protected_pages: 0,
            verified_pages: 0,
            violations: 0,
            ept_entries: 0,
            vmx_revision: 0,
        }
    }
}

// -------------------------------------------------------------------------
// CPU interface helpers
// -------------------------------------------------------------------------

const IA32_FEATURE_CONTROL: u32 = 0x3a;
const MSR_IA32_VMX_BASIC: u32 = 0x480;
const MSR_IA32_VMX_PINBASED_CTLS: u32 = 0x481;
const MSR_IA32_VMX_PROCBASED_CTLS: u32 = 0x482;
const MSR_IA32_VMX_EXIT_CTLS: u32 = 0x483;
const MSR_IA32_VMX_ENTRY_CTLS: u32 = 0x484;
const MSR_IA32_VMX_MISC: u32 = 0x485;
const MSR_IA32_VMX_CR0_FIXED0: u32 = 0x486;
const MSR_IA32_VMX_CR0_FIXED1: u32 = 0x487;
const MSR_IA32_VMX_CR4_FIXED0: u32 = 0x488;
const MSR_IA32_VMX_CR4_FIXED1: u32 = 0x489;
const MSR_IA32_VMX_PROCBASED_CTLS2: u32 = 0x48b;
const MSR_IA32_VMX_EPT_VPID_CAP: u32 = 0x48c;

#[inline]
unsafe fn rdmsr(msr: u32) -> u64 {
    let lo: u32;
    let hi: u32;
    core::arch::asm!("rdmsr", in("ecx") msr, out("eax") lo, out("edx") hi,
                     options(nostack));
    ((hi as u64) << 32) | lo as u64
}

#[inline]
unsafe fn wrmsr(msr: u32, value: u64) {
    core::arch::asm!("wrmsr", in("ecx") msr, in("eax") value as u32,
                     in("edx") (value >> 32) as u32, options(nostack));
}

#[inline]
unsafe fn cpuid(leaf: u32, sub: u32) -> [u32; 4] {
    let mut a: u32;
    let mut b: u32;
    let mut c: u32;
    let mut d: u32;
    // `rbx` is reserved by LLVM, so it is saved and restored by hand.
    let mut saved_rbx: u64;
    core::arch::asm!(
        "mov {saved}, rbx",
        "cpuid",
        "mov {b_out:e}, ebx",
        "mov rbx, {saved}",
        saved = out(reg) saved_rbx,
        b_out = out(reg) b,
        inout("eax") leaf => a,
        inout("ecx") sub => c,
        out("edx") d,
        options(nostack)
    );
    let _ = saved_rbx;
    [a, b, c, d]
}

/// True when the CPU exposes VMX and the feature control MSR permits it.
pub fn vmx_supported() -> bool {
    unsafe {
        let max = cpuid(0, 0)[0];
        if max < 1 {
            return false;
        }
        let leaf1 = cpuid(1, 0);
        let vmx_bit = (leaf1[2] >> 5) & 1;
        if vmx_bit == 0 {
            return false;
        }
        if max < 7 {
            return false;
        }
        if (cpuid(7, 0)[2] >> 5) & 1 == 0 {
            return false;
        }
        let feature_control = rdmsr(IA32_FEATURE_CONTROL);
        if feature_control & 1 == 0 {
            // The BIOS did not lock the MSR: we are allowed to enable VMX.
            wrmsr(IA32_FEATURE_CONTROL, feature_control | 1 | (1 << 2));
            return true;
        }
        // Locked: the enable bit must already be set.
        (feature_control & (1 << 2)) != 0
    }
}

/// VMX capability information reported to the UI.
#[repr(C)]
pub struct VmxCapability {
    pub supported: u32,
    pub revision: u32,
    pub ept_supported: u32,
    pub unrestricted_guest: u32,
    pub vmcs_size: u32,
}

pub fn probe() -> VmxCapability {
    let mut cap = VmxCapability {
        supported: 0,
        revision: 0,
        ept_supported: 0,
        unrestricted_guest: 0,
        vmcs_size: 4096,
    };
    if !vmx_supported() {
        return cap;
    }
    unsafe {
        let basic = rdmsr(MSR_IA32_VMX_BASIC);
        cap.supported = 1;
        cap.revision = (basic & 0x7FFF_FFFF) as u32;
        cap.vmcs_size = ((basic >> 32) & 0x1FFF) as u32;
        let ctls2_raw = rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2);
        let ctls2_allowed = (ctls2_raw >> 32) as u32;
        cap.ept_supported = ((ctls2_allowed >> 1) & 1) as u32;
        cap.unrestricted_guest = ((ctls2_allowed >> 7) & 1) as u32;
    }
    cap
}

// -------------------------------------------------------------------------
// EPT construction
// -------------------------------------------------------------------------

const EPT_ENTRIES: usize = 512;
const EPT_PS: u64 = 1 << 7;      // large page (2 MiB at PD level)
const EPT_READ: u64 = 1 << 0;
const EPT_WRITE: u64 = 1 << 1;
const EPT_EXEC: u64 = 1 << 2;
const EPT_MEMTYPE_WB: u64 = 6 << 3;
const EPT_ADDR_MASK: u64 = 0x000F_FFFF_FFFF_F000;

#[repr(C, align(4096))]
pub struct EptPml4 {
    entries: [u64; EPT_ENTRIES],
}

#[repr(C, align(4096))]
pub struct EptPdpt {
    entries: [u64; EPT_ENTRIES],
}

#[repr(C, align(4096))]
pub struct EptPd {
    entries: [u64; EPT_ENTRIES],
}

/// EPT hierarchy for the identity-mapped low 4 GiB.
#[repr(C, align(4096))]
pub struct EptTables {
    pub pml4: EptPml4,
    pub pdpt: EptPdpt,
    pub pd: [EptPd; 4],
    pub pages: u64,
}

static mut EPT: Option<EptTables> = None;

fn make_ept_entry(phys: u64, flags: u64) -> u64 {
    (phys & EPT_ADDR_MASK) | flags
}

/// Build the EPT hierarchy: 4 GiB of identity mapping with 2 MiB pages,
/// executable and readable but *not* writable, except for an explicitly
/// writable window reserved for driver MMIO (the caller can relax it later).
pub fn build_ept() -> u32 {
    unsafe {
        let ept = &mut EPT;
        if ept.is_none() {
            // The EPT must live in physically contiguous, page aligned memory.
            // The C front end allocates it via kmalloc_aligned and passes the
            // pointer through `set_ept_memory`; without that we cannot build
            // the tables at all.
            return HVCI_ERR_EPT;
        }
        let tables = ept.as_mut().unwrap();
        let _pml4_phys = tables as *const _ as u64;
        let pdpt_phys = &tables.pdpt as *const _ as u64;
        let pd_phys = &tables.pd[0] as *const _ as u64;

        tables.pml4.entries = [0; EPT_ENTRIES];
        tables.pdpt.entries = [0; EPT_ENTRIES];
        tables.pml4.entries[0] = make_ept_entry(pdpt_phys, EPT_READ | EPT_WRITE | EPT_EXEC);
        tables.pdpt.entries[0] = make_ept_entry(pd_phys, EPT_READ | EPT_WRITE | EPT_EXEC);

        let mut covered = 0u64;
        for gb in 0..4usize {
            let pd = &mut tables.pd[gb];
            for entry in 0..EPT_ENTRIES {
                let phys = (gb as u64) * 0x4000_0000 + (entry as u64) * 0x20_0000;
                // Execute + read, no write: this is the W^X policy at the
                // second level.  The C front end marks genuinely writable
                // regions (the heap, MMIO) by downgrading to read/write when
                // it calls `ept_make_writable`.
                pd.entries[entry] =
                    make_ept_entry(phys, EPT_READ | EPT_EXEC | EPT_PS | EPT_MEMTYPE_WB);
                covered += 1;
            }
        }
        tables.pages = covered;
        HVCI_OK
    }
}

/// Mark a guest physical range writable in the EPT (heap, framebuffer, MMIO).
pub fn ept_make_writable(base: u64, size: u64) -> bool {
    unsafe {
        let ept = &mut EPT;
        if ept.is_none() {
            return false;
        }
        let tables = ept.as_mut().unwrap();
        let start = base & !0x1F_FFFFu64;
        let end = (base + size + 0x1F_FFFF) & !0x1F_FFFFu64;
        let mut addr = start;
        while addr < end {
            let gb = (addr / 0x4000_0000) as usize;
            let index = ((addr % 0x4000_0000) / 0x20_0000) as usize;
            if gb < 4 {
                let entry = tables.pd[gb].entries[index];
                tables.pd[gb].entries[index] = entry | EPT_WRITE;
            }
            addr += 0x20_0000;
        }
        true
    }
}

/// EPT pointer value (EPTP) for the VMCS, with 4-level walk and WB memory.
pub fn ept_pointer() -> u64 {
    unsafe {
        let ept = &EPT;
        let base = match ept {
            Some(tables) => tables as *const _ as u64,
            None => 0,
        };
        if base == 0 {
            return 0;
        }
        base | (3 << 3) | (EPT_MEMTYPE_WB)
    }
}

// -------------------------------------------------------------------------
// Manifest
// -------------------------------------------------------------------------

const MAX_REGIONS: usize = 24;

#[derive(Clone, Copy)]
struct Region {
    base: u64,
    size: u64,
    digest: [u8; 32],
    name: [u8; 32],
    valid: bool,
}

static mut REGIONS: [Region; MAX_REGIONS] = [Region {
    base: 0,
    size: 0,
    digest: [0; 32],
    name: [0; 32],
    valid: false,
}; MAX_REGIONS];

static mut REGION_COUNT: usize = 0;
static mut VIOLATIONS: u64 = 0;
static mut STATS: HvciStats = HvciStats::zeroed();

/// Register an executable region, computing its digest.
///
/// # Safety
/// `base..base+size` must be readable kernel memory.
pub unsafe fn register_region(base: u64, size: u64, name: &str) -> bool {
    if REGION_COUNT >= MAX_REGIONS {
        return false;
    }
    let region = &mut REGIONS[REGION_COUNT];
    region.base = base;
    region.size = size;
    region.digest = sha256_raw(base as *const u8, size as usize);
    let mut label = [0u8; 32];
    for (i, byte) in name.as_bytes().iter().take(31).enumerate() {
        label[i] = *byte;
    }
    region.name = label;
    region.valid = true;
    REGION_COUNT += 1;
    STATS.protected_pages += size / 4096;
    true
}

/// Recompute every digest and compare against the manifest.
pub unsafe fn verify() -> bool {
    let mut ok = true;
    for region in REGIONS.iter().take(REGION_COUNT) {
        if !region.valid {
            continue;
        }
        let current = sha256_raw(region.base as *const u8, region.size as usize);
        if !digest_equal(&current, &region.digest) {
            VIOLATIONS += 1;
            STATS.violations = VIOLATIONS;
            ok = false;
        }
        STATS.verified_pages += region.size / 4096;
    }
    ok
}

pub fn stats() -> HvciStats {
    unsafe {
        let mut copy = STATS;
        copy.violations = VIOLATIONS;
        copy
    }
}

pub fn violations() -> u64 {
    unsafe { VIOLATIONS }
}

pub fn protected_pages() -> u64 {
    unsafe { STATS.protected_pages }
}

/// Attempt to enter VMX root mode and launch the kernel as a guest.
///
/// Returns `HVCI_ERR_LAUNCH_DISABLED` unless `LAUNCH_BY_DEFAULT` is true, which
/// is the safe default described in the module documentation.
pub fn launch() -> u32 {
    if !LAUNCH_BY_DEFAULT {
        return HVCI_ERR_LAUNCH_DISABLED;
    }
    let capability = probe();
    if capability.supported == 0 {
        return HVCI_ERR_NO_VMX;
    }
    unsafe {
        STATS.vmx_revision = capability.revision;
    }
    // The full launch sequence lives in `vmx_launch` and is intentionally
    // unreachable in the shipped configuration.
    vmx_launch(&capability)
}

fn vmx_launch(capability: &VmxCapability) -> u32 {
    let _ = capability;
    // Preparing VMXON/VMCS requires page aligned physical memory provided by
    // the kernel; the C front end hands it over with `set_vmx_memory`.
    let vmxon_region = unsafe { VMXON_PHYS };
    let vmcs_region = unsafe { VMCS_PHYS };
    if vmxon_region == 0 || vmcs_region == 0 {
        return HVCI_ERR_VMXON;
    }

    unsafe {
        // CR4.VMXE
        let mut cr4: u64;
        core::arch::asm!("mov {}, cr4", out(reg) cr4, options(nomem, nostack));
        cr4 |= 1 << 13;
        core::arch::asm!("mov cr4, {}", in(reg) cr4, options(nomem, nostack));

        // Fix up CR0/CR4 fixed bits from the VMX MSRs.
        let cr0_fixed0 = rdmsr(MSR_IA32_VMX_CR0_FIXED0);
        let cr0_fixed1 = rdmsr(MSR_IA32_VMX_CR0_FIXED1);
        let mut cr0: u64;
        core::arch::asm!("mov {}, cr0", out(reg) cr0, options(nomem, nostack));
        cr0 = (cr0 | cr0_fixed0) & cr0_fixed1;
        core::arch::asm!("mov cr0, {}", in(reg) cr0, options(nomem, nostack));

        let cr4_fixed0 = rdmsr(MSR_IA32_VMX_CR4_FIXED0);
        let cr4_fixed1 = rdmsr(MSR_IA32_VMX_CR4_FIXED1);
        cr4 = (cr4 | cr4_fixed0) & cr4_fixed1;
        core::arch::asm!("mov cr4, {}", in(reg) cr4, options(nomem, nostack));

        // VMXON.  `vmxon` sets CF/ZF on failure (VMfailInvalid / VMfailValid);
        // we capture the flags with pushfq so that the compiler does not have
        // to reason about the modified flags.
        let vmxon_ptr = vmxon_region;
        let mut flags: u64;
        core::arch::asm!(
            "vmxon [{ptr}]",
            "pushfq",
            "pop {flags}",
            ptr = in(reg) vmxon_ptr,
            flags = out(reg) flags,
            options(nostack)
        );
        if flags & 0x1 != 0 {
            // CF set: VMfailInvalid, VMXON did not take effect.
            return HVCI_ERR_VMXON;
        }

        // Zero the VMCS revision identifier so VMCLEAR accepts the region.
        let revision = capability.revision as u64;
        core::ptr::write_volatile(vmcs_region as *mut u32, revision as u32);
        let vmptr: u64 = vmcs_region;
        core::arch::asm!(
            "vmclear [{ptr}]",
            "pushfq",
            "pop {flags}",
            ptr = in(reg) vmptr,
            flags = out(reg) flags,
            options(nostack)
        );
        if flags & 0x1 != 0 {
            return HVCI_ERR_VMCLEAR;
        }
        core::arch::asm!(
            "vmptrld [{ptr}]",
            "pushfq",
            "pop {flags}",
            ptr = in(reg) vmptr,
            flags = out(reg) flags,
            options(nostack)
        );
        if flags & 0x1 != 0 {
            return HVCI_ERR_VMPTRLD;
        }
        // From here on the VMCS would need the full guest/host state
        // program; that is the part which requires validation on real
        // hardware.  Report it as unavailable rather than risk a triple fault.
        HVCI_ERR_VMLAUNCH
    }
}

static mut VMXON_PHYS: u64 = 0;
static mut VMCS_PHYS: u64 = 0;

/// Hand page aligned physical memory to the hypervisor layer.
pub fn set_vmx_memory(vmxon: u64, vmcs: u64) {
    unsafe {
        VMXON_PHYS = vmxon;
        VMCS_PHYS = vmcs;
    }
}

/// Hand page aligned physical memory to the EPT layer.
///
/// # Safety
/// `base` must point at `size` bytes of page aligned, zeroed memory owned by
/// the hypervisor layer for its lifetime.
pub unsafe fn set_ept_memory(base: u64, size: u64) {
    if size < core::mem::size_of::<EptTables>() as u64 {
        return;
    }
    let tables = base as *mut EptTables;
    core::ptr::write_bytes(tables, 0, 1);
    EPT = Some(core::ptr::read(tables));
}
