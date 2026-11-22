/* AxiomOS - SDHCI host controller driver for eMMC and SD cards.
 *
 * The controller exposes a register window with a command/response protocol on
 * top of it.  This module models identification, block size selection, single
 * and multi block transfers, and the error recovery path (reset command line,
 * reset data line, reset all).
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXSD_SLOT_COUNT      2
#define AXSD_MAX_BLOCKS      64
#define AXSD_BLOCK_SIZE      512
#define AXSD_CMD_TIMEOUT     1000
#define AXSD_RETRY_LIMIT     3

#define AXSD_REG_SDMA         0x00
#define AXSD_REG_BLKSIZE      0x04
#define AXSD_REG_BLOCKCOUNT   0x06
#define AXSD_REG_ARGUMENT     0x08
#define AXSD_REG_TRANSFER     0x0C
#define AXSD_REG_COMMAND      0x0E
#define AXSD_REG_RESPONSE     0x10
#define AXSD_REG_BUFFER       0x20
#define AXSD_REG_PRESENT      0x24
#define AXSD_REG_HOSTCTRL     0x28
#define AXSD_REG_CLOCKCTRL    0x2C
#define AXSD_REG_TIMEOUTCTRL  0x2E
#define AXSD_REG_SOFTWARE     0x2F
#define AXSD_REG_NORMAL_IS    0x30
#define AXSD_REG_ERROR_IS     0x32
#define AXSD_REG_NORMAL_IE    0x34
#define AXSD_REG_ERROR_IE     0x36
#define AXSD_REG_SIGNAL_IE    0x38
#define AXSD_REG_ACMD12_ERR   0x3C
#define AXSD_REG_CAPS         0x40

#define AXSD_INT_CMD_COMPLETE 0x0001
#define AXSD_INT_TRANS_COMPL  0x0002
#define AXSD_INT_BUF_WR_READY 0x0004
#define AXSD_INT_BUF_RD_READY 0x0008
#define AXSD_INT_CARD_INSERT  0x0040
#define AXSD_INT_CARD_REMOVE  0x0080

#define AXSD_ERR_CMD_TIMEOUT  0x0001
#define AXSD_ERR_CMD_CRC      0x0002
#define AXSD_ERR_CMD_END_BIT  0x0004
#define AXSD_ERR_CMD_INDEX    0x0008
#define AXSD_ERR_DATA_TIMEOUT 0x0010
#define AXSD_ERR_DATA_CRC     0x0020
#define AXSD_ERR_DATA_END_BIT 0x0040
#define AXSD_ERR_CURRENT_LIMIT 0x0080
#define AXSD_ERR_AUTO_CMD12   0x0100
#define AXSD_ERR_ADMA          0x0200

#define AXSD_CMD_GO_IDLE      0
#define AXSD_CMD_ALL_SEND_CID 2
#define AXSD_CMD_SEND_RCA     3
#define AXSD_CMD_SELECT_CARD  7
#define AXSD_CMD_SEND_IF_COND 8
#define AXSD_CMD_SEND_CSD     9
#define AXSD_CMD_READ_SINGLE  17
#define AXSD_CMD_READ_MULTI   18
#define AXSD_CMD_WRITE_SINGLE 24
#define AXSD_CMD_WRITE_MULTI  25
#define AXSD_CMD_APP_CMD      55

#define AXSD_CARD_NONE        0
#define AXSD_CARD_SD          1
#define AXSD_CARD_MMC         2

typedef struct axsd_card
{
    u32  rca;
    u32  capacity_blocks;
    u32  block_size;
    u32  cid[4];
    u16  vendor;
    u8   type;
    u8   bus_width;
    bool_t high_capacity;
    bool_t present;
    bool_t write_protected;
} axsd_card_t;

typedef struct axsd_slot
{
    axsd_card_t card;
    u32  base;
    u32  command_count;
    u32  data_transfers;
    u32  blocks_read;
    u32  blocks_written;
    u32  timeouts;
    u32  crc_errors;
    u32  resets;
    u32  retries;
    u32  normal_int;
    u32  error_int;
    u16  clock_khz;
    u8   voltage;
    bool_t initialized;
    bool_t busy;
} axsd_slot_t;

typedef struct axsd_state
{
    axsd_slot_t slots[AXSD_SLOT_COUNT];
    u32  slot_count;
    u32  total_blocks_read;
    u32  total_blocks_written;
    u32  interrupts;
    bool_t ready;
} axsd_state_t;

static axsd_state_t axsd_state;

static void axsd_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axsd_slot_t *axsd_slot(u32 index)
{
    if (index >= axsd_state.slot_count)
        return NULL_PTR;
    return &axsd_state.slots[index];
}

static void axsd_clear_errors(axsd_slot_t *slot)
{
    slot->error_int = 0;
}

static bool_t axsd_command(axsd_slot_t *slot, u8 command, u32 argument, u32 *response)
{
    if (slot == NULL_PTR || !slot->initialized)
        return false;
    slot->command_count++;
    slot->busy = true;
    switch (command)
    {
    case AXSD_CMD_GO_IDLE:
        slot->card.type = AXSD_CARD_NONE;
        slot->card.present = false;
        slot->normal_int |= AXSD_INT_CMD_COMPLETE;
        break;
    case AXSD_CMD_SEND_IF_COND:
        if (response != NULL_PTR)
            *response = 0x1AA;
        break;
    case AXSD_CMD_ALL_SEND_CID:
        slot->card.present = true;
        slot->card.type = AXSD_CARD_SD;
        slot->card.vendor = 0x0003;
        slot->card.cid[0] = 0x03534453;
        slot->card.cid[1] = 0x4D313238;
        slot->card.cid[2] = 0x4713A1B2;
        slot->card.cid[3] = 0x0001D2C3;
        if (response != NULL_PTR)
            *response = slot->card.cid[0];
        break;
    case AXSD_CMD_SEND_RCA:
        slot->card.rca = 0x0001;
        if (response != NULL_PTR)
            *response = 0x00010000u;
        break;
    case AXSD_CMD_SELECT_CARD:
        slot->card.block_size = AXSD_BLOCK_SIZE;
        slot->card.high_capacity = true;
        slot->card.capacity_blocks = 0x00760000u;
        slot->card.bus_width = 4;
        if (response != NULL_PTR)
            *response = 0x00010000u;
        break;
    case AXSD_CMD_SEND_CSD:
        if (response != NULL_PTR)
            *response = 0x400E0032u;
        break;
    case AXSD_CMD_APP_CMD:
        if (response != NULL_PTR)
            *response = 0x00010000u;
        break;
    default:
        slot->timeouts++;
        slot->error_int |= AXSD_ERR_CMD_INDEX;
        slot->busy = false;
        return false;
    }
    slot->normal_int |= AXSD_INT_CMD_COMPLETE;
    slot->busy = false;
    return true;
}

/* -------------------------------------------------------------- public API */

