//! AxiomOS kernel Rust facade (`axi_kernel`).
//!
//! The kernel proper is written in C for the parts that interact with the
//! bootloader, paging and device drivers; Rust carries the security layer
//! (HVCI + hashing) and the utilities that benefit from `core`'s type system.
//! This crate is the single static library the linker pulls in: it re-exports
//! the other crates and exports the `extern "C"` symbols the C front end
//! declares weakly (`rust_hvci_start`, `rust_sha256`, ...).
//!
//! Because the C side declares every one of these as `AXIOM_WEAK`, a kernel
//! built without this library still links and simply degrades to software
//! integrity checking.

#![no_std]

pub use axi_crypto as crypto;
pub use axi_hvci as hvci;
pub use axi_rt as rt;

use core::panic::PanicInfo;

// -------------------------------------------------------------------------
// C interface
// -------------------------------------------------------------------------

extern "C" {
    /// Kernel logging entry point (kernel/core/log.c).
    fn kputs(text: *const u8);
    /// Fatal error path (kernel/core/log.c).  Declared as noreturn.
    fn panic(message: *const u8) -> !;
}

fn report(text: &str) {
    // The string is not NUL terminated, so build a bounded buffer.
    let mut buffer = [0u8; 128];
    let bytes = text.as_bytes();
    let len = core::cmp::min(bytes.len(), buffer.len() - 1);
    buffer[..len].copy_from_slice(&bytes[..len]);
    unsafe { kputs(buffer.as_ptr()) };
}

#[panic_handler]
fn panic_handler(info: &PanicInfo) -> ! {
    match info.location() {
        Some(location) => {
            report("rust: panic at ");
            report(location.file());
        }
        None => report("rust: panic (no location)"),
    }
    let message = [b'r', b'u', b's', b't', 0];
    unsafe { panic(message.as_ptr()) }
}

// -------------------------------------------------------------------------
// Exported symbols
// -------------------------------------------------------------------------

/// Start the HVCI security layer.
///
/// Returns 0 when a hypervisor was launched, or one of the `HVCI_ERR_*` codes
/// when only the software layer is available (which is the normal case on
/// TCG-emulated CPUs, in which VMX is not exposed).
#[no_mangle]
pub extern "C" fn rust_hvci_start() -> u32 {
    let capability = hvci::probe();
    if capability.supported == 0 {
        return hvci::HVCI_ERR_NO_VMX;
    }
    hvci::launch()
}

/// Stop the HVCI layer (no-op when running in software mode).
#[no_mangle]
pub extern "C" fn rust_hvci_stop() {}

/// Number of pages covered by the integrity manifest.
#[no_mangle]
pub extern "C" fn rust_hvci_protected_pages() -> u64 {
    hvci::protected_pages()
}

/// Number of integrity violations observed so far.
#[no_mangle]
pub extern "C" fn rust_hvci_violations() -> u64 {
    hvci::violations()
}

/// Register an executable region with the manifest.
#[no_mangle]
pub unsafe extern "C" fn rust_hvci_register(base: u64, size: u64) -> u32 {
    if hvci::register_region(base, size, "kernel") {
        1
    } else {
        0
    }
}

/// Re-verify the manifest.
#[no_mangle]
pub unsafe extern "C" fn rust_hvci_verify() -> u32 {
    if hvci::verify() {
        1
    } else {
        0
    }
}

/// Second SHA-256 implementation, used to cross-check the C one.
#[no_mangle]
pub unsafe extern "C" fn rust_sha256(data: *const u8, len: usize, out: *mut u8) {
    let digest = crypto::sha256_raw(data, len);
    core::ptr::copy_nonoverlapping(digest.as_ptr(), out, 32);
}

/// CRC-32 over a buffer, exported for the filesystem layer.
#[no_mangle]
pub unsafe extern "C" fn rust_crc32(data: *const u8, len: usize) -> u32 {
    let slice = core::slice::from_raw_parts(data, len);
    crypto::crc32(slice)
}

/// A tiny self test executed during boot; returns the number of failures.
#[no_mangle]
pub extern "C" fn rust_selftest() -> u32 {
    let mut failures = 0u32;

    // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    let digest = crypto::sha256(b"abc");
    let expected = [
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    ];
    if !crypto::digest_equal(&digest, &expected) {
        failures += 1;
    }

    // SHA-256 of the empty string.
    let empty = crypto::sha256(b"");
    let empty_expected = [
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
    ];
    if !crypto::digest_equal(&empty, &empty_expected) {
        failures += 1;
    }

    // CRC-32("123456789") = 0xCBF43926.
    if crypto::crc32(b"123456789") != 0xCBF4_3926 {
        failures += 1;
    }

    // Integer square root.
    if rt::isqrt(144) != 12 || rt::isqrt(1_000_000) != 1000 {
        failures += 1;
    }

    failures
}
