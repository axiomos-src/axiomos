/* AxiomOS - time stamp counter calibration and monotonic clock.
 *
 * The TSC is calibrated against the PIT or HPET channel once at boot; afterwards
 * the kernel converts cycles to nanoseconds using a 32.32 fixed point multiplier.
 * Drift compensation keeps the wall clock stable on hosts where the reference
 * oscillator is slow by a few hundred ppm.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXTS_CALIBRATE_MS     50
#define AXTS_MS_PER_DAY       86400000ull
#define AXTS_MAX_DRIFT_PPM    5000
#define AXTS_SAMPLES          16
#define AXTS_32_32_SHIFT      32

typedef struct axts_sample
{
    u64  tsc;
    u64  reference;
    u64  delta;
    u32  ppm;
    bool_t valid;
} axts_sample_t;

typedef struct axts_state
{
    axts_sample_t samples[AXTS_SAMPLES];
    u64  base_tsc;
    u64  last_tsc;
    u64  multiplier_hi;
    u64  multiplier_lo;
    u64  nanoseconds;
    u64  reference_ns;
    u64  cycles_per_ms;
    u64  cycles_per_us;
    u32  sample_count;
    u32  calibrations;
    u32  ppm_estimate;
    u32  ppm_correction;
    u32  wrap_count;
    u32  monotonic_violations;
    u32  calibrate_cost_us;
    bool_t stable;
    bool_t invariant;
    bool_t calibrated;
} axts_state_t;

static axts_state_t axts_state;

static void axts_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static inline u64 axts_read(void)
{
    u32 low;
    u32 high;
#if defined(__x86_64__)
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
#else
    low = 0;
    high = 0;
#endif
    return ((u64)high << 32) | (u64)low;
}

static void axts_compute_multiplier(u64 cycles_per_ms)
{
    /* multiplier = cycles_per_ms * 1e6 ns / ms expressed as 64 bit fixed point */
    u64 numerator = cycles_per_ms * 1000000ull;
    if (cycles_per_ms == 0)
        return;
    /* Store the reciprocal instead: ns_per_cycle scaled by 2^32. */
    axts_state.multiplier_lo = (1000000000ull * (1ull << 32)) / (cycles_per_ms * 1000ull);
    axts_state.multiplier_hi = axts_state.multiplier_lo >> 32;
}

/* -------------------------------------------------------------- public API */

void axts_init(u64 initial_tsc)
{
    axts_zero(&axts_state, (u32)sizeof(axts_state));
    axts_state.base_tsc = initial_tsc;
    axts_state.last_tsc = initial_tsc;
    axts_state.nanoseconds = 0;
}

bool_t axts_calibrate(u64 start_tsc, u64 end_tsc, u64 elapsed_us)
{
    u64 delta;
    u32 ppm;
    if (end_tsc <= start_tsc || elapsed_us == 0)
        return false;
    delta = end_tsc - start_tsc;
    axts_state.cycles_per_us = delta / elapsed_us;
    axts_state.cycles_per_ms = axts_state.cycles_per_us * 1000ull;
    axts_state.calibrate_cost_us = (u32)elapsed_us;
    if (axts_state.cycles_per_ms == 0)
        return false;
    ppm = (u32)((delta * 1000000ull) / (elapsed_us * 1000ull));
    if (ppm > 1000000u)
        ppm = 1000000u;
    if (axts_state.sample_count < AXTS_SAMPLES)
    {
        axts_sample_t *sample = &axts_state.samples[axts_state.sample_count];
        sample->tsc = delta;
        sample->reference = elapsed_us;
        sample->delta = delta;
        sample->ppm = ppm;
        sample->valid = true;
        axts_state.sample_count++;
    }
    axts_state.ppm_estimate = ppm;
    axts_compute_multiplier(axts_state.cycles_per_ms);
    axts_state.calibrations++;
    axts_state.calibrated = true;
    axts_state.stable = axts_state.sample_count >= 3;
    return true;
}

u64 axts_now(void)
{
    u64 current;
    u64 delta;
    u64 ns;
    if (!axts_state.calibrated)
        return axts_state.nanoseconds;
    current = axts_read();
    if (current < axts_state.last_tsc)
    {
        axts_state.wrap_count++;
        axts_state.monotonic_violations++;
        axts_state.last_tsc = current;
        return axts_state.nanoseconds;
    }
    delta = current - axts_state.last_tsc;
    axts_state.last_tsc = current;
    ns = (delta * axts_state.multiplier_lo) >> AXTS_32_32_SHIFT;
    if (axts_state.ppm_correction != 0)
    {
        u64 correction = ns * axts_state.ppm_correction;
        ns -= correction / 1000000ull;
    }
    axts_state.nanoseconds += ns;
    return axts_state.nanoseconds;
}

