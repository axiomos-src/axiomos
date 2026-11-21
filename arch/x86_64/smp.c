/* AxiomOS - symmetric multiprocessing bring-up.
 *
 * The bootstrap processor walks the MADT, sends INIT-SIPI-SIPI to each
 * application processor and waits for the trampoline to publish its stack.  This
 * module keeps the per CPU state block, the trampoline parameters and the
 * start-up barrier that the APs spin on until the BSP releases them.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXSM_MAX_CPUS        16
#define AXSM_TRAMPOLINE_SIZE 4096
#define AXSM_STACK_SIZE      16384
#define AXSM_AP_TIMEOUT      2000
#define AXSM_PATIENCE_TICKS  100

#define AXSM_CPU_OFFLINE     0
#define AXSM_CPU_STARTING    1
#define AXSM_CPU_ONLINE      2
#define AXSM_CPU_PARKED      3
#define AXSM_CPU_FAULTED     4

typedef struct axsm_cpu
{
    u32  apic_id;
    u32  index;
    u32  state;
    u32  ticks_since_start;
    u64  stack_top;
    u64  goto_address;
    u64  last_tsc;
    u32  ipis_sent;
    u32  ipis_received;
    u32  work_items;
    u32  steal_attempts;
    bool_t bsp;
    bool_t ready;
} axsm_cpu_t;

typedef struct axsm_state
{
    axsm_cpu_t cpus[AXSM_MAX_CPUS];
    u32  cpu_count;
    u32  online_count;
    u32  boot_apic_id;
    u32  ipi_broadcasts;
    u32  reschedule_ipis;
    u32  tlb_ipis;
    u32  faults;
    u32  smp_case;
    u64  trampoline;
    u64  release_barrier;
    bool_t started;
    bool_t bsp_only;
} axsm_state_t;

static axsm_state_t axsm_state;

static void axsm_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axsm_cpu_t *axsm_cpu(u32 index)
{
    if (index >= axsm_state.cpu_count)
        return NULL_PTR;
    return &axsm_state.cpus[index];
}

static u32 axsm_next_index(void)
{
    u32 i;
    for (i = 0; i < axsm_state.cpu_count; i++)
    {
        if (axsm_state.cpus[i].state == AXSM_CPU_OFFLINE)
            return i;
    }
    return 0xFFFFFFFFu;
}

/* -------------------------------------------------------------- public API */

void axsm_init(u64 trampoline)
{
    u32 i;
    axsm_zero(&axsm_state, (u32)sizeof(axsm_state));
    axsm_state.trampoline = trampoline;
    axsm_state.boot_apic_id = 0;
    axsm_state.cpus[0].apic_id = 0;
    axsm_state.cpus[0].index = 0;
    axsm_state.cpus[0].bsp = true;
    axsm_state.cpus[0].state = AXSM_CPU_ONLINE;
    axsm_state.cpus[0].ready = true;
    axsm_state.cpu_count = AXSM_MAX_CPUS;
    axsm_state.online_count = 1;
    axsm_state.release_barrier = 1;
    for (i = 1; i < AXSM_MAX_CPUS; i++)
    {
        axsm_state.cpus[i].index = i;
        axsm_state.cpus[i].state = AXSM_CPU_OFFLINE;
    }
    axsm_state.started = true;
}

u32 axsm_register(u32 apic_id)
{
    u32 index = axsm_next_index();
    axsm_cpu_t *cpu;
    if (index == 0xFFFFFFFFu)
        return 0xFFFFFFFFu;
    cpu = &axsm_state.cpus[index];
    cpu->apic_id = apic_id;
    cpu->state = AXSM_CPU_STARTING;
    cpu->stack_top = 0;
    cpu->ticks_since_start = 0;
    return index;
}

bool_t axsm_bring_up(u32 index)
{
    axsm_cpu_t *cpu = axsm_cpu(index);
    if (cpu == NULL_PTR || cpu->bsp)
        return false;
    if (cpu->state == AXSM_CPU_ONLINE)
        return true;
    cpu->state = AXSM_CPU_STARTING;
    cpu->ticks_since_start = 0;
    cpu->goto_address = axsm_state.trampoline;
    axsm_state.ipi_broadcasts++;
    return true;
}

bool_t axsm_acknowledge(u32 index, u64 stack_top)
{
    axsm_cpu_t *cpu = axsm_cpu(index);
    if (cpu == NULL_PTR)
        return false;
    if (cpu->state != AXSM_CPU_STARTING)
        return false;
    cpu->stack_top = stack_top;
    cpu->state = AXSM_CPU_ONLINE;
    cpu->ready = true;
    axsm_state.online_count++;
    return true;
}

u32 axsm_poll_startup(void)
{
    u32 i;
    u32 progressed = 0;
    for (i = 0; i < axsm_state.cpu_count; i++)
    {
        axsm_cpu_t *cpu = &axsm_state.cpus[i];
        if (cpu->state != AXSM_CPU_STARTING)
            continue;
        cpu->ticks_since_start++;
        if (cpu->ticks_since_start >= AXSM_AP_TIMEOUT)
        {
            if (cpu->ticks_since_start >= AXSM_AP_TIMEOUT * 2)
            {
                cpu->state = AXSM_CPU_FAULTED;
                axsm_state.faults++;
                continue;
            }
            axsm_state.ipi_broadcasts++;
        }
        progressed++;
    }
    return progressed;
}

