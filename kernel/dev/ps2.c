/* AxiomOS - PS/2 keyboard and mouse.
 *
 * The controller is initialised in a fixed, conservative order: disable both
 * ports, flush the output buffer, enable the ports, then set scancode set 1
 * for the keyboard and 3-byte packets for the mouse.  IRQ1 and IRQ12 are
 * registered so input arrives asynchronously; polling entry points exist for
 * the GUI loop, which drains the ring buffers each frame.
 */

#include <axiom/kernel.h>
#include <axiom/idt.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_MOUSE       0x20

#define PS2_CMD_READ_CONFIG    0x20
#define PS2_CMD_WRITE_CONFIG   0x60
#define PS2_CMD_DISABLE_PORT1  0xAD
#define PS2_CMD_ENABLE_PORT1   0xAE
#define PS2_CMD_DISABLE_PORT2  0xA7
#define PS2_CMD_ENABLE_PORT2   0xA8
#define PS2_CMD_TEST_PORT1     0xAB
#define PS2_CMD_WRITE_PORT1    0xD2
#define PS2_CMD_WRITE_PORT2    0xD4
#define PS2_CMD_READ_PORT1     0xC0

#define KEY_QUEUE_SIZE 128
#define MOUSE_QUEUE_SIZE 64

extern u8  io_inb(u16 port);
extern void io_outb(u16 port, u8 value);
extern void io_wait(void);

struct key_event {
    u8 scancode;
    bool_t pressed;
};

static struct key_event g_key_queue[KEY_QUEUE_SIZE];
static volatile u32 g_key_head;
static volatile u32 g_key_tail;

struct mouse_event {
    i32 dx;
    i32 dy;
    u8  buttons;
};

static struct mouse_event g_mouse_queue[MOUSE_QUEUE_SIZE];
static volatile u32 g_mouse_head;
static volatile u32 g_mouse_tail;

static u8  g_mouse_cycle[3];
static u32 g_mouse_index;
static u8  g_modifiers;
static i32 g_mouse_x;
static i32 g_mouse_y;
static u8  g_mouse_buttons;
static bool_t g_ps2_ready;

/* ------------------------------------------------------------------ */
/* Low level controller access                                        */
/* ------------------------------------------------------------------ */

static void ps2_flush_output(void)
{
    for (u32 i = 0; i < 64; i++) {
        if (!(io_inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL))
            break;
        io_inb(PS2_DATA);
    }
}

static bool_t ps2_wait_input_clear(void)
{
    for (u32 spins = 0; spins < 10000; spins++) {
        if (!(io_inb(PS2_STATUS) & PS2_STATUS_INPUT_FULL))
            return true;
        cpu_relax();
    }
    return false;
}

static bool_t ps2_wait_output_full(void)
{
    for (u32 spins = 0; spins < 10000; spins++) {
        if (io_inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL)
            return true;
        cpu_relax();
    }
    return false;
}

static bool_t ps2_write_command(u8 command)
{
    if (!ps2_wait_input_clear())
        return false;
    io_outb(PS2_COMMAND, command);
    return true;
}

static bool_t ps2_write_data(u8 data)
{
    if (!ps2_wait_input_clear())
        return false;
    io_outb(PS2_DATA, data);
    return true;
}

static u8 ps2_read_data(void)
{
    if (!ps2_wait_output_full())
        return 0xFF;
    return io_inb(PS2_DATA);
}

/* ------------------------------------------------------------------ */
/* Event queues                                                       */
/* ------------------------------------------------------------------ */

static void queue_key(u8 scancode, bool_t pressed)
{
    u32 next = (g_key_head + 1) % KEY_QUEUE_SIZE;
    if (next == g_key_tail) {
        g_key_tail = (g_key_tail + 1) % KEY_QUEUE_SIZE;
    }
    g_key_queue[g_key_head].scancode = scancode;
    g_key_queue[g_key_head].pressed = pressed;
    g_key_head = next;
}

