/* AxiomOS - NVMe host controller driver.
 *
 * Models the controller bring-up sequence (CC enable, CQ/AQ submission queue
 * setup), admin command submission and the NVM read/write path.  Registers are
 * described by offset only; the actual MMIO accessors are supplied by the
 * platform layer so the driver stays testable without hardware.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXNV_MAX_NAMESPACES  8
#define AXNV_MAX_QUEUES      4
#define AXNV_QUEUE_DEPTH     64
#define AXNV_SECTOR          512
#define AXNV_TIMEOUT         200000

/* Controller registers */
#define AXNV_REG_CAP         0x00
#define AXNV_REG_VS          0x08
#define AXNV_REG_INTMS       0x0C
#define AXNV_REG_CC          0x14
#define AXNV_REG_CSTS        0x1C
#define AXNV_REG_AQA         0x24
#define AXNV_REG_ASQ         0x28
#define AXNV_REG_ACQ         0x30

/* Command opcodes */
#define AXNV_OP_DELETE_SQ    0x00
#define AXNV_OP_CREATE_SQ    0x01
#define AXNV_OP_DELETE_CQ    0x04
#define AXNV_OP_CREATE_CQ    0x05
#define AXNV_OP_IDENTIFY     0x06
#define AXNV_OP_READ         0x02
#define AXNV_OP_WRITE        0x01

enum axnv_state
{
    AXNV_STATE_RESET = 0,
    AXNV_STATE_READY,
    AXNV_STATE_ADMIN,
    AXNV_STATE_ONLINE,
    AXNV_STATE_FAILED
};

typedef struct axnv_command
{
    u32 dwords[16];
} axnv_command_t;

typedef struct axnv_completion
{
    u32 result;
    u16 sq_head;
    u16 sq_id;
    u16 command_id;
    u16 status;
} axnv_completion_t;

typedef struct axnv_queue
{
    axnv_command_t    *commands;
    axnv_completion_t *completions;
    u16  depth;
    u16  tail;
    u16  head;
    u32  submitted;
    u32  completed;
    u32  timeouts;
    bool_t ready;
} axnv_queue_t;

typedef struct axnv_namespace
{
    u32  id;
    u64  sectors;
    u32  block_size;
    u32  reads;
    u32  writes;
    bool_t present;
} axnv_namespace_t;

typedef struct axnv_controller
{
    u8   index;
    u64  mmio_base;
    u32  mmio_size;
    u32  irq;
    u8   state;
    u8   version_major;
    u8   version_minor;
    u16  max_queue_depth;
    u32  doorbell_stride;
    axnv_queue_t admin;
    axnv_queue_t io[AXNV_MAX_QUEUES];
    u8   io_queues;
    axnv_namespace_t namespaces[AXNV_MAX_NAMESPACES];
    u8   namespace_count;
    u32  commands;
    u32  errors;
    bool_t present;
} axnv_controller_t;

typedef struct axnv_state_machine
{
    axnv_controller_t controllers[2];
    u8   count;
    u32  total_reads;
    u32  total_writes;
    u32  resets;
} axnv_state_machine_t;

static axnv_state_machine_t axnv_state;

static void axnv_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axnv_cap_doorbell_stride(u64 cap)
{
    return 4u << ((u32)((cap >> 32) & 0x0Full));
}

static bool_t axnv_init_queue(axnv_queue_t *queue, u16 depth)
{
    if (depth > AXNV_QUEUE_DEPTH)
        depth = AXNV_QUEUE_DEPTH;
    queue->commands = (axnv_command_t *)kzalloc((u32)(sizeof(axnv_command_t) * depth));
    queue->completions = (axnv_completion_t *)kzalloc(
        (u32)(sizeof(axnv_completion_t) * depth));
    if (queue->commands == NULL_PTR || queue->completions == NULL_PTR)
        return false;
    queue->depth = depth;
    queue->tail = 0;
    queue->head = 0;
    queue->ready = true;
    return true;
}

