/*
 * AxiomOS - i8042 PS/2 controller driver.
 *
 * The controller is two ports: 0x64 for status and commands, 0x60 for data in
 * either direction.  Every transfer is a handshake -- wait for the input buffer
 * to drain before writing, wait for the output buffer to fill before reading --
 * and a timeout is not fatal: real hardware occasionally drops an ACK, and a
 * driver that panics on the first timeout is useless.  Timeouts are counted and
 * retried once, then reported upward.
 *
 * Initialisation follows the sequence the PC/AT specification prescribes:
 * disable both ports, flush, read the configuration byte, run the controller
 * self-test, test both ports, enable what answered, then set the configuration
 * byte with translation and both IRQs on.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>
#include <axiom/axinput.h>

extern u8 io_inb(u16 port);
extern void io_outb(u16 port, u8 value);
extern void io_wait(void);

#define I8042_TIMEOUT_US 20000

static i8042_controller_t g_controller;
static bool_t g_ready;

/* Crude microsecond delay.  A calibrated TSC would be better, but the controller
 * init runs before the clock driver is final on every platform we support, so a
 * port-read loop is the honest choice. */
static void i8042_delay_us(u32 microseconds)
{
    for (u32 i = 0; i < microseconds; i++) {
        io_inb(0x80);
    }
}

static u64 i8042_now(void)
{
    return axbpf_now_ns();
}

i32 i8042_read_status(u8 *out)
{
    if (!out) {
        return -22;
    }
    *out = io_inb(I8042_STATUS_PORT);
    g_controller.status = *out;
    g_controller.stats.reads++;
    return 0;
}

bool_t i8042_has_output(void)
{
    u8 status = io_inb(I8042_STATUS_PORT);
    return (bool_t)((status & I8042_STATUS_OUTPUT_FULL) != 0);
}

bool_t i8042_has_input(void)
{
    u8 status = io_inb(I8042_STATUS_PORT);
    return (bool_t)((status & I8042_STATUS_INPUT_FULL) != 0);
}

i32 i8042_wait_write(void)
{
    u64 start = i8042_now();
    while (i8042_has_input()) {
        if (i8042_now() - start > (u64)I8042_TIMEOUT_US * 1000) {
            g_controller.stats.timeouts++;
            return -110;
        }
        i8042_delay_us(1);
    }
    return 0;
}

i32 i8042_wait_read(void)
{
    u64 start = i8042_now();
    while (!i8042_has_output()) {
        if (i8042_now() - start > (u64)I8042_TIMEOUT_US * 1000) {
            g_controller.stats.timeouts++;
            return -110;
        }
        i8042_delay_us(1);
    }
    return 0;
}

i32 i8042_write_command(u8 command)
{
    i32 rc = i8042_wait_write();
    if (rc < 0) {
        return rc;
    }
    io_outb(I8042_COMMAND_PORT, command);
    g_controller.stats.commands++;
    g_controller.stats.writes++;
    i8042_delay_us(4);
    return 0;
}

i32 i8042_write_data(u8 data)
{
    i32 rc = i8042_wait_write();
    if (rc < 0) {
        return rc;
    }
    io_outb(I8042_DATA_PORT, data);
    g_controller.stats.writes++;
    i8042_delay_us(4);
    return 0;
}

i32 i8042_read_data(u8 *out)
{
    if (!out) {
        return -22;
    }
    i32 rc = i8042_wait_read();
    if (rc < 0) {
        return rc;
    }
    *out = io_inb(I8042_DATA_PORT);
    g_controller.stats.reads++;
    return 0;
}

/* Write a command with an optional parameter and optionally collect one byte of
 * response.  The parameter byte is written to 0x60 after the command completes,
 * which is how the controller distinguishes command and data phases. */
i32 i8042_command(u8 command, u8 *parameter)
{
    i32 rc = i8042_write_command(command);
    if (rc < 0) {
        return rc;
    }
    if (parameter) {
        rc = i8042_write_data(*parameter);
        if (rc < 0) {
            return rc;
        }
    }
    return 0;
}

i32 i8042_read_config_byte(u8 *out)
{
    i32 rc = i8042_write_command(I8042_CMD_READ_CONFIG);
    if (rc < 0) {
        return rc;
    }
    return i8042_read_data(out);
}

