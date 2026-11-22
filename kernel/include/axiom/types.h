/* AxiomOS - fundamental types and compiler attributes.
 *
 * The kernel is built freestanding, so this header is the only place that
 * defines the integer types used across C, assembly and Rust boundaries.
 * Rust sees the same layout through #[repr(C)] mirror types in rust/axi_types.
 */
#ifndef AXIOM_TYPES_H
#define AXIOM_TYPES_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef signed char         i8;
typedef signed short        i16;
typedef signed int          i32;
typedef signed long long    i64;
typedef unsigned long       usize;
typedef signed long         isize;
typedef u64                 phys_t;
typedef u64                 virt_t;
typedef u64                 paddr_t;
typedef u64                 vaddr_t;

typedef _Bool               bool_t;

#define true  ((bool_t)1)
#define false ((bool_t)0)

#define NULL_PTR ((void *)0)

#define AXIOM_PACKED        __attribute__((packed))
#define AXIOM_ALIGNED(n)    __attribute__((aligned(n)))
#define AXIOM_NORETURN      __attribute__((noreturn))
#define AXIOM_UNUSED        __attribute__((unused))
#define AXIOM_WEAK          __attribute__((weak))
#define AXIOM_PRINTF(f, a)  __attribute__((format(printf, f, a)))
#define AXIOM_INLINE        static inline __attribute__((always_inline))
#define AXIOM_SECTION(s)    __attribute__((section(s)))
#define AXIOM_USED          __attribute__((used))
#define AXIOM_ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define AXIOM_ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define AXIOM_ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))
#define AXIOM_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define AXIOM_MAX(a, b)        ((a) > (b) ? (a) : (b))
#define AXIOM_BIT(n)           (1ULL << (n))
#define AXIOM_UNUSED_VAR(x)    ((void)(x))

#define AXIOM_PAGE_SIZE     4096ULL
#define AXIOM_PAGE_MASK     (~(AXIOM_PAGE_SIZE - 1))
#define AXIOM_PAGE_ALIGN(p) AXIOM_ALIGN_UP((u64)(p), AXIOM_PAGE_SIZE)
#define AXIOM_HUGE_SIZE     (2ULL * 1024 * 1024)
#define AXIOM_KERNEL_BASE   0x100000ULL
#define AXIOM_NULL_PAGE     0x1000ULL

/* Static assertions usable at file scope. */
#define AXIOM_STATIC_ASSERT(cond, name) \
    typedef char axiom_static_assert_##name[(cond) ? 1 : -1]

/* Common compiler barriers. */
#define barrier()           __asm__ __volatile__("" ::: "memory")
#define cpu_relax()         __asm__ __volatile__("pause")
#define cli()               __asm__ __volatile__("cli" ::: "memory")
#define sti()               __asm__ __volatile__("sti" ::: "memory")
#define hlt()               __asm__ __volatile__("hlt")
#define read_cr0()          ({ u64 v; __asm__ __volatile__("mov %%cr0, %0" : "=r"(v)); v; })
#define read_cr2()          ({ u64 v; __asm__ __volatile__("mov %%cr2, %0" : "=r"(v)); v; })
#define read_cr3()          ({ u64 v; __asm__ __volatile__("mov %%cr3, %0" : "=r"(v)); v; })
#define read_cr4()          ({ u64 v; __asm__ __volatile__("mov %%cr4, %0" : "=r"(v)); v; })
#define write_cr0(v)        __asm__ __volatile__("mov %0, %%cr0" :: "r"((u64)(v)) : "memory")
#define write_cr3(v)        __asm__ __volatile__("mov %0, %%cr3" :: "r"((u64)(v)) : "memory")
#define write_cr4(v)        __asm__ __volatile__("mov %0, %%cr4" :: "r"((u64)(v)) : "memory")
#define read_msr(m)         ({ u32 lo, hi; __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"((u32)(m))); ((u64)hi << 32) | lo; })
#define write_msr(m, v)     do { u32 lo = (u32)(v), hi = (u32)((u64)(v) >> 32); \
                                 __asm__ __volatile__("wrmsr" :: "a"(lo), "d"(hi), "c"((u32)(m)) : "memory"); } while (0)
#define rdtsc()             ({ u32 lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); ((u64)hi << 32) | lo; })
#define invlpg(v)           __asm__ __volatile__("invlpg (%0)" :: "r"((u64)(v)) : "memory")
#define wfi()               __asm__ __volatile__("hlt")

#endif /* AXIOM_TYPES_H */
