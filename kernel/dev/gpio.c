/* AxiomOS - GPIO / pin control abstraction.
 *
 * Boards describe pins as a flat table of controller/pin pairs.  A controller
 * exposes direction, level, pull and alternate-function configuration together
 * with an interrupt trigger mask.  The core keeps a shadow copy of every pin so
 * read-modify-write sequences do not depend on the hardware register layout and
 * can be replayed after a suspend/resume cycle.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXGP_MAX_CONTROLLERS  8
#define AXGP_PINS_PER_BANK    32
#define AXGP_MAX_EVENTS       32
#define AXGP_DEBOUNCE_HZ      1000
#define AXGP_DEFAULT_PULL     0
#define AXGP_MAX_FUNCTIONS    8

#define AXGP_DIR_INPUT        0
#define AXGP_DIR_OUTPUT       1
#define AXGP_DIR_ALT          2

#define AXGP_PULL_NONE        0
#define AXGP_PULL_UP          1
#define AXGP_PULL_DOWN        2

#define AXGP_TRIG_NONE        0
#define AXGP_TRIG_RISING      1
#define AXGP_TRIG_FALLING     2
#define AXGP_TRIG_BOTH        3
#define AXGP_TRIG_HIGH        4
#define AXGP_TRIG_LOW         5

typedef struct axgp_event
{
    u32  stamp_ms;
    u16  pin;
    u8   bank;
    u8   level;
    bool_t consumed;
} axgp_event_t;

typedef struct axgp_pin
{
    u8   direction;
    u8   pull;
    u8   trigger;
    u8   function;
    u8   level;
    u8   drive_strength;
    u8   debounce_ms;
    u16  label;
    u32  changes;
    u32  interrupts;
    bool_t claimed;
    bool_t open_drain;
    bool_t schmitt;
    bool_t wake_capable;
} axgp_pin_t;

typedef struct axgp_controller
{
    axgp_pin_t pins[AXGP_PINS_PER_BANK];
    u32  base_pin;
    u32  bank;
    u32  regs;
    u32  claimed_count;
    u32  interrupt_count;
    u32  writes;
    u8   function_count;
    bool_t present;
    bool_t irq_enabled;
} axgp_controller_t;

typedef struct axgp_state
{
    axgp_controller_t controllers[AXGP_MAX_CONTROLLERS];
    axgp_event_t events[AXGP_MAX_EVENTS];
    u32  controller_count;
    u32  event_head;
    u32  event_tail;
    u32  event_dropped;
    u32  total_claims;
    u32  total_writes;
    u32  now_ms;
    bool_t initialized;
} axgp_state_t;

static axgp_state_t axgp_state;

static void axgp_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axgp_controller_t *axgp_controller(u32 bank)
{
    for (u32 i = 0; i < axgp_state.controller_count; i++)
    {
        if (axgp_state.controllers[i].present && axgp_state.controllers[i].bank == bank)
            return &axgp_state.controllers[i];
    }
    return NULL_PTR;
}

static axgp_pin_t *axgp_pin(u32 bank, u8 pin)
{
    axgp_controller_t *controller = axgp_controller(bank);
    if (controller == NULL_PTR || pin >= AXGP_PINS_PER_BANK)
        return NULL_PTR;
    return &controller->pins[pin];
}

static void axgp_push_event(u32 bank, u8 pin, u8 level)
{
    axgp_event_t *event;
    u32 next = (axgp_state.event_head + 1) % AXGP_MAX_EVENTS;
    if (next == axgp_state.event_tail)
    {
        axgp_state.event_dropped++;
        return;
    }
    event = &axgp_state.events[axgp_state.event_head];
    event->stamp_ms = axgp_state.now_ms;
    event->bank = (u8)bank;
    event->pin = pin;
    event->level = level;
    event->consumed = false;
    axgp_state.event_head = next;
}

/* -------------------------------------------------------------- public API */

void axgp_init(void)
{
    axgp_zero(&axgp_state, (u32)sizeof(axgp_state));
    axgp_state.initialized = true;
}

bool_t axgp_register_bank(u32 bank, u32 base_pin, u32 regs, u8 function_count)
{
    axgp_controller_t *controller;
    if (!axgp_state.initialized || axgp_state.controller_count >= AXGP_MAX_CONTROLLERS)
        return false;
    if (axgp_controller(bank) != NULL_PTR)
        return false;
    controller = &axgp_state.controllers[axgp_state.controller_count];
    axgp_zero(controller, (u32)sizeof(*controller));
    controller->bank = bank;
    controller->base_pin = base_pin;
    controller->regs = regs;
    controller->function_count = function_count > AXGP_MAX_FUNCTIONS ? AXGP_MAX_FUNCTIONS : function_count;
    controller->present = true;
    for (u32 i = 0; i < AXGP_PINS_PER_BANK; i++)
    {
        controller->pins[i].direction = AXGP_DIR_INPUT;
        controller->pins[i].pull = AXGP_DEFAULT_PULL;
        controller->pins[i].debounce_ms = 2;
        controller->pins[i].wake_capable = (i % 8) == 0;
    }
    axgp_state.controller_count++;
    return true;
}

