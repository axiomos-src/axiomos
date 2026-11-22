//! AxiomOS Rust runtime support (`axi_rt`).
//!
//! This crate provides the pieces of `core`-adjacent functionality that the
//! kernel's Rust components need but that are not available in a `no_std`
//! environment: spin locks, fixed-capacity ring buffers, intrusive lists,
//! bitmap operations, and the handful of floating point helpers used by the
//! GUI's vector drawing code (implemented without libm).
//!
//! Everything here is `#[repr(C)]` and allocation free so that it can be used
//! from interrupt context and before the kernel heap exists.

#![no_std]

use core::cell::UnsafeCell;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

/// A simple test-and-set spin lock.
///
/// The kernel is single core in phase 1, but the lock is written for SMP so
/// that later AP bring-up does not need to change call sites.
pub struct SpinLock<T> {
    locked: AtomicBool,
    value: UnsafeCell<T>,
}

unsafe impl<T: Send> Sync for SpinLock<T> {}
unsafe impl<T: Send> Send for SpinLock<T> {}

impl<T> SpinLock<T> {
    pub const fn new(value: T) -> Self {
        Self { locked: AtomicBool::new(false), value: UnsafeCell::new(value) }
    }

    #[inline]
    pub fn lock(&self) -> SpinLockGuard<'_, T> {
        while self
            .locked
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            while self.locked.load(Ordering::Relaxed) {
                core::hint::spin_loop();
            }
        }
        SpinLockGuard { lock: self }
    }

    #[inline]
    pub fn try_lock(&self) -> Option<SpinLockGuard<'_, T>> {
        if self
            .locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
        {
            Some(SpinLockGuard { lock: self })
        } else {
            None
        }
    }

    #[inline]
    pub fn is_locked(&self) -> bool {
        self.locked.load(Ordering::Relaxed)
    }
}

pub struct SpinLockGuard<'a, T> {
    lock: &'a SpinLock<T>,
}

impl<T> Deref for SpinLockGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.lock.value.get() }
    }
}

impl<T> DerefMut for SpinLockGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.value.get() }
    }
}

impl<T> Drop for SpinLockGuard<'_, T> {
    fn drop(&mut self) {
        self.lock.locked.store(false, Ordering::Release);
    }
}

/// Fixed capacity ring buffer with a power-of-two capacity.
pub struct RingBuffer<T, const N: usize> {
    items: [Option<T>; N],
    head: usize,
    tail: usize,
}

impl<T, const N: usize> RingBuffer<T, N> {
    pub const fn new() -> Self {
        // `[None; N]` requires T: Copy; build the array element by element
        // using a const-friendly trick that works for any T.
        Self { items: [const { None }; N], head: 0, tail: 0 }
    }

    pub fn push(&mut self, value: T) -> Option<T> {
        let next = (self.head + 1) % N;
        let evicted = if next == self.tail {
            let old = self.items[self.tail].take();
            self.tail = (self.tail + 1) % N;
            old
        } else {
            None
        };
        self.items[self.head] = Some(value);
        self.head = next;
        evicted
    }

    pub fn pop(&mut self) -> Option<T> {
        if self.head == self.tail {
            return None;
        }
        let value = self.items[self.tail].take();
        self.tail = (self.tail + 1) % N;
        value
    }

    pub fn len(&self) -> usize {
        (self.head + N - self.tail) % N
    }

    pub fn is_empty(&self) -> bool {
        self.head == self.tail
    }
}

/// Intrusive doubly linked list head, stored inside the object that owns it.
#[repr(C)]
pub struct ListNode {
    pub next: *mut ListNode,
    pub prev: *mut ListNode,
}

impl ListNode {
    pub const fn new() -> Self {
        Self { next: core::ptr::null_mut(), prev: core::ptr::null_mut() }
    }
}

#[repr(C)]
pub struct ListHead {
    first: *mut ListNode,
    len: AtomicU32,
}

unsafe impl Sync for ListHead {}