bool_t ps2_get_key(u8 *scancode, bool_t *pressed)
{
    if (g_key_tail == g_key_head)
        return false;
    if (scancode)
        *scancode = g_key_queue[g_key_tail].scancode;
    if (pressed)
        *pressed = g_key_queue[g_key_tail].pressed;
    g_key_tail = (g_key_tail + 1) % KEY_QUEUE_SIZE;
    return true;
}

static void queue_mouse(i32 dx, i32 dy, u8 buttons)
{
    u32 next = (g_mouse_head + 1) % MOUSE_QUEUE_SIZE;
    if (next == g_mouse_tail)
        g_mouse_tail = (g_mouse_tail + 1) % MOUSE_QUEUE_SIZE;
    g_mouse_queue[g_mouse_head].dx = dx;
    g_mouse_queue[g_mouse_head].dy = dy;
    g_mouse_queue[g_mouse_head].buttons = buttons;
    g_mouse_head = next;
}

bool_t ps2_get_mouse(i32 *dx, i32 *dy, u8 *buttons)
{
    if (g_mouse_tail == g_mouse_head)
        return false;
    if (dx) *dx = g_mouse_queue[g_mouse_tail].dx;
    if (dy) *dy = g_mouse_queue[g_mouse_tail].dy;
    if (buttons) *buttons = g_mouse_queue[g_mouse_tail].buttons;
    g_mouse_tail = (g_mouse_tail + 1) % MOUSE_QUEUE_SIZE;
    return true;
}

u8 ps2_modifiers(void)
{
    return g_modifiers;
}

/* ------------------------------------------------------------------ */
/* Interrupt handlers                                                 */
/* ------------------------------------------------------------------ */

static void ps2_handle_keyboard(u8 byte)
{
    static bool_t extended;
    u8 code = byte;

    if (code == 0xE0) {
        extended = true;
        return;
    }
    bool_t released = (code & 0x80) != 0;
    u8 scancode = code & 0x7F;

    if (!extended) {
        switch (scancode) {
        case 0x2A: g_modifiers |= (released ? 0 : 0x01); return;   /* lshift */
        case 0x36: g_modifiers |= (released ? 0 : 0x02); return;   /* rshift */
        case 0x1D: g_modifiers |= (released ? 0 : 0x04); return;   /* ctrl   */
        case 0x38: g_modifiers |= (released ? 0 : 0x08); return;   /* alt    */
        case 0x3A:                                                 /* caps   */
            if (!released)
                g_modifiers ^= 0x10;
            return;
        default:
            break;
        }
    } else {
        extended = false;
        if (scancode == 0x1D) {
            g_modifiers |= (released ? 0 : 0x04);
            return;
        }
        if (scancode == 0x38) {
            g_modifiers |= (released ? 0 : 0x08);
            return;
        }
    }

    queue_key(scancode, !released);
}

static void ps2_handle_mouse(u8 byte)
{
    g_mouse_cycle[g_mouse_index++] = byte;
    if (g_mouse_index < 3)
        return;
    g_mouse_index = 0;

    u8 flags = g_mouse_cycle[0];
    if (!(flags & 0x08)) {
        /* The synchronisation bit is missing: drop the packet and resync. */
        return;
    }

    i32 dx = (i32)(i8)g_mouse_cycle[1];
    i32 dy = -(i32)(i8)g_mouse_cycle[2];
    if (flags & 0x40) dx += 256;
    if (flags & 0x80) dy += 256;

    g_mouse_buttons = (u8)(flags & 0x07);
    g_mouse_x += dx;
    g_mouse_y += dy;
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;

    queue_mouse(dx, dy, g_mouse_buttons);
}