bool_t axsm_send_ipi(u32 index, u8 vector)
{
    axsm_cpu_t *cpu = axsm_cpu(index);
    if (cpu == NULL_PTR || cpu->state != AXSM_CPU_ONLINE)
        return false;
    cpu->ipis_sent++;
    switch (vector)
    {
    case 0xF0:
        axsm_state.reschedule_ipis++;
        break;
    case 0xF1:
        axsm_state.tlb_ipis++;
        break;
    default:
        break;
    }
    return true;
}

u32 axsm_broadcast(u8 vector)
{
    u32 i;
    u32 sent = 0;
    for (i = 0; i < axsm_state.cpu_count; i++)
    {
        if (axsm_state.cpus[i].state != AXSM_CPU_ONLINE)
            continue;
        if (axsm_state.cpus[i].bsp && vector == 0xF0)
            continue;
        if (axsm_send_ipi(i, vector))
            sent++;
    }
    axsm_state.ipi_broadcasts++;
    return sent;
}

u32 axsm_online_mask(void)
{
    u32 mask = 0;
    u32 i;
    for (i = 0; i < axsm_state.cpu_count; i++)
    {
        if (axsm_state.cpus[i].state == AXSM_CPU_ONLINE)
            mask |= (1u << i);
    }
    return mask;
}

u32 axsm_online_count(void)
{
    return axsm_state.online_count;
}

bool_t axsm_park(u32 index)
{
    axsm_cpu_t *cpu = axsm_cpu(index);
    if (cpu == NULL_PTR || cpu->bsp)
        return false;
    cpu->state = AXSM_CPU_PARKED;
    if (axsm_state.online_count > 0)
        axsm_state.online_count--;
    return true;
}

bool_t axsm_wake(u32 index)
{
    axsm_cpu_t *cpu = axsm_cpu(index);
    if (cpu == NULL_PTR)
        return false;
    if (cpu->state != AXSM_CPU_PARKED)
        return false;
    cpu->state = AXSM_CPU_ONLINE;
    cpu->goto_address = axsm_state.trampoline;
    axsm_state.online_count++;
    return true;
}

u32 axsm_steal_work(u32 index, u32 quantum)
{
    axsm_cpu_t *cpu = axsm_cpu(index);
    if (cpu == NULL_PTR || cpu->state != AXSM_CPU_ONLINE)
        return 0;
    cpu->steal_attempts++;
    if (quantum == 0)
        return 0;
    cpu->work_items += quantum;
    return quantum;
}

u32 axsm_apic_id(u32 index)
{
    axsm_cpu_t *cpu = axsm_cpu(index);
    return cpu == NULL_PTR ? 0xFFFFFFFFu : cpu->apic_id;
}

u32 axsm_state_of(u32 index)
{
    axsm_cpu_t *cpu = axsm_cpu(index);
    return cpu == NULL_PTR ? AXSM_CPU_FAULTED : cpu->state;
}

void axsm_release_all(void)
{
    axsm_state.release_barrier = 1;
  }

void axsm_report(void)
{
    u32 i;
    printk("axsm: cpus=%u online=%u mask=0x%x ipi=%u resched=%u tlb=%u faults=%u\n",
           (unsigned)axsm_state.cpu_count, (unsigned)axsm_state.online_count,
           (unsigned)axsm_online_mask(), (unsigned)axsm_state.ipi_broadcasts,
           (unsigned)axsm_state.reschedule_ipis, (unsigned)axsm_state.tlb_ipis,
           (unsigned)axsm_state.faults);
    for (i = 0; i < axsm_state.cpu_count; i++)
    {
        const axsm_cpu_t *cpu = &axsm_state.cpus[i];
        if (cpu->state == AXSM_CPU_OFFLINE)
            continue;
        printk("  cpu %u apic=%u state=%u bsp=%u ipis=%u work=%u stack=0x%lx\n",
               (unsigned)i, (unsigned)cpu->apic_id, (unsigned)cpu->state,
               (unsigned)cpu->bsp, (unsigned)cpu->ipis_sent,
               (unsigned)cpu->work_items, (unsigned long)cpu->stack_top);
    }
}

bool_t axsm_selftest(void)
{
    u32 first;
    u32 second;
    axsm_init(0x8000);
    if (axsm_online_count() != 1)
        return false;
    first = axsm_register(2);
    if (first == 0xFFFFFFFFu)
        return false;
    second = axsm_register(4);
    if (second == 0xFFFFFFFFu)
        return false;
    if (!axsm_bring_up(first))
        return false;
    if (!axsm_acknowledge(first, 0x9000))
        return false;
    if (axsm_online_count() != 2)
        return false;
    if (axsm_poll_startup() != 1)
        return false;
    axsm_state.cpus[second].ticks_since_start = AXSM_AP_TIMEOUT * 2;
    if (axsm_poll_startup() == 0)
        return false;
    if (axsm_state.cpus[second].state != AXSM_CPU_FAULTED)
        return false;
    if (axsm_broadcast(0xF0) != 1)
        return false;
    if (!axsm_park(first))
        return false;
    if (axsm_online_count() != 1)
        return false;
    if (!axsm_wake(first))
        return false;
    if (axsm_steal_work(first, 3) != 3)
        return false;
    if (axsm_online_mask() != 0x3)
        return false;
    if (axsm_apic_id(1) != 2)
        return false;
    axsm_release_all();
    return true;
}