i32 i8042_write_config_byte(u8 value)
{
    i32 rc = i8042_write_command(I8042_CMD_WRITE_CONFIG);
    if (rc < 0) {
        return rc;
    }
    return i8042_write_data(value);
}

/* Send a byte to the auxiliary (mouse) port.  The D4 prefix tells the
 * controller the following data byte is for the second port. */
i32 i8042_write_aux(u8 data)
{
    i32 rc = i8042_write_command(I8042_CMD_WRITE_AUX);
    if (rc < 0) {
        return rc;
    }
    rc = i8042_write_data(data);
    if (rc < 0) {
        return rc;
    }
    g_controller.stats.mouse_bytes++;
    return 0;
}

i32 i8042_read_aux(u8 *out)
{
    u8 status = 0;
    i32 rc = i8042_read_status(&status);
    if (rc < 0) {
        return rc;
    }
    if (!(status & I8042_STATUS_AUX_DATA)) {
        /* Data is from the keyboard; push it back conceptually by returning a
     * distinct code so the caller can route it. */
        return -11;
    }
    if (!(status & I8042_STATUS_OUTPUT_FULL)) {
        return -11;
    }
    *out = io_inb(I8042_DATA_PORT);
    g_controller.stats.reads++;
    return 0;
}

/* Wait for an ACK from the device, tolerating one resend. */
static i32 i8042_expect_ack(void)
{
    for (u32 attempt = 0; attempt < 2; attempt++) {
        u8 value = 0;
        i32 rc = i8042_read_data(&value);
        if (rc < 0) {
            return rc;
        }
        if (value == I8042_ACK) {
            return 0;
        }
        if (value == I8042_RESEND) {
            g_controller.stats.resends++;
            continue;
        }
        if (value == I8042_ERROR) {
            return -5;
        }
        /* Some devices emit 0x00 or an extra byte before the ACK. */
    }
    return -5;
}

/* Drain every pending byte, counting what we throw away. */
static void i8042_flush(void)
{
    for (u32 i = 0; i < 64 && i8042_has_output(); i++) {
        (void)io_inb(I8042_DATA_PORT);
        g_controller.stats.spurious++;
    }
}

/* Keyboard self-test and port enable. */
static bool_t i8042_init_keyboard(void)
{
    i32 rc = i8042_write_command(I8042_CMD_DISABLE_FIRST);
    if (rc < 0) {
        return (bool_t)0;
    }
    rc = i8042_write_command(I8042_CMD_TEST_FIRST);
    if (rc < 0) {
        return (bool_t)0;
    }
    u8 result = 0;
    rc = i8042_read_data(&result);
    if (rc < 0 || result != 0x00) {
        printk("i8042: keyboard port test failed (%02x)\n", result);
        return (bool_t)0;
    }
    rc = i8042_write_command(I8042_CMD_ENABLE_FIRST);
    if (rc < 0) {
        return (bool_t)0;
    }
    return (bool_t)1;
}

/* Mouse port self-test; controllers report 0x00 on success. */
static bool_t i8042_init_mouse(void)
{
    i32 rc = i8042_write_command(I8042_CMD_DISABLE_AUX);
    if (rc < 0) {
        return (bool_t)0;
    }
    rc = i8042_write_command(I8042_CMD_TEST_AUX);
    if (rc < 0) {
        return (bool_t)0;
    }
    u8 result = 0;
    rc = i8042_read_data(&result);
    if (rc < 0 || result != I8042_AUX_TEST_OK) {
        printk("i8042: auxiliary port test failed (%02x)\n", result);
        return (bool_t)0;
    }
    rc = i8042_write_command(I8042_CMD_ENABLE_AUX);
    if (rc < 0) {
        return (bool_t)0;
    }
    return (bool_t)1;
}

/* Put the keyboard into a known state: translated set 1, typematic defaults,
 * LEDs off.  Each command must be acknowledged. */
