/* AxiomOS - battery and AC adapter power supply abstraction.
 *
 * ACPI exposes both batteries and adapters through the _BST/_BIF methods.  This
 * module keeps the last reading, integrates a coarse energy estimate over time
 * and raises low/critical threshold events that the power manager consumes.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXPS_MAX_BATTERIES   4
#define AXPS_HISTORY         8
#define AXPS_LOW_PERCENT     20
#define AXPS_CRITICAL_PERCENT 5
#define AXPS_FULL_WATTS      65

#define AXPS_STATE_UNKNOWN   0
#define AXPS_STATE_CHARGING  1
#define AXPS_STATE_DISCHARGING 2
#define AXPS_STATE_IDLE      3
#define AXPS_STATE_FULL      4

#define AXPS_EVENT_NONE      0
#define AXPS_EVENT_LOW       1
#define AXPS_EVENT_CRITICAL  2
#define AXPS_EVENT_PLUGGED   3
#define AXPS_EVENT_UNPLUGGED 4
#define AXPS_EVENT_FULL      5

typedef struct axps_battery
{
    u32  percent;
    u32  design_mwh;
    u32  remaining_mwh;
    u32  rate_mw;
    u32  voltage_mv;
    u32  cycles;
    u16  temperature_c;
    u16  health_percent;
    u8   state;
    bool_t present;
    bool_t critical;
    bool_t low;
    u32  history[AXPS_HISTORY];
    u8   history_head;
} axps_battery_t;

typedef struct axps_state
{
    axps_battery_t batteries[AXPS_MAX_BATTERIES];
    u32  battery_count;
    u32  last_event;
    u32  events_raised;
    u32  readings;
    u32  ac_online;
    u32  charge_cycles;
    u32  time_to_empty_min;
    u32  time_to_full_min;
    u32  last_tick;
    bool_t initialized;
} axps_state_t;

static axps_state_t axps_state;

static void axps_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axps_battery_t *axps_battery(u32 index)
{
    if (index >= axps_state.battery_count)
        return NULL_PTR;
    return &axps_state.batteries[index];
}

static void axps_raise(u32 event)
{
    axps_state.last_event = event;
    axps_state.events_raised++;
}

static u32 axps_clamp_percent(u32 value)
{
    return value > 100 ? 100 : value;
}

static void axps_push_history(axps_battery_t *battery, u32 percent)
{
    battery->history[battery->history_head] = percent;
    battery->history_head = (u8)((battery->history_head + 1) % AXPS_HISTORY);
}

static u32 axps_average(const axps_battery_t *battery)
{
    u32 sum = 0;
    u32 i;
    for (i = 0; i < AXPS_HISTORY; i++)
        sum += battery->history[i];
    return sum / AXPS_HISTORY;
}

/* -------------------------------------------------------------- public API */

void axps_init(void)
{
    axps_zero(&axps_state, (u32)sizeof(axps_state));
    axps_state.initialized = true;
}

u32 axps_add_battery(u32 design_mwh, u32 health_percent)
{
    axps_battery_t *battery;
    if (axps_state.battery_count >= AXPS_MAX_BATTERIES)
        return 0xFFFFFFFFu;
    battery = &axps_state.batteries[axps_state.battery_count];
    axps_zero(battery, (u32)sizeof(*battery));
    battery->design_mwh = design_mwh;
    battery->health_percent = (u16)(health_percent > 100 ? 100 : health_percent);
    battery->present = true;
    battery->percent = 100;
    battery->remaining_mwh = design_mwh;
    battery->state = AXPS_STATE_FULL;
    axps_state.battery_count++;
    return axps_state.battery_count - 1;
}

bool_t axps_update(u32 index, u32 remaining_mwh, u32 rate_mw, u32 voltage_mv,
                   u16 temperature_c, bool_t charging)
{
    axps_battery_t *battery = axps_battery(index);
    u32 percent;
    if (battery == NULL_PTR || !battery->present)
        return false;
    battery->remaining_mwh = remaining_mwh;
    battery->rate_mw = rate_mw;
    battery->voltage_mv = voltage_mv;
    battery->temperature_c = temperature_c;
    battery->state = charging ? AXPS_STATE_CHARGING : AXPS_STATE_DISCHARGING;
    percent = battery->design_mwh == 0 ? 0 : (remaining_mwh * 100) / battery->design_mwh;
    battery->percent = axps_clamp_percent(percent);
    axps_push_history(battery, battery->percent);
    axps_state.readings++;
    if (battery->percent <= AXPS_CRITICAL_PERCENT && !battery->critical)
    {
        battery->critical = true;
        axps_raise(AXPS_EVENT_CRITICAL);
    }
    else if (battery->percent <= AXPS_LOW_PERCENT && !battery->low)
    {
        battery->low = true;
        axps_raise(AXPS_EVENT_LOW);
    }
    if (battery->percent >= 100 && !charging)
    {
        battery->state = AXPS_STATE_FULL;
        axps_raise(AXPS_EVENT_FULL);
    }
    if (rate_mw != 0)
    {
        if (charging)
            axps_state.time_to_full_min = (battery->design_mwh - remaining_mwh) * 60 / rate_mw;
        else
            axps_state.time_to_empty_min = remaining_mwh * 60 / rate_mw;
    }
    return true;
}