bool_t axsd_register_slot(u32 base, u16 clock_khz, u8 voltage)
{
    axsd_slot_t *slot;
    if (axsd_state.slot_count >= AXSD_SLOT_COUNT)
        return false;
    slot = &axsd_state.slots[axsd_state.slot_count];
    axsd_zero(slot, (u32)sizeof(*slot));
    slot->base = base;
    slot->clock_khz = clock_khz;
    slot->voltage = voltage;
    axsd_state.slot_count++;
    return true;
}

bool_t axsd_reset(u32 index, u8 type)
{
    axsd_slot_t *slot = axsd_slot(index);
    if (slot == NULL_PTR)
        return false;
    switch (type)
    {
    case 0:
        axsd_clear_errors(slot);
        break;
    case 1:
        axsd_clear_errors(slot);
        slot->busy = false;
        break;
    case 2:
        axsd_zero(&slot->card, (u32)sizeof(slot->card));
        axsd_clear_errors(slot);
        slot->initialized = false;
        break;
    default:
        return false;
    }
    slot->resets++;
    return true;
}

bool_t axsd_init_slot(u32 index)
{
    axsd_slot_t *slot = axsd_slot(index);
    u32 response = 0;
    if (slot == NULL_PTR)
        return false;
    axsd_reset(index, 2);
    if (!axsd_command(slot, AXSD_CMD_GO_IDLE, 0, &response))
        return false;
    if (!axsd_command(slot, AXSD_CMD_SEND_IF_COND, 0x1AA, &response))
        return false;
    if (!axsd_command(slot, AXSD_CMD_ALL_SEND_CID, 0, &response))
        return false;
    if (!axsd_command(slot, AXSD_CMD_SEND_RCA, 0, &response))
        return false;
    if (!axsd_command(slot, AXSD_CMD_SELECT_CARD, 0x00010000u, &response))
        return false;
    slot->initialized = true;
    return true;
}

bool_t axsd_card_present(u32 index)
{
    axsd_slot_t *slot = axsd_slot(index);
    if (slot == NULL_PTR)
        return false;
    return slot->initialized && slot->card.present;
}

u32 axsd_capacity_blocks(u32 index)
{
    axsd_slot_t *slot = axsd_slot(index);
    if (slot == NULL_PTR || !slot->card.present)
        return 0;
    return slot->card.capacity_blocks;
}

u8 axsd_card_type(u32 index)
{
    axsd_slot_t *slot = axsd_slot(index);
    if (slot == NULL_PTR)
        return AXSD_CARD_NONE;
    return slot->card.type;
}

bool_t axsd_set_block_size(u32 index, u32 size)
{
    axsd_slot_t *slot = axsd_slot(index);
    if (slot == NULL_PTR || size != 512 || !slot->card.present)
        return false;
    slot->card.block_size = size;
    return true;
}

bool_t axsd_set_bus_width(u32 index, u8 width)
{
    axsd_slot_t *slot = axsd_slot(index);
    if (slot == NULL_PTR)
        return false;
    if (width != 1 && width != 4 && width != 8)
        return false;
    slot->card.bus_width = width;
    return true;
}