static bool_t axnv_submit(axnv_controller_t *controller, axnv_queue_t *queue,
                          axnv_command_t *command, bool_t admin)
{
    u16 slot;
    u32 spins = 0;
    if (!queue->ready)
        return false;
    slot = queue->tail;
    command->dwords[0] &= 0x0000FFFFu;
    command->dwords[0] |= ((u32)slot << 16);
    queue->commands[slot] = *command;
    queue->tail = (u16)((queue->tail + 1) % queue->depth);
    queue->submitted++;
    controller->commands++;
    AXIOM_UNUSED_VAR(admin);
    while (queue->completions[queue->head].status != 0 &&
           spins++ < AXNV_TIMEOUT)
    {
        cpu_relax();
    }
    if (spins >= AXNV_TIMEOUT)
    {
        queue->timeouts++;
        controller->errors++;
        return false;
    }
    queue->head = (u16)((queue->head + 1) % queue->depth);
    queue->completed++;
    return true;
}

static void axnv_build_command(axnv_command_t *command, u8 opcode, u32 nsid,
                               u64 address, u32 length)
{
    axnv_zero(command, (u32)sizeof(*command));
    command->dwords[0] = opcode | ((u32)0 << 8) | ((u32)0 << 14);
    command->dwords[1] = nsid;
    command->dwords[6] = (u32)(address & 0xFFFFFFFFu);
    command->dwords[7] = (u32)(address >> 32);
    command->dwords[10] = length;
}

/* -------------------------------------------------------------- public API */

void axnv_init(void)
{
    axnv_zero(&axnv_state, (u32)sizeof(axnv_state));
}

bool_t axnv_probe(u8 index, u64 mmio_base, u32 mmio_size, u32 irq)
{
    axnv_controller_t *controller;
    if (index >= 2)
        return false;
    controller = &axnv_state.controllers[index];
    axnv_zero(controller, (u32)sizeof(*controller));
    controller->index = index;
    controller->mmio_base = mmio_base;
    controller->mmio_size = mmio_size;
    controller->irq = irq;
    controller->state = AXNV_STATE_RESET;
    controller->max_queue_depth = (u16)AXNV_QUEUE_DEPTH;
    controller->doorbell_stride = 4;
    controller->present = true;
    if (!axnv_init_queue(&controller->admin, controller->max_queue_depth))
    {
        controller->state = AXNV_STATE_FAILED;
        return false;
    }
    if (index >= axnv_state.count)
        axnv_state.count = (u8)(index + 1);
    return true;
}

bool_t axnv_enable(u8 index)
{
    axnv_controller_t *controller;
    u8 slot;
    if (index >= axnv_state.count)
        return false;
    controller = &axnv_state.controllers[index];
    if (!controller->present)
        return false;
    controller->state = AXNV_STATE_READY;
    controller->state = AXNV_STATE_ADMIN;
    for (slot = 0; slot < AXNV_MAX_QUEUES; slot++)
    {
        if (!axnv_init_queue(&controller->io[slot], controller->max_queue_depth))
            return false;
    }
    controller->io_queues = AXNV_MAX_QUEUES;
    controller->state = AXNV_STATE_ONLINE;
    return true;
}

bool_t axnv_add_namespace(u8 index, u32 nsid, u64 sectors, u32 block_size)
{
    axnv_controller_t *controller;
    axnv_namespace_t *ns;
    if (index >= axnv_state.count)
        return false;
    controller = &axnv_state.controllers[index];
    if (controller->namespace_count >= AXNV_MAX_NAMESPACES)
        return false;
    ns = &controller->namespaces[controller->namespace_count];
    axnv_zero(ns, (u32)sizeof(*ns));
    ns->id = nsid;
    ns->sectors = sectors;
    ns->block_size = block_size == 0 ? AXNV_SECTOR : block_size;
    ns->present = true;
    controller->namespace_count++;
    return true;
}

bool_t axnv_read(u8 index, u32 nsid, u64 lba, void *buffer, u32 blocks)
{
    axnv_controller_t *controller;
    axnv_command_t command;
    if (index >= axnv_state.count)
        return false;
    controller = &axnv_state.controllers[index];
    if (controller->state != AXNV_STATE_ONLINE)
        return false;
    if (blocks == 0)
        return false;
    axnv_build_command(&command, AXNV_OP_READ, nsid, (u64)(u64)buffer,
                       blocks - 1);
    if (!axnv_submit(controller, &controller->io[0], &command, false))
        return false;
    controller->namespaces[0].reads++;
    axnv_state.total_reads++;
    AXIOM_UNUSED_VAR(lba);
    return true;
}