bool_t axgp_claim(u32 bank, u8 pin, u16 label)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    if (target == NULL_PTR)
        return false;
    if (target->claimed && target->label != label)
        return false;
    target->claimed = true;
    target->label = label;
    axgp_state.total_claims++;
    return true;
}

bool_t axgp_release(u32 bank, u8 pin)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    if (target == NULL_PTR || !target->claimed)
        return false;
    target->claimed = false;
    target->label = 0;
    target->direction = AXGP_DIR_INPUT;
    target->trigger = AXGP_TRIG_NONE;
    return true;
}

bool_t axgp_set_direction(u32 bank, u8 pin, u8 direction)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    axgp_controller_t *controller = axgp_controller(bank);
    if (target == NULL_PTR || direction > AXGP_DIR_ALT)
        return false;
    if (direction == AXGP_DIR_OUTPUT && target->open_drain && target->level)
        return false;
    target->direction = direction;
    target->changes++;
    controller->writes++;
    axgp_state.total_writes++;
    return true;
}

bool_t axgp_set_level(u32 bank, u8 pin, u8 level)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    axgp_controller_t *controller = axgp_controller(bank);
    if (target == NULL_PTR)
        return false;
    if (target->direction != AXGP_DIR_OUTPUT)
        return false;
    level = level ? 1 : 0;
    if (target->level == level)
        return true;
    target->level = level;
    target->changes++;
    controller->writes++;
    axgp_state.total_writes++;
    return true;
}

bool_t axgp_get_level(u32 bank, u8 pin, u8 *level)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    if (target == NULL_PTR || level == NULL_PTR)
        return false;
    *level = target->level;
    return true;
}

bool_t axgp_toggle(u32 bank, u8 pin)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    if (target == NULL_PTR || target->direction != AXGP_DIR_OUTPUT)
        return false;
    return axgp_set_level(bank, pin, target->level ? 0 : 1);
}

bool_t axgp_set_pull(u32 bank, u8 pin, u8 pull)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    if (target == NULL_PTR || pull > AXGP_PULL_DOWN)
        return false;
    target->pull = pull;
    target->changes++;
    return true;
}

bool_t axgp_set_function(u32 bank, u8 pin, u8 function)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    axgp_controller_t *controller = axgp_controller(bank);
    if (target == NULL_PTR || controller == NULL_PTR)
        return false;
    if (function >= controller->function_count)
        return false;
    target->function = function;
    target->direction = AXGP_DIR_ALT;
    return true;
}

bool_t axgp_set_trigger(u32 bank, u8 pin, u8 trigger)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    if (target == NULL_PTR || trigger > AXGP_TRIG_LOW)
        return false;
    target->trigger = trigger;
    return true;
}

bool_t axgp_set_debounce(u32 bank, u8 pin, u8 milliseconds)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    if (target == NULL_PTR || milliseconds > 100)
        return false;
    target->debounce_ms = milliseconds;
    return true;
}

bool_t axgp_configure(u32 bank, u8 pin, u8 direction, u8 pull, u8 trigger, u8 function)
{
    if (!axgp_set_direction(bank, pin, direction))
        return false;
    if (!axgp_set_pull(bank, pin, pull))
        return false;
    if (!axgp_set_trigger(bank, pin, trigger))
        return false;
    if (direction == AXGP_DIR_ALT && !axgp_set_function(bank, pin, function))
        return false;
    return true;
}

/* Edge stimulus used by the simulated controller and by tests. */
bool_t axgp_stimulus(u32 bank, u8 pin, u8 level)
{
    axgp_pin_t *target = axgp_pin(bank, pin);
    axgp_controller_t *controller = axgp_controller(bank);
    bool_t fire = false;
    if (target == NULL_PTR || controller == NULL_PTR)
        return false;
    level = level ? 1 : 0;
    if (target->level == level)
        return true;
    switch (target->trigger)
    {
    case AXGP_TRIG_RISING: fire = level == 1; break;
    case AXGP_TRIG_FALLING: fire = level == 0; break;
    case AXGP_TRIG_BOTH: fire = true; break;
    case AXGP_TRIG_HIGH: fire = level == 1; break;
    case AXGP_TRIG_LOW: fire = level == 0; break;
    default: fire = false; break;
    }
    target->level = level;
    if (fire && controller->irq_enabled && target->claimed)
    {
        target->interrupts++;
        controller->interrupt_count++;
        axgp_push_event(bank, pin, level);
    }
    return true;
}

bool_t axgp_read_event(axgp_event_t *out)
{
    if (out == NULL_PTR || axgp_state.event_tail == axgp_state.event_head)
        return false;
    *out = axgp_state.events[axgp_state.event_tail];
    axgp_state.events[axgp_state.event_tail].consumed = true;
    axgp_state.event_tail = (axgp_state.event_tail + 1) % AXGP_MAX_EVENTS;
    return true;
}

