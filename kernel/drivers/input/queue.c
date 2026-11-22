/*
 * AxiomOS - input event ring.
 *
 * One producer (the interrupt path), one consumer (the GUI frame loop).  A
 * single-producer single-consumer ring needs no lock as long as the indices are
 * only ever advanced by their owner and the head/tail reads are done in the
 * right order: the producer writes the payload before publishing head, the
 * consumer reads head before copying the payload.  On x86 the store ordering is
 * already strong enough; the comments below mark where a weaker architecture
 * would need a barrier.
 *
 * Overflow policy: drop the oldest event, not the newest.  A GUI that lags
 * behind a key-repeat burst should still see the most recent keystrokes; losing
 * the first few is visible as a missing character, losing the last few is
 * visible as a keystroke that never arrived, which feels broken.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axinput.h>

#define RING_MASK (AXINPUT_RING_SIZE - 1)

void axinput_ring_init(axinput_ring_t *ring)
{
    if (!ring) {
        return;
    }
    ax_memset(ring, 0, sizeof(*ring));
}

u32 axinput_ring_count(const axinput_ring_t *ring)
{
    if (!ring) {
        return 0;
    }
    /* head and tail are free-running; the difference is the fill level. */
    u32 head = ring->head;
    u32 tail = ring->tail;
    if (head >= tail) {
        return head - tail;
    }
    return (AXINPUT_RING_SIZE - tail) + head;
}

static bool_t ring_full(const axinput_ring_t *ring)
{
    return (bool_t)(axinput_ring_count(ring) >= AXINPUT_RING_SIZE);
}

bool_t axinput_ring_push(axinput_ring_t *ring, const axinput_event_t *event)
{
    if (!ring || !event) {
        return (bool_t)0;
    }

    if (ring_full(ring)) {
        /* Drop the oldest so the consumer stays close to real time. */
        ring->tail = (ring->tail + 1) & RING_MASK;
        ring->dropped++;
    }

    ring->events[ring->head & RING_MASK] = *event;
    /* Publish after the payload is visible. */
    ring->head = (ring->head + 1);
    ring->pushed++;

    u32 count = axinput_ring_count(ring);
    if (count > ring->high_water) {
        ring->high_water = count;
    }
    return (bool_t)1;
}

bool_t axinput_ring_pop(axinput_ring_t *ring, axinput_event_t *out)
{
    if (!ring || !out) {
        return (bool_t)0;
    }
    if (axinput_ring_count(ring) == 0) {
        return (bool_t)0;
    }
    *out = ring->events[ring->tail & RING_MASK];
    /* Advance only after the copy. */
    ring->tail = (ring->tail + 1);
    ring->popped++;
    return (bool_t)1;
}

bool_t axinput_ring_peek(const axinput_ring_t *ring, axinput_event_t *out)
{
    if (!ring || !out) {
        return (bool_t)0;
    }
    if (axinput_ring_count(ring) == 0) {
        return (bool_t)0;
    }
    *out = ring->events[ring->tail & RING_MASK];
    return (bool_t)1;
}

void axinput_ring_flush(axinput_ring_t *ring)
{
    if (!ring) {
        return;
    }
    ring->tail = ring->head;
}

void axinput_ring_stats(const axinput_ring_t *ring, char *buffer, usize cap)
{
    if (!ring || !buffer || cap == 0) {
        return;
    }
    ax_snprintf(buffer, cap,
                "input: %u queued, %u high water, %llu pushed, %llu popped, "
                "%u dropped",
                axinput_ring_count(ring), ring->high_water,
                (unsigned long long)ring->pushed,
                (unsigned long long)ring->popped, ring->dropped);
}

/* Return the event class as a short name for logs and the debug overlay. */
const char *axinput_class_name(u16 event_class)
{
    switch (event_class) {
    case AXINPUT_KEY: return "key";
    case AXINPUT_MOUSE_MOVE: return "move";
    case AXINPUT_MOUSE_BUTTON: return "button";
    case AXINPUT_MOUSE_WHEEL: return "wheel";
    case AXINPUT_HOTKEY: return "hotkey";
    case AXINPUT_SYNC: return "sync";
    default: return "?";
    }
}

/* Human-readable modifier string, e.g. "C-A-S". */
void axinput_modifier_string(u32 modifiers, char *buffer, usize cap)
{
    if (!buffer || cap == 0) {
        return;
    }
    usize used = 0;
    if (modifiers & AXMOD_CTRL) {
        if (used + 2 < cap) { buffer[used++] = 'C'; buffer[used++] = '-'; }
    }
    if (modifiers & AXMOD_ALT) {
        if (used + 2 < cap) { buffer[used++] = 'A'; buffer[used++] = '-'; }
    }
    if (modifiers & AXMOD_ANY_SHIFT) {
        if (used + 2 < cap) { buffer[used++] = 'S'; buffer[used++] = '-'; }
    }
    if (modifiers & AXMOD_META) {
        if (used + 2 < cap) { buffer[used++] = 'M'; buffer[used++] = '-'; }
    }
    if (used > 0 && used <= cap) {
        buffer[used - 1] = 0;   /* drop the trailing separator */
    } else {
        buffer[0] = 0;
    }
}