bool_t axps_set_ac(bool_t online)
{
    if (!axps_state.initialized)
        return false;
    if ((bool_t)axps_state.ac_online == online)
        return true;
    axps_state.ac_online = online ? 1 : 0;
    axps_raise(online ? AXPS_EVENT_PLUGGED : AXPS_EVENT_UNPLUGGED);
    return true;
}

bool_t axps_ac_online(void)
{
    return axps_state.ac_online != 0;
}

u32 axps_percent(u32 index)
{
    axps_battery_t *battery = axps_battery(index);
    return battery == NULL_PTR ? 0 : battery->percent;
}

u32 axps_average_percent(void)
{
    u32 sum = 0;
    u32 count = 0;
    u32 i;
    for (i = 0; i < axps_state.battery_count; i++)
    {
        if (!axps_state.batteries[i].present)
            continue;
        sum += axps_state.batteries[i].percent;
        count++;
    }
    return count == 0 ? 0 : sum / count;
}

u32 axps_estimate_watts(void)
{
    u32 remaining = 0;
    u32 i;
    for (i = 0; i < axps_state.battery_count; i++)
    {
        const axps_battery_t *battery = &axps_state.batteries[i];
        if (!battery->present)
            continue;
        remaining += battery->remaining_mwh;
    }
    /* Remaining milliwatt hours divided by an assumed hour gives an average draw
     * in milliwatts; scale to whole watts for display purposes only. */
    return remaining == 0 ? 0 : (AXPS_FULL_WATTS * remaining) / (remaining + 1000);
}

u32 axps_last_event(void)
{
    return axps_state.last_event;
}

u32 axps_tick(u32 now)
{
    u32 elapsed = now - axps_state.last_tick;
    u32 i;
    axps_state.last_tick = now;
    for (i = 0; i < axps_state.battery_count; i++)
    {
        axps_battery_t *battery = &axps_state.batteries[i];
        if (!battery->present || battery->state != AXPS_STATE_DISCHARGING)
            continue;
        if (battery->rate_mw != 0)
        {
            u32 drained = (battery->rate_mw * elapsed) / 3600;
            if (drained < battery->remaining_mwh)
                battery->remaining_mwh -= drained;
            else
                battery->remaining_mwh = 0;
        }
    }
    return axps_average_percent();
}

bool_t axps_health(u32 index, u16 *health, u32 *cycles)
{
    axps_battery_t *battery = axps_battery(index);
    if (battery == NULL_PTR)
        return false;
    if (health != NULL_PTR)
        *health = battery->health_percent;
    if (cycles != NULL_PTR)
        *cycles = battery->cycles;
    return true;
}

u32 axps_average_smoothed(u32 index)
{
    axps_battery_t *battery = axps_battery(index);
    if (battery == NULL_PTR)
        return 0;
    return axps_average(battery);
}

void axps_report(void)
{
    u32 i;
    printk("axps: batteries=%u ac=%u avg=%u%% event=%u readings=%u tte=%umin ttf=%umin\n",
           (unsigned)axps_state.battery_count, (unsigned)axps_state.ac_online,
           (unsigned)axps_average_percent(), (unsigned)axps_state.last_event,
           (unsigned)axps_state.readings, (unsigned)axps_state.time_to_empty_min,
           (unsigned)axps_state.time_to_full_min);
    for (i = 0; i < axps_state.battery_count; i++)
    {
        const axps_battery_t *battery = &axps_state.batteries[i];
        printk("  bat %u pct=%u mwh=%u rate=%u state=%u temp=%d health=%u\n",
               (unsigned)i, (unsigned)battery->percent, (unsigned)battery->remaining_mwh,
               (unsigned)battery->rate_mw, (unsigned)battery->state,
               (int)battery->temperature_c, (unsigned)battery->health_percent);
    }
}

bool_t axps_selftest(void)
{
    u32 index;
    axps_init();
    index = axps_add_battery(50000, 92);
    if (index != 0)
        return false;
    if (!axps_update(0, 40000, 10000, 12000, 31, false))
        return false;
    if (axps_percent(0) != 80)
        return false;
    if (!axps_set_ac(false))
        return false;
    if (axps_last_event() != AXPS_EVENT_UNPLUGGED)
        return false;
    if (!axps_update(0, 9000, 10000, 11500, 33, false))
        return false;
    if (axps_last_event() != AXPS_EVENT_CRITICAL)
        return false;
    if (!axps_update(0, 30000, 12000, 12500, 30, true))
        return false;
    if (axps_state.time_to_full_min == 0)
        return false;
    if (axps_average_percent() != 60)
        return false;
    if (axps_tick(3600) != 58 && axps_tick(3600) != 59)
        return false;
    {
        u16 health = 0;
        u32 cycles = 0;
        if (!axps_health(0, &health, &cycles))
            return false;
        if (health != 92)
            return false;
    }
    return true;
}