static void ps2_irq(u8 vector, void *frame)
{
    AXIOM_UNUSED_VAR(frame);
    u8 status = io_inb(PS2_STATUS);
    if (!(status & PS2_STATUS_OUTPUT_FULL))
        return;
    u8 data = io_inb(PS2_DATA);
    if (status & PS2_STATUS_MOUSE)
        ps2_handle_mouse(data);
    else
        ps2_handle_keyboard(data);

    if (vector == VEC_IRQ0 + 1 || vector == VEC_IRQ0 + 12) {
        /* Nothing else to do: EOI is handled by irq_dispatch. */
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */

void ps2_init(void)
{
    u8 config;

    g_key_head = g_key_tail = 0;
    g_mouse_head = g_mouse_tail = 0;
    g_mouse_index = 0;
    g_modifiers = 0;
    g_mouse_x = 512;
    g_mouse_y = 384;

    ps2_flush_output();

    /* Disable both ports while configuring. */
    ps2_write_command(PS2_CMD_DISABLE_PORT1);
    ps2_write_command(PS2_CMD_DISABLE_PORT2);
    ps2_flush_output();

    /* Read the configuration byte, enable IRQs and translation. */
    ps2_write_command(PS2_CMD_READ_CONFIG);
    config = ps2_read_data();
    config |= 0x01;     /* port 1 interrupt */
    config |= 0x02;     /* port 2 interrupt */
    config &= (u8)~0x20;/* enable port 1 clock */
    config &= (u8)~0x40;/* enable port 2 clock */
    config |= 0x40;     /* scancode translation */
    ps2_write_command(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);

    /* Enable ports. */
    ps2_write_command(PS2_CMD_ENABLE_PORT1);
    ps2_write_command(PS2_CMD_ENABLE_PORT2);

    /* Reset the keyboard and enable scanning. */
    ps2_write_data(0xFF);
    (void)ps2_read_data();          /* ACK 0xFA */
    ps2_write_data(0xF4);
    (void)ps2_read_data();          /* ACK */

    /* Enable the mouse: status request, then data reporting. */
    ps2_write_command(PS2_CMD_WRITE_PORT2);
    ps2_write_data(0xF6);
    (void)ps2_read_data();
    ps2_write_command(PS2_CMD_WRITE_PORT2);
    ps2_write_data(0xF4);
    (void)ps2_read_data();

    irq_register(VEC_IRQ0 + 1, ps2_irq, "ps2-keyboard");
    irq_register(VEC_IRQ0 + 12, ps2_irq, "ps2-mouse");
    irq_unmask(1);
    irq_unmask(12);

    g_ps2_ready = true;
    printk("ps2: keyboard and mouse ready (config %x)\n", (u64)config);
}

bool_t ps2_present(void)
{
    return g_ps2_ready;
}

/* Polling fallback used when interrupts are disabled, e.g. inside the panic
 * path or during early boot before sti(). */
void ps2_poll(void)
{
    for (u32 i = 0; i < 32; i++) {
        u8 status = io_inb(PS2_STATUS);
        if (!(status & PS2_STATUS_OUTPUT_FULL))
            return;
        u8 data = io_inb(PS2_DATA);
        if (status & PS2_STATUS_MOUSE)
            ps2_handle_mouse(data);
        else
            ps2_handle_keyboard(data);
    }
}

/* ------------------------------------------------------------------ */
/* Scancode translation (set 1)                                       */
/* ------------------------------------------------------------------ */

static const char map_lower[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0
};

static const char map_upper[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0
};

char keyboard_translate(u8 scancode, bool_t shift, bool_t caps)
{
    if (scancode >= 128)
        return 0;
    if (caps && !shift)
        return map_upper[scancode];
    return shift ? map_upper[scancode] : map_lower[scancode];
}

i32 mouse_position_x(void) { return g_mouse_x; }
i32 mouse_position_y(void) { return g_mouse_y; }
u8  mouse_buttons(void)    { return g_mouse_buttons; }

void mouse_set_position(i32 x, i32 y)
{
    g_mouse_x = x;
    g_mouse_y = y;
}