u32 axgp_pending_events(void)
{
    return (axgp_state.event_head + AXGP_MAX_EVENTS - axgp_state.event_tail) % AXGP_MAX_EVENTS;
}

void axgp_enable_irq(u32 bank, bool_t enable)
{
    axgp_controller_t *controller = axgp_controller(bank);
    if (controller != NULL_PTR)
        controller->irq_enabled = enable;
}

u32 axgp_controller_count(void)
{
    return axgp_state.controller_count;
}

u32 axgp_claimed_count(u32 bank)
{
    axgp_controller_t *controller = axgp_controller(bank);
    return controller == NULL_PTR ? 0 : controller->claimed_count;
}

void axgp_snapshot(u32 bank, u32 *directions, u32 *levels, u32 capacity)
{
    axgp_controller_t *controller = axgp_controller(bank);
    if (controller == NULL_PTR || directions == NULL_PTR || levels == NULL_PTR)
        return;
    for (u32 i = 0; i < capacity && i < AXGP_PINS_PER_BANK; i++)
    {
        directions[i] = controller->pins[i].direction;
        levels[i] = controller->pins[i].level;
    }
}

u32 axgp_save_state(u8 *buffer, u32 capacity)
{
    u32 offset = 0;
    if (buffer == NULL_PTR)
        return 0;
    for (u32 c = 0; c < axgp_state.controller_count && offset + 8 <= capacity; c++)
    {
        axgp_controller_t *controller = &axgp_state.controllers[c];
        if (!controller->present)
            continue;
        buffer[offset++] = (u8)controller->bank;
        buffer[offset++] = 0;
        buffer[offset++] = 0;
        buffer[offset++] = 0;
        for (u32 p = 0; p < AXGP_PINS_PER_BANK && offset + 4 <= capacity; p++)
        {
            axgp_pin_t *pin = &controller->pins[p];
            buffer[offset++] = pin->direction;
            buffer[offset++] = pin->pull;
            buffer[offset++] = pin->level;
            buffer[offset++] = (u8)pin->function;
        }
    }
    return offset;
}

void axgp_report(void)
{
    printk("axgp: controllers=%u claims=%u writes=%u events=%u dropped=%u\n",
           (unsigned)axgp_state.controller_count, (unsigned)axgp_state.total_claims,
           (unsigned)axgp_state.total_writes, (unsigned)axgp_pending_events(),
           (unsigned)axgp_state.event_dropped);
    for (u32 i = 0; i < axgp_state.controller_count; i++)
    {
        axgp_controller_t *controller = &axgp_state.controllers[i];
        if (!controller->present)
            continue;
        printk("  bank %u base=%u functions=%u interrupts=%u\n",
               (unsigned)controller->bank, (unsigned)controller->base_pin,
               (unsigned)controller->function_count, (unsigned)controller->interrupt_count);
    }
}

bool_t axgp_selftest(void)
{
    axgp_event_t event;
    u8 level = 0;
    u32 directions[4];
    u32 levels[4];
    u8 snapshot[64];
    axgp_init();
    if (!axgp_register_bank(0, 0, 0x400, 4))
        return false;
    if (axgp_register_bank(0, 0, 0, 0))
        return false;
    if (!axgp_claim(0, 3, 0x100))
        return false;
    if (axgp_claim(0, 3, 0x200))
        return false;
    if (!axgp_set_direction(0, 3, AXGP_DIR_OUTPUT))
        return false;
    if (!axgp_set_level(0, 3, 1))
        return false;
    if (!axgp_get_level(0, 3, &level))
        return false;
    if (level != 1)
        return false;
    if (!axgp_toggle(0, 3))
        return false;
    if (!axgp_get_level(0, 3, &level))
        return false;
    if (level != 0)
        return false;
    if (axgp_set_level(0, 7, 1))
        return false;
    if (!axgp_claim(0, 5, 0x300))
        return false;
    if (!axgp_set_trigger(0, 5, AXGP_TRIG_BOTH))
        return false;
    axgp_enable_irq(0, true);
    axgp_stimulus(0, 5, 0);
    axgp_stimulus(0, 5, 1);
    axgp_stimulus(0, 5, 1);
    if (axgp_pending_events() != 2)
        return false;
    if (!axgp_read_event(&event))
        return false;
    if (event.pin != 5)
        return false;
    if (!axgp_read_event(&event))
        return false;
    if (axgp_read_event(&event))
        return false;
    axgp_snapshot(0, directions, levels, 4);
    if (directions[3] != AXGP_DIR_OUTPUT)
        return false;
    if (axgp_save_state(snapshot, 64) == 0)
        return false;
    if (!axgp_set_debounce(0, 3, 5))
        return false;
    if (axgp_set_debounce(0, 3, 200))
        return false;
    if (!axgp_release(0, 3))
        return false;
    if (axgp_set_level(0, 3, 1))
        return false;
    return true;
}