u64 axts_now_ms(void)
{
    return axts_now() / 1000000ull;
}

u64 axts_cycles_to_ns(u64 cycles)
{
    if (!axts_state.calibrated)
        return 0;
    return (cycles * axts_state.multiplier_lo) >> AXTS_32_32_SHIFT;
}

u64 axts_ns_to_cycles(u64 nanoseconds)
{
    if (!axts_state.calibrated || axts_state.cycles_per_ms == 0)
        return 0;
    return (nanoseconds * axts_state.cycles_per_ms) / 1000000ull;
}

u64 axts_cpu_hz(void)
{
    return axts_state.cycles_per_us * 1000000ull;
}

u32 axts_ppm(void)
{
    return axts_state.ppm_estimate;
}

void axts_set_drift_ppm(u32 ppm)
{
    if (ppm > AXTS_MAX_DRIFT_PPM)
        ppm = AXTS_MAX_DRIFT_PPM;
    axts_state.ppm_correction = ppm;
}

bool_t axts_stable(void)
{
    return axts_state.stable;
}

u64 axts_spin_until(u64 target_ns, u32 limit)
{
    u32 spins = 0;
    while (axts_state.nanoseconds < target_ns && spins < limit)
    {
        axts_now();
        spins++;
    }
    return axts_state.nanoseconds;
}

void axts_update_reference(u64 reference_ns)
{
    axts_state.reference_ns = reference_ns;
    if (axts_state.nanoseconds > reference_ns + 1000000000ull)
        axts_state.ppm_correction = 100;
}

void axts_snooze(u32 microseconds)
{
    u64 target = axts_now() + (u64)microseconds * 1000ull;
    u64 now = axts_now();
    u32 spins = 0;
    while (now < target && spins < 1000000)
    {
        cpu_relax();
        now = axts_now();
        spins++;
    }
}

void axts_report(void)
{
    printk("axts: calibrated=%u stable=%u cycles/us=%u cpu=%uMHz ppm=%u drift=%u\n",
           (unsigned)axts_state.calibrated, (unsigned)axts_state.stable,
           (unsigned)axts_state.cycles_per_us,
           (unsigned)(axts_cpu_hz() / 1000000ull),
           (unsigned)axts_state.ppm_estimate, (unsigned)axts_state.ppm_correction);
    printk("axts: samples=%u calibrations=%u wraps=%u violations=%u ns=%u\n",
           (unsigned)axts_state.sample_count, (unsigned)axts_state.calibrations,
           (unsigned)axts_state.wrap_count, (unsigned)axts_state.monotonic_violations,
           (unsigned)axts_state.nanoseconds);
}

bool_t axts_selftest(void)
{
    u64 before;
    axts_init(1000);
    if (axts_now() != 0)
        return false;
    if (axts_now_ms() != 0)
        return false;
    if (axts_calibrate(1000, 1000 + 3000000ull, 1000) != true)
        return false;
    if (axts_state.cycles_per_us != 3000)
        return false;
    if (axts_calibrate(0, 0, 0))
        return false;
    if (axts_calibrate(5, 4, 1))
        return false;
    if (axts_calibrate(1000, 1000 + 6000000ull, 2000) != true)
        return false;
    if (axts_stable())
        return false;
    if (axts_calibrate(1000, 1000 + 9000000ull, 3000) != true)
        return false;
    if (!axts_stable())
        return false;
    before = axts_state.nanoseconds;
    if (axts_cycles_to_ns(3000) == 0)
        return false;
    if (axts_ns_to_cycles(1000000ull) == 0)
        return false;
    axts_snooze(1);
    if (axts_state.nanoseconds < before)
        return false;
    axts_set_drift_ppm(9000);
    if (axts_state.ppm_correction != AXTS_MAX_DRIFT_PPM)
        return false;
    axts_set_drift_ppm(50);
    if (axts_state.ppm_correction != 50)
        return false;
    if (axts_cpu_hz() == 0)
        return false;
    return true;
}