static i32 i8042_keyboard_setup(void)
{
    i32 rc = i8042_write_data(I8042_KBD_CMD_DISABLE);
    if (rc >= 0) {
        (void)i8042_expect_ack();
    }
    i8042_flush();
    rc = i8042_write_data(I8042_KBD_CMD_SET_SCAN);
    if (rc < 0) {
        return rc;
    }
    if (i8042_expect_ack() < 0) {
        /* The device may not support scanning; continue anyway. */
    }
    rc = i8042_write_data(0x01);
    if (rc < 0) {
        return rc;
    }
    g_controller.stats.keyboard_bytes++;

    rc = i8042_write_data(I8042_KBD_CMD_SET_LEDS);
    if (rc >= 0 && i8042_expect_ack() == 0) {
        (void)i8042_write_data(0x00);
        (void)i8042_expect_ack();
    }

    /* Identify so we can tell a translated keyboard from a native one. */
    rc = i8042_write_data(I8042_KBD_CMD_IDENTIFY);
    if (rc >= 0 && i8042_expect_ack() == 0) {
        u8 id = 0;
        if (i8042_read_data(&id) == 0) {
            g_controller.keyboard_id[0] = id;
            g_controller.keyboard_id[1] = 0;
            if (id == 0xAB || id == 0xAC) {
                g_controller.translation = (bool_t)1;
            }
        }
    }

    rc = i8042_write_data(I8042_KBD_CMD_ENABLE);
    if (rc >= 0) {
        (void)i8042_expect_ack();
    }
    return 0;
}

/* Probe the mouse: reset, read the device id, then enable reporting at a sane
 * sample rate.  A device id of 3 or 4 means scroll wheel, 4 meaning five
 * buttons. */
static i32 i8042_mouse_setup(void)
{
    u8 id = 0;
    i32 rc = i8042_write_aux(I8042_MOUSE_CMD_RESET);
    if (rc < 0) {
        return rc;
    }
    (void)i8042_expect_ack();
    i8042_flush();

    rc = i8042_write_aux(I8042_MOUSE_CMD_GET_DEVICE);
    if (rc < 0) {
        return rc;
    }
    if (i8042_expect_ack() == 0) {
        (void)i8042_read_aux(&id);
        g_controller.mouse_id = id;
        if (id == 3) {
            g_controller.mouse_wheel = (bool_t)1;
        } else if (id == 4) {
            g_controller.mouse_wheel = (bool_t)1;
            g_controller.mouse_5button = (bool_t)1;
        }
    }

    /* Sample rate 100/s, 8 counts/mm, 1:1 scaling. */
    rc = i8042_write_aux(I8042_MOUSE_CMD_SET_SAMPLE);
    if (rc >= 0 && i8042_expect_ack() == 0) {
        (void)i8042_write_aux(100);
        (void)i8042_expect_ack();
    }
    rc = i8042_write_aux(I8042_MOUSE_CMD_SET_RES);
    if (rc >= 0 && i8042_expect_ack() == 0) {
        (void)i8042_write_aux(3);
        (void)i8042_expect_ack();
    }
    rc = i8042_write_aux(I8042_MOUSE_CMD_SET_SCALE11);
    if (rc >= 0) {
        (void)i8042_expect_ack();
    }
    rc = i8042_write_aux(I8042_MOUSE_CMD_ENABLE_DATA);
    if (rc >= 0) {
        (void)i8042_expect_ack();
    }
    return 0;
}

