/* AxiomOS - power management.
 *
 * Keeps the system sleep state machine, records wake reasons and exposes a
 * single entry point that the shell, settings app and ACPI SCI handler all
 * funnel into.  Nothing here touches hardware directly unless the ACPI reset
 * or sleep port has been registered by acpi.c, which keeps this module unit
 * testable on a machine without ACPI.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXPW_MAX_REQUESTS   8
#define AXPW_MAX_WAKE       16
#define AXPW_REASON_LEN     24

enum axpw_state
{
    AXPW_STATE_RUNNING = 0,
    AXPW_STATE_IDLE,
    AXPW_STATE_SLEEPING,
    AXPW_STATE_RESUMING,
    AXPW_STATE_SHUTTING,
    AXPW_STATE_RESETTING
};

enum axpw_request
{
    AXPW_REQ_SLEEP = 1,
    AXPW_REQ_SHUTDOWN,
    AXPW_REQ_REBOOT,
    AXPW_REQ_HIBERNATE
};

typedef struct axpw_wake_reason
{
    char   reason[AXPW_REASON_LEN];
    u32    tick;
    u32    state_from;
} axpw_wake_reason_t;

typedef struct axpw_state_machine
{
    u8                state;
    u8                pending_request;
    u32               request_tick;
    u32               sleep_count;
    u32               wake_count;
    u32               rejected;
    u32               capability_mask;
    u64               reset_port;
    u8                reset_value;
    u64               sleep_port_a;
    u64               sleep_value_a;
    axpw_wake_reason_t wake_log[AXPW_MAX_WAKE];
    u8                wake_index;
    u8                wake_count_total;
    bool_t            initialized;
    bool_t            inhibit_sleep;
    char              inhibit_reason[AXPW_REASON_LEN];
} axpw_state_t;

#define AXPW_CAP_SLEEP     (1u << 0)
#define AXPW_CAP_SHUTDOWN  (1u << 1)
#define AXPW_CAP_REBOOT    (1u << 2)
#define AXPW_CAP_HIBERNATE (1u << 3)

static axpw_state_t axpw_state;

static void axpw_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axpw_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static const char *axpw_state_name(u8 state)
{
    switch (state)
    {
    case AXPW_STATE_IDLE: return "idle";
    case AXPW_STATE_SLEEPING: return "sleeping";
    case AXPW_STATE_RESUMING: return "resuming";
    case AXPW_STATE_SHUTTING: return "shutting down";
    case AXPW_STATE_RESETTING: return "resetting";
    default: return "running";
    }
}

static void axpw_remember(const char *reason, u8 from)
{
    axpw_wake_reason_t *slot = &axpw_state.wake_log[axpw_state.wake_index];
    axpw_copy(slot->reason, AXPW_REASON_LEN, reason);
    slot->tick = axpw_state.sleep_count;
    slot->state_from = from;
    axpw_state.wake_index = (axpw_state.wake_index + 1) % AXPW_MAX_WAKE;
    if (axpw_state.wake_count_total < AXPW_MAX_WAKE)
        axpw_state.wake_count_total++;
}

/* -------------------------------------------------------------- public API */

void axpw_init(void)
{
    axpw_zero(&axpw_state, (u32)sizeof(axpw_state));
    axpw_state.state = AXPW_STATE_RUNNING;
    axpw_state.capability_mask = AXPW_CAP_SHUTDOWN | AXPW_CAP_REBOOT;
    axpw_state.initialized = true;
    axpw_state.inhibit_sleep = false;
}

void axpw_register_reset(u64 port, u8 value)
{
    axpw_state.reset_port = port;
    axpw_state.reset_value = value;
    axpw_state.capability_mask |= AXPW_CAP_REBOOT;
}

void axpw_register_sleep(u64 port, u8 value)
{
    axpw_state.sleep_port_a = port;
    axpw_state.sleep_value_a = value;
    axpw_state.capability_mask |= AXPW_CAP_SLEEP | AXPW_CAP_HIBERNATE;
}

u32 axpw_capabilities(void)
{
    return axpw_state.capability_mask;
}

bool_t axpw_can(u8 request)
{
    switch (request)
    {
    case AXPW_REQ_SLEEP: return (axpw_state.capability_mask & AXPW_CAP_SLEEP) != 0;
    case AXPW_REQ_SHUTDOWN: return (axpw_state.capability_mask & AXPW_CAP_SHUTDOWN) != 0;
    case AXPW_REQ_REBOOT: return (axpw_state.capability_mask & AXPW_CAP_REBOOT) != 0;
    case AXPW_REQ_HIBERNATE:
        return (axpw_state.capability_mask & AXPW_CAP_HIBERNATE) != 0;
    default: return false;
    }
}

bool_t axpw_inhibit(bool_t inhibit, const char *reason)
{
    axpw_state.inhibit_sleep = inhibit;
    if (inhibit && reason != NULL_PTR)
        axpw_copy(axpw_state.inhibit_reason, AXPW_REASON_LEN, reason);
    else
        axpw_state.inhibit_reason[0] = 0;
    return axpw_state.inhibit_sleep;
}