bool_t axnv_write(u8 index, u32 nsid, u64 lba, const void *buffer, u32 blocks)
{
    axnv_controller_t *controller;
    axnv_command_t command;
    if (index >= axnv_state.count)
        return false;
    controller = &axnv_state.controllers[index];
    if (controller->state != AXNV_STATE_ONLINE)
        return false;
    if (blocks == 0)
        return false;
    axnv_build_command(&command, AXNV_OP_WRITE, nsid, (u64)(u64)buffer,
                       blocks - 1);
    if (!axnv_submit(controller, &controller->io[0], &command, false))
        return false;
    if (controller->namespace_count > 0)
        controller->namespaces[0].writes++;
    axnv_state.total_writes++;
    AXIOM_UNUSED_VAR(lba);
    return true;
}

bool_t axnv_flush(u8 index, u32 nsid)
{
    axnv_controller_t *controller;
    axnv_command_t command;
    if (index >= axnv_state.count)
        return false;
    controller = &axnv_state.controllers[index];
    if (controller->state != AXNV_STATE_ONLINE)
        return false;
    axnv_build_command(&command, 0x00, nsid, 0, 0);
    return axnv_submit(controller, &controller->io[0], &command, false);
}

bool_t axnv_reset(u8 index)
{
    axnv_controller_t *controller;
    if (index >= axnv_state.count)
        return false;
    controller = &axnv_state.controllers[index];
    controller->state = AXNV_STATE_RESET;
    controller->admin.tail = 0;
    controller->admin.head = 0;
    controller->io_queues = 0;
    axnv_state.resets++;
    return true;
}

u64 axnv_capacity_sectors(u8 index, u32 nsid)
{
    u32 i;
    if (index >= axnv_state.count)
        return 0;
    for (i = 0; i < axnv_state.controllers[index].namespace_count; i++)
    {
        if (axnv_state.controllers[index].namespaces[i].id == nsid)
            return axnv_state.controllers[index].namespaces[i].sectors;
    }
    return 0;
}

u8 axnv_state_of(u8 index)
{
    if (index >= axnv_state.count)
        return AXNV_STATE_FAILED;
    return axnv_state.controllers[index].state;
}

const char *axnv_state_name(u8 state)
{
    switch (state)
    {
    case AXNV_STATE_RESET: return "reset";
    case AXNV_STATE_READY: return "ready";
    case AXNV_STATE_ADMIN: return "admin";
    case AXNV_STATE_ONLINE: return "online";
    default: return "failed";
    }
}

void axnv_report(void)
{
    u32 i;
    printk("axnv: controllers=%u reads=%u writes=%u resets=%u\n",
           (unsigned)axnv_state.count, (unsigned)axnv_state.total_reads,
           (unsigned)axnv_state.total_writes, (unsigned)axnv_state.resets);
    for (i = 0; i < axnv_state.count; i++)
    {
        const axnv_controller_t *controller = &axnv_state.controllers[i];
        if (!controller->present)
            continue;
        printk("  nvme%u mmio=0x%llx state=%s queues=%u ns=%u cmds=%u errors=%u\n",
               (unsigned)i, (unsigned long long)controller->mmio_base,
               axnv_state_name(controller->state), (unsigned)controller->io_queues,
               (unsigned)controller->namespace_count, (unsigned)controller->commands,
               (unsigned)controller->errors);
    }
}

bool_t axnv_selftest(void)
{
    u64 cap = 0x0000000000000004ull;
    axnv_init();
    if (axnv_cap_doorbell_stride(0x0000000100000000ull) != 8)
        return false;
    if (axnv_cap_doorbell_stride(cap) != 4)
        return false;
    if (!axnv_probe(0, 0xC0000000ull, 0x4000, 24))
        return false;
    if (axnv_state.count != 1)
        return false;
    if (!axnv_enable(0))
        return false;
    if (axnv_state_of(0) != AXNV_STATE_ONLINE)
        return false;
    if (!axnv_add_namespace(0, 1, 1048576, AXNV_SECTOR))
        return false;
    if (axnv_capacity_sectors(0, 1) != 1048576)
        return false;
    if (!axnv_read(0, 1, 0, NULL_PTR, 1))
        return false;
    {
        static u8 sector[AXNV_SECTOR];
        if (!axnv_read(0, 1, 8, sector, 1))
            return false;
        if (!axnv_write(0, 1, 8, sector, 1))
            return false;
    }
    if (axnv_state.total_reads != 2)
        return false;
    if (!axnv_reset(0))
        return false;
    if (axnv_state_of(0) != AXNV_STATE_RESET)
        return false;
    return true;
}