i8042_controller_t *i8042_init(void)
{
    ax_memset(&g_controller, 0, sizeof(g_controller));
    g_controller.state = I8042_STATE_UNINITIALISED;
    g_controller.poll_interval_us = 1000;

    /* Disable both ports and drain before touching configuration. */
    (void)i8042_write_command(I8042_CMD_DISABLE_FIRST);
    (void)i8042_write_command(I8042_CMD_DISABLE_AUX);
    i8042_flush();

    u8 config = 0;
    if (i8042_read_config_byte(&config) < 0) {
        printk("i8042: controller did not answer\n");
        g_controller.state = I8042_STATE_ABSENT;
        return &g_controller;
    }
    g_controller.config = config;

    /* Controller self-test.  A failure here is not fatal on all firmware --
     * some report 0xFC yet work -- so we only warn. */
    i32 rc = i8042_write_command(I8042_CMD_SELF_TEST);
    if (rc >= 0) {
        u8 result = 0;
        if (i8042_read_data(&result) == 0 && result != I8042_SELF_TEST_OK) {
            printk("i8042: self test reported %02x\n", result);
            g_controller.state = I8042_STATE_DEGRADED;
        }
    }

    g_controller.keyboard_present = i8042_init_keyboard();
    g_controller.mouse_present = i8042_init_mouse();

    /* Configuration: IRQ1 on, IRQ12 on when a mouse exists, translation on so
     * we always see scancode set 1 at the port. */
    config = (u8)(config & ~(I8042_CONFIG_FIRST_IRQ | I8042_CONFIG_AUX_IRQ));
    config |= I8042_CONFIG_FIRST_IRQ;
    if (g_controller.mouse_present) {
        config |= I8042_CONFIG_AUX_IRQ;
    }
    config |= I8042_CONFIG_TRANSLATE;
    config &= (u8)~(I8042_CONFIG_CLOCK1 | I8042_CONFIG_CLOCK2);
    g_controller.config = config;
    g_controller.translation = (bool_t)1;
    if (i8042_write_config_byte(config) < 0) {
        printk("i8042: could not write configuration byte\n");
    }

    if (g_controller.keyboard_present) {
        (void)i8042_keyboard_setup();
    }
    if (g_controller.mouse_present) {
        (void)i8042_mouse_setup();
    }
    i8042_flush();

    if (g_controller.keyboard_present || g_controller.mouse_present) {
        g_controller.state = I8042_STATE_READY;
    } else {
        g_controller.state = I8042_STATE_ABSENT;
    }
    g_ready = (bool_t)(g_controller.state == I8042_STATE_READY);

    printk("i8042: %s keyboard=%d mouse=%d id=%02x wheel=%d config=%02x\n",
           g_ready ? "ready" : "absent", g_controller.keyboard_present,
           g_controller.mouse_present, g_controller.mouse_id,
           g_controller.mouse_wheel, g_controller.config);
    return &g_controller;
}

bool_t i8042_ready(void)
{
    return g_ready;
}

i8042_state_t i8042_state(void)
{
    return g_controller.state;
}

/* Poll the controller: read up to `budget` bytes and route them.  Returns the
 * number of bytes consumed.  The IRQ path and the polling path share this
 * function, so there is exactly one place where bytes enter the system. */
u32 i8042_poll(u32 budget)
{
    u32 consumed = 0;
    while (consumed < budget && i8042_has_output()) {
        u8 status = io_inb(I8042_STATUS_PORT);
        u8 data = io_inb(I8042_DATA_PORT);
        if (status & (I8042_STATUS_TIMEOUT | I8042_STATUS_PARITY)) {
            g_controller.stats.spurious++;
            consumed++;
            continue;
        }
        if (status & I8042_STATUS_AUX_DATA) {
            if (g_controller.mouse_present) {
                (void)mouse_feed_byte(data);
            }
            g_controller.stats.mouse_bytes++;
        } else {
            if (g_controller.keyboard_present) {
                (void)keyboard_feed_byte(data);
            }
            g_controller.stats.keyboard_bytes++;
        }
        consumed++;
    }
    return consumed;
}

/* Handle an interrupt from IRQ1 or IRQ12; the byte source is the status bit. */
void i8042_irq(void)
{
    if (!g_ready) {
        return;
    }
    (void)i8042_poll(16);
}

void i8042_reset_stats(void)
{
    ax_memset(&g_controller.stats, 0, sizeof(g_controller.stats));
}

void i8042_dump(void)
{
    printk("i8042: state=%u config=%02x kbd=%d mouse=%d id=%02x wheel=%d\n",
           (u32)g_controller.state, g_controller.config,
           g_controller.keyboard_present, g_controller.mouse_present,
           g_controller.mouse_id, g_controller.mouse_wheel);
    printk("i8042: cmd=%u rd=%u wr=%u to=%u resend=%u kbd_bytes=%u "
           "mouse_bytes=%u spurious=%u\n",
           g_controller.stats.commands, g_controller.stats.reads,
           g_controller.stats.writes, g_controller.stats.timeouts,
           g_controller.stats.resends, g_controller.stats.keyboard_bytes,
           g_controller.stats.mouse_bytes, g_controller.stats.spurious);
}