bool_t axpw_request(u8 request)
{
    if (!axpw_state.initialized)
        return false;
    if (request < AXPW_REQ_SLEEP || request > AXPW_REQ_HIBERNATE)
        return false;
    if (!axpw_can(request))
    {
        axpw_state.rejected++;
        return false;
    }
    if ((request == AXPW_REQ_SLEEP || request == AXPW_REQ_HIBERNATE) &&
        axpw_state.inhibit_sleep)
    {
        axpw_state.rejected++;
        return false;
    }
    if (axpw_state.state != AXPW_STATE_RUNNING &&
        axpw_state.state != AXPW_STATE_IDLE)
    {
        axpw_state.rejected++;
        return false;
    }
    axpw_state.pending_request = request;
    axpw_state.request_tick = 0;
    return true;
}

bool_t axpw_sleep(void)
{
    return axpw_request(AXPW_REQ_SLEEP);
}

bool_t axpw_shutdown(void)
{
    return axpw_request(AXPW_REQ_SHUTDOWN);
}

bool_t axpw_reboot(void)
{
    return axpw_request(AXPW_REQ_REBOOT);
}

void axpw_resume(const char *reason)
{
    if (axpw_state.state != AXPW_STATE_SLEEPING &&
        axpw_state.state != AXPW_STATE_RESUMING)
        return;
    axpw_state.state = AXPW_STATE_RUNNING;
    axpw_state.wake_count++;
    axpw_remember(reason == NULL_PTR ? "unknown" : reason, AXPW_STATE_SLEEPING);
}

void axpw_idle(void)
{
    if (axpw_state.state == AXPW_STATE_RUNNING)
        axpw_state.state = AXPW_STATE_IDLE;
}

void axpw_busy(void)
{
    if (axpw_state.state == AXPW_STATE_IDLE)
        axpw_state.state = AXPW_STATE_RUNNING;
}

u8 axpw_state_now(void)
{
    return axpw_state.state;
}

bool_t axpw_tick(void)
{
    if (axpw_state.pending_request == 0)
    {
        if (axpw_state.state == AXPW_STATE_IDLE && axpw_state.request_tick++ > 64)
            axpw_state.state = AXPW_STATE_RUNNING;
        return false;
    }
    axpw_state.request_tick++;
    switch (axpw_state.pending_request)
    {
    case AXPW_REQ_SLEEP:
        axpw_state.state = AXPW_STATE_SLEEPING;
        axpw_state.sleep_count++;
        axpw_state.pending_request = 0;
        return true;
    case AXPW_REQ_HIBERNATE:
        axpw_state.state = AXPW_STATE_SLEEPING;
        axpw_state.sleep_count++;
        axpw_state.pending_request = 0;
        return true;
    case AXPW_REQ_SHUTDOWN:
        axpw_state.state = AXPW_STATE_SHUTTING;
        return false;
    case AXPW_REQ_REBOOT:
        axpw_state.state = AXPW_STATE_RESETTING;
        return false;
    default:
        axpw_state.pending_request = 0;
        axpw_state.rejected++;
        return false;
    }
}

bool_t axpw_perform(u8 request)
{
    if (!axpw_request(request))
        return false;
    if (!axpw_tick())
        return false;
    if (request == AXPW_REQ_SHUTDOWN)
    {
        printk("axpw: shutdown requested\n");
        return true;
    }
    if (request == AXPW_REQ_REBOOT)
    {
        printk("axpw: reboot requested port=0x%x value=0x%x\n",
               (unsigned)axpw_state.reset_port, (unsigned)axpw_state.reset_value);
        return true;
    }
    printk("axpw: entering sleep state %s\n", axpw_state_name(axpw_state.state));
    return true;
}

const char *axpw_reason(u8 index)
{
    if (index >= axpw_state.wake_count_total)
        return "";
    return axpw_state.wake_log[index].reason;
}

void axpw_report(void)
{
    u32 i;
    printk("axpw: state=%s caps=0x%x sleeps=%u wakes=%u rejected=%u%s%s\n",
           axpw_state_name(axpw_state.state), (unsigned)axpw_state.capability_mask,
           (unsigned)axpw_state.sleep_count, (unsigned)axpw_state.wake_count,
           (unsigned)axpw_state.rejected,
           axpw_state.inhibit_sleep ? " inhibit=" : "",
           axpw_state.inhibit_sleep ? axpw_state.inhibit_reason : "");
    for (i = 0; i < axpw_state.wake_count_total; i++)
    {
        printk("  wake %u: %s\n", (unsigned)i, axpw_state.wake_log[i].reason);
    }
}

bool_t axpw_selftest(void)
{
    axpw_init();
    if (axpw_capabilities() != (AXPW_CAP_SHUTDOWN | AXPW_CAP_REBOOT))
        return false;
    if (axpw_can(AXPW_REQ_SLEEP))
        return false;
    if (!axpw_can(AXPW_REQ_SHUTDOWN))
        return false;
    if (!axpw_request(AXPW_REQ_SHUTDOWN))
        return false;
    axpw_tick();
    if (axpw_state_now() != AXPW_STATE_SHUTTING)
        return false;
    axpw_init();
    axpw_register_sleep(0x1CF, 0x02);
    if (!axpw_sleep())
        return false;
    axpw_tick();
    if (axpw_state_now() != AXPW_STATE_SLEEPING)
        return false;
    axpw_resume("power button");
    if (axpw_state_now() != AXPW_STATE_RUNNING)
        return false;
    if (axpw_state.wake_count != 1)
        return false;
    if (!axpw_inhibit(true, "installer"))
        return false;
    if (axpw_sleep())
        return false;
    axpw_inhibit(false, NULL_PTR);
    if (!axpw_sleep())
        return false;
    axpw_tick();
    axpw_resume("timer");
    if (axpw_state.wake_count != 2)
        return false;
    return true;
}