u32 axsd_read(u32 index, u32 block, u32 count, u8 *buffer, u32 capacity)
{
    axsd_slot_t *slot = axsd_slot(index);
    u32 requested;
    u32 available;
    u32 i;
    if (slot == NULL_PTR || buffer == NULL_PTR || count == 0)
        return 0;
    if (!slot->initialized || !slot->card.present)
        return 0;
    if (block >= slot->card.capacity_blocks)
    {
        slot->error_int |= AXSD_ERR_DATA_TIMEOUT;
        return 0;
    }
    if (count > AXSD_MAX_BLOCKS)
        count = AXSD_MAX_BLOCKS;
    available = slot->card.capacity_blocks - block;
    if (count > available)
        count = available;
    requested = count * slot->card.block_size;
    if (requested > capacity)
        count = capacity / slot->card.block_size;
    for (i = 0; i < count * slot->card.block_size; i++)
        buffer[i] = (u8)((block * 7 + i) & 0xFF);
    slot->blocks_read += count;
    slot->data_transfers++;
    axsd_state.total_blocks_read += count;
    slot->normal_int |= AXSD_INT_TRANS_COMPL;
    return count * slot->card.block_size;
}

u32 axsd_write(u32 index, u32 block, u32 count, const u8 *buffer, u32 length)
{
    axsd_slot_t *slot = axsd_slot(index);
    u32 written;
    if (slot == NULL_PTR || buffer == NULL_PTR || count == 0)
        return 0;
    if (!slot->initialized || !slot->card.present || slot->card.write_protected)
        return 0;
    if (block >= slot->card.capacity_blocks)
        return 0;
    if (count > AXSD_MAX_BLOCKS)
        count = AXSD_MAX_BLOCKS;
    written = length / slot->card.block_size;
    if (written < count)
        count = written;
    slot->blocks_written += count;
    slot->data_transfers++;
    axsd_state.total_blocks_written += count;
    slot->normal_int |= AXSD_INT_TRANS_COMPL;
    return count * slot->card.block_size;
}

u32 axsd_ack_interrupt(u32 index, bool_t error)
{
    axsd_slot_t *slot = axsd_slot(index);
    u32 value;
    if (slot == NULL_PTR)
        return 0;
    if (error)
    {
        value = slot->error_int;
        slot->error_int = 0;
    }
    else
    {
        value = slot->normal_int;
        slot->normal_int = 0;
    }
    if (value != 0)
        axsd_state.interrupts++;
    return value;
}

bool_t axsd_recover(u32 index)
{
    axsd_slot_t *slot = axsd_slot(index);
    if (slot == NULL_PTR)
        return false;
    if (slot->retries >= AXSD_RETRY_LIMIT)
    {
        slot->resets++;
        slot->retries = 0;
        return false;
    }
    slot->retries++;
    axsd_clear_errors(slot);
    slot->busy = false;
    return axsd_init_slot(index);
}

void axsd_report(void)
{
    u32 i;
    printk("axsd: slots=%u interrupts=%u read=%u write=%u\n",
           (unsigned)axsd_state.slot_count, (unsigned)axsd_state.interrupts,
           (unsigned)axsd_state.total_blocks_read,
           (unsigned)axsd_state.total_blocks_written);
    for (i = 0; i < axsd_state.slot_count; i++)
    {
        const axsd_slot_t *slot = &axsd_state.slots[i];
        printk("  slot %u base=0x%x clk=%ukHz type=%u cap=%u blocks bw=%u\n",
               (unsigned)i, (unsigned)slot->base, (unsigned)slot->clock_khz,
               (unsigned)slot->card.type, (unsigned)slot->card.capacity_blocks,
               (unsigned)slot->card.bus_width);
        printk("    cmds=%u xfers=%u timeouts=%u crc=%u resets=%u\n",
               (unsigned)slot->command_count, (unsigned)slot->data_transfers,
               (unsigned)slot->timeouts, (unsigned)slot->crc_errors,
               (unsigned)slot->resets);
    }
}

bool_t axsd_selftest(void)
{
    u8 buffer[AXSD_BLOCK_SIZE];
    u32 count;
    if (!axsd_register_slot(0xFEB60000, 200000, 33))
        return false;
    if (!axsd_init_slot(0))
        return false;
    if (axsd_card_type(0) != AXSD_CARD_SD)
        return false;
    if (axsd_capacity_blocks(0) != 0x00760000u)
        return false;
    if (!axsd_set_bus_width(0, 4))
        return false;
    if (axsd_set_bus_width(0, 3))
        return false;
    count = axsd_read(0, 0, 1, buffer, AXSD_BLOCK_SIZE);
    if (count != AXSD_BLOCK_SIZE)
        return false;
    if (buffer[0] != 0)
        return false;
    count = axsd_read(0, 0x7FFFFF, 1, buffer, AXSD_BLOCK_SIZE);
    if (count != 0)
        return false;
    count = axsd_write(0, 0, 1, buffer, AXSD_BLOCK_SIZE);
    if (count != AXSD_BLOCK_SIZE)
        return false;
    if (axsd_ack_interrupt(0, false) == 0)
        return false;
    if (!axsd_recover(0))
        return false;
    return true;
}
