/* AxiomOS - embedded controller and GPE handling.
 *
 * The embedded controller is a two byte register file behind an I/O port pair
 * (command/status and data).  Firmware transactions poll the status register
 * for the output buffer full and input buffer empty bits.  This module models
 * that protocol, adds a small transaction queue and a GPE dispatch table for
 * the ACPI fixed events.
 */

#include <axiom/kernel.h>

#define AXEC_MAX_TRANSACTIONS  32
#define AXEC_MAX_GPE           16
#define AXEC_TIMEOUT_SPINS     1024

#define AXEC_STATUS_OBF        0x01
#define AXEC_STATUS_IBF        0x02
#define AXEC_STATUS_SCI        0x20
#define AXEC_STATUS_BURST      0x10

typedef struct axec_transaction
{
    u8   address;
    u8   value;
    u8   kind;          /* 0 read, 1 write */
    u8   state;         /* 0 queued, 1 running, 2 done, 3 failed */
    u32  spins;
} axec_transaction_t;

typedef struct axec_gpe
{
    u16 number;
    u32 events;
    u32 handled;
    bool_t active;
    char name[16];
} axec_gpe_t;

typedef struct axec_controller
{
    u16  command_port;
    u16  data_port;
    axec_transaction_t queue[AXEC_MAX_TRANSACTIONS];
    u32  head;
    u32  tail;
    u32  count;
    axec_gpe_t gpes[AXEC_MAX_GPE];
    u32  gpe_count;
    u8   status;
    u8   data;
    u32  transactions;
    u32  failures;
    u32  timeouts;
    bool_t burst_mode;
    bool_t initialised;
    u32  sci_count;
} axec_controller_t;

static void axec_zero(void *ptr, u32 size);
static u32  axec_next_index(u32 value);
static void axec_push(axec_controller_t *ec, u8 address, u8 kind, u8 value);
static bool_t axec_pop(axec_controller_t *ec, axec_transaction_t *out);
static void axec_complete(axec_controller_t *ec, u8 address, u8 value);
static void axec_poll(axec_controller_t *ec);
static u8   axec_read(axec_controller_t *ec, u8 address, bool_t *ok);
static bool_t axec_write(axec_controller_t *ec, u8 address, u8 value);
static void axec_add_gpe(axec_controller_t *ec, u16 number, const char *name);
static u32  axec_raise_gpe(axec_controller_t *ec, u16 number);
static const char *axec_gpe_name(const axec_controller_t *ec, u16 number);

static void axec_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axec_next_index(u32 value)
{
    return (value + 1) % AXEC_MAX_TRANSACTIONS;
}

static void axec_push(axec_controller_t *ec, u8 address, u8 kind, u8 value)
{
    axec_transaction_t *slot;
    if (ec->count >= AXEC_MAX_TRANSACTIONS)
    {
        ec->failures++;
        return;
    }
    slot = &ec->queue[ec->tail];
    slot->address = address;
    slot->value = value;
    slot->kind = kind;
    slot->state = 0;
    slot->spins = 0;
    ec->tail = axec_next_index(ec->tail);
    ec->count++;
}

static bool_t axec_pop(axec_controller_t *ec, axec_transaction_t *out)
{
    if (ec->count == 0)
        return false;
    *out = ec->queue[ec->head];
    ec->head = axec_next_index(ec->head);
    ec->count--;
    return true;
}

static void axec_complete(axec_controller_t *ec, u8 address, u8 value)
{
    ec->transactions++;
    ec->status = (u8)(ec->status & ~AXEC_STATUS_OBF);
    if (address == 0x00)
        ec->data = value;
}

static void axec_poll(axec_controller_t *ec)
{
    u32 budget = AXEC_TIMEOUT_SPINS;
    while (ec->count > 0 && budget > 0)
    {
        axec_transaction_t *slot = &ec->queue[ec->head];
        if (slot->state == 0)
            slot->state = 1;
        if (slot->state == 1)
        {
            if (slot->kind == 1)
            {
                axec_complete(ec, slot->address, slot->value);
                slot->state = 2;
            }
            else
            {
                ec->status = (u8)(ec->status | AXEC_STATUS_OBF);
                axec_complete(ec, slot->address, ec->data);
                slot->state = 2;
            }
        }
        if (slot->state == 2)
        {
            axec_pop(ec, &slot[0]);
            continue;
        }
        slot->spins++;
        if (slot->spins > AXEC_TIMEOUT_SPINS)
        {
            slot->state = 3;
            ec->timeouts++;
            ec->failures++;
            axec_pop(ec, &slot[0]);
            continue;
        }
        budget--;
    }
}

static u8 axec_read(axec_controller_t *ec, u8 address, bool_t *ok)
{
    axec_push(ec, address, 0, 0);
    axec_poll(ec);
    if (ec->count == 0 && ec->status == 0)
    {
        *ok = false;
        return 0;
    }
    *ok = true;
    return ec->data;
}

static bool_t axec_write(axec_controller_t *ec, u8 address, u8 value)
{
    axec_push(ec, address, 1, value);
    axec_poll(ec);
    return (bool_t)(ec->count == 0);
}

static const char *axec_gpe_name(const axec_controller_t *ec, u16 number)
{
    u32 i;
    for (i = 0; i < ec->gpe_count; i++)
    {
        if (ec->gpes[i].number == number)
            return ec->gpes[i].name;
    }
    return "unknown";
}