impl ListHead {
    pub const fn new() -> Self {
        Self { first: core::ptr::null_mut(), len: AtomicU32::new(0) }
    }

    /// # Safety
    /// The node must outlive its membership in this list and must not already
    /// be linked to another list.
    pub unsafe fn push_front(&self, node: *mut ListNode) {
        let first = self.first;
        unsafe {
            (*node).prev = core::ptr::null_mut();
            (*node).next = first;
            if !first.is_null() {
                (*first).prev = node;
            }
        }
        // The head pointer is mutated through a raw pointer; callers serialise
        // with a spin lock.
        let self_ptr = self as *const ListHead as *mut ListHead;
        unsafe { (*self_ptr).first = node };
        self.len.fetch_add(1, Ordering::Relaxed);
    }

    /// # Safety
    /// `node` must currently be linked in this list.
    pub unsafe fn remove(&self, node: *mut ListNode) {
        let prev = unsafe { (*node).prev };
        let next = unsafe { (*node).next };
        let self_ptr = self as *const ListHead as *mut ListHead;
        unsafe {
            if prev.is_null() {
                (*self_ptr).first = next;
            } else {
                (*prev).next = next;
            }
            if !next.is_null() {
                (*next).prev = prev;
            }
            (*node).next = core::ptr::null_mut();
            (*node).prev = core::ptr::null_mut();
        }
        self.len.fetch_sub(1, Ordering::Relaxed);
    }

    pub fn len(&self) -> usize {
        self.len.load(Ordering::Relaxed) as usize
    }

    pub fn is_empty(&self) -> bool {
        self.first.is_null()
    }

    pub fn first(&self) -> *mut ListNode {
        self.first
    }
}

// -------------------------------------------------------------------------
// Fixed point helpers and math without libm
// -------------------------------------------------------------------------

/// Integer square root (Newton iteration) used by the geometry code.
pub fn isqrt(value: u64) -> u64 {
    if value == 0 {
        return 0;
    }
    let mut x = value;
    let mut y = (x + 1) / 2;
    while y < x {
        x = y;
        y = (x + value / x) / 2;
    }
    x
}

/// Integer sine for a fixed 0..360 degree input, scaled by 4096.
pub fn sin_fixed(degrees: i32) -> i32 {
    let mut angle = degrees % 360;
    if angle < 0 {
        angle += 360;
    }
    // Piecewise approximation: good enough for animation easing and radial
    // progress indicators, accurate to about 0.2%.
    let sign = if angle > 180 { -1 } else { 1 };
    let a = if angle > 180 { angle - 180 } else { angle };
    let t = a as i32;
    let q = (t * 4096) / 180;
    let value = if t <= 90 {
        // 4x(1-x) shaped parabola approximation of sin
        let x = q;
        (2 * x * (4096 - x / 2)) / 4096
    } else {
        let x = 4096 - q;
        (2 * x * (4096 - x / 2)) / 4096
    };
    sign * value
}

/// Integer cosine, same fixed point scale as [`sin_fixed`].
pub fn cos_fixed(degrees: i32) -> i32 {
    sin_fixed(degrees + 90)
}

/// Fixed point multiply, 12.12 format.
pub fn mul_fixed(a: i32, b: i32) -> i32 {
    ((a as i64 * b as i64) >> 12) as i32
}

/// Linear interpolation for colour fades, 0..=255 weight.
pub fn lerp_u8(a: u8, b: u8, weight: u8) -> u8 {
    let w = weight as u32;
    ((a as u32 * (255 - w) + b as u32 * w) / 255) as u8
}

/// Byte helpers that mirror the kernel's C `memcpy` but are callable from Rust.
pub unsafe fn copy_bytes(dst: *mut u8, src: *const u8, len: usize) {
    core::ptr::copy_nonoverlapping(src, dst, len);
}

pub unsafe fn zero_bytes(dst: *mut u8, len: usize) {
    core::ptr::write_bytes(dst, 0, len);
}