static void axec_add_gpe(axec_controller_t *ec, u16 number, const char *name)
{
    axec_gpe_t *gpe;
    u32 i;
    if (ec->gpe_count >= AXEC_MAX_GPE)
        return;
    gpe = &ec->gpes[ec->gpe_count];
    gpe->number = number;
    gpe->events = 0;
    gpe->handled = 0;
    gpe->active = true;
    for (i = 0; i < 15 && name[i] != 0; i++)
        gpe->name[i] = name[i];
    gpe->name[i] = 0;
    ec->gpe_count++;
}

static u32 axec_raise_gpe(axec_controller_t *ec, u16 number)
{
    u32 i;
    for (i = 0; i < ec->gpe_count; i++)
    {
        if (ec->gpes[i].number != number || !ec->gpes[i].active)
            continue;
        ec->gpes[i].events++;
        ec->gpes[i].handled++;
        ec->sci_count++;
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------- public API */

void axec_init(axec_controller_t *ec, u16 command_port, u16 data_port)
{
    axec_zero(ec, (u32)sizeof(*ec));
    ec->command_port = command_port;
    ec->data_port = data_port;
    ec->initialised = true;
    axec_add_gpe(ec, 0x0010, "lid");
    axec_add_gpe(ec, 0x0018, "battery");
    axec_add_gpe(ec, 0x0020, "thermal");
}

u8 axec_reg(const axec_controller_t *ec, u8 address)
{
    if (address == 0x00)
        return ec->data;
    if (address == 0x01)
        return ec->status;
    return 0;
}

void axec_tx(axec_controller_t *ec, u8 address, u8 value)
{
    axec_write(ec, address, value);
}

u8 axec_rx(axec_controller_t *ec, u8 address, bool_t *ok)
{
    return axec_read(ec, address, ok);
}

void axec_set_burst(axec_controller_t *ec, bool_t enabled)
{
    ec->burst_mode = enabled;
    if (enabled)
        ec->status = (u8)(ec->status | AXEC_STATUS_BURST);
    else
        ec->status = (u8)(ec->status & ~AXEC_STATUS_BURST);
}

bool_t axec_self_test(axec_controller_t *ec)
{
    bool_t ok = false;
    u8 data;
    axec_tx(ec, 0x00, 0x5A);
    data = axec_reg(ec, 0x00);
    if (data != 0x5A)
        return false;
    data = axec_rx(ec, 0x00, &ok);
    if (!ok)
        return false;
    return true;
}

u32 axec_pending(const axec_controller_t *ec)
{
    return ec->count;
}

u32 axec_handle_sci(axec_controller_t *ec)
{
    u32 handled = 0;
    if ((ec->status & AXEC_STATUS_SCI) == 0)
        return 0;
    ec->status = (u8)(ec->status & ~AXEC_STATUS_SCI);
    ec->sci_count++;
    {
        u32 i;
        for (i = 0; i < ec->gpe_count; i++)
        {
            if (!ec->gpes[i].active)
                continue;
            ec->gpes[i].handled++;
            handled++;
        }
    }
    return handled;
}

u32 axec_raise(axec_controller_t *ec, u16 number)
{
    return axec_raise_gpe(ec, number);
}

u32 axec_mask(axec_controller_t *ec, u16 number)
{
    u32 i;
    for (i = 0; i < ec->gpe_count; i++)
    {
        if (ec->gpes[i].number == number)
        {
            ec->gpes[i].active = false;
            return 1;
        }
    }
    return 0;
}

u32 axec_unmask(axec_controller_t *ec, u16 number)
{
    u32 i;
    for (i = 0; i < ec->gpe_count; i++)
    {
        if (ec->gpes[i].number == number)
        {
            ec->gpes[i].active = true;
            return 1;
        }
    }
    return 0;
}

u16 axec_thermal_zone(const axec_controller_t *ec)
{
    AXIOM_UNUSED_VAR(ec);
    return 0x1C;
}

void axec_dump(const axec_controller_t *ec)
{
    u32 i;
    printk("ec: ports %04x/%04x tx=%u fail=%u timeout=%u sci=%u\n",
           (unsigned)ec->command_port, (unsigned)ec->data_port,
           (unsigned)ec->transactions, (unsigned)ec->failures,
           (unsigned)ec->timeouts, (unsigned)ec->sci_count);
    for (i = 0; i < ec->gpe_count; i++)
    {
        printk("ec: gpe %04x %-8s events=%u\n", (unsigned)ec->gpes[i].number,
               ec->gpes[i].name, (unsigned)ec->gpes[i].events);
    }
}

bool_t axec_selftest(void)
{
    static axec_controller_t ec;
    bool_t ok = false;
    axec_init(&ec, 0x62, 0x66);
    if (!axec_self_test(&ec))
        return false;
    if (ec.transactions == 0)
        return false;
    axec_set_burst(&ec, true);
    if ((ec.status & AXEC_STATUS_BURST) == 0)
        return false;
    axec_raise(&ec, 0x0010);
    if (ec.gpes[0].events == 0)
        return false;
    if (axec_mask(&ec, 0x0010) != 1)
        return false;
    axec_unmask(&ec, 0x0010);
    if (!axec_rx(&ec, 0x00, &ok))
    {
        if (!ok)
            return false;
    }
    return true;
}
