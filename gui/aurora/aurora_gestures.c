/*
 * AxiomOS - pointer and touch gesture recognition.
 *
 * The recogniser turns a stream of pointer samples into intents: scroll, fling,
 * pinch, swipe, drag.  Recognition is a state machine with explicit thresholds,
 * because a gesture layer without thresholds sends every accidental one-pixel
 * wobble to the application and feels broken.
 *
 * Kinetic scrolling uses a fixed decay per frame rather than a time-based
 * exponential.  The pointer layer already samples at a stable rate, so an
 * exponential in "elapsed microseconds" would need the sample clock plumbed
 * through here for no visible gain; decaying per tick is smooth and testable.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXG_TAP_MAX_MOVE        4
#define AXG_TAP_MAX_TICKS       12
#define AXG_SCROLL_THRESHOLD    3
#define AXG_FLING_MIN_VELOCITY  12
#define AXG_PINCH_THRESHOLD     0.06f
#define AXG_SWIPE_MIN_DISTANCE 96
#define AXG_SWIPE_MAX_CROSS     40
#define AXG_PRESSURE_STEPS      4

typedef enum axg_state {
    AXG_IDLE = 0,
    AXG_PRESSED,
    AXG_SCROLLING,
    AXG_PINCHING,
    AXG_SWIPING,
    AXG_RELEASED
} axg_state_t;

typedef enum axg_intent {
    AXG_INTENT_NONE = 0,
    AXG_INTENT_TAP,
    AXG_INTENT_DOUBLE_TAP,
    AXG_INTENT_SCROLL,
    AXG_INTENT_FLING,
    AXG_INTENT_PINCH,
    AXG_INTENT_SWIPE_LEFT,
    AXG_INTENT_SWIPE_RIGHT,
    AXG_INTENT_SWIPE_UP,
    AXG_INTENT_SWIPE_DOWN,
    AXG_INTENT_DRAG
} axg_intent_t;

typedef struct axg_recogniser {
    axg_state_t state;
    i32         start_x;
    i32         start_y;
    i32         last_x;
    i32         last_y;
    i32         total_x;
    i32         total_y;
    i32         ticks;
    i32         velocity_x;      /* pixels per tick, quantised */
    i32         velocity_y;
    i32         fingers;
    i32         pressure;
    i32         last_tap_x;
    i32         last_tap_y;
    i32         tap_ticks;
    bool_t      dragging;
    bool_t      momentum;
    i32         momentum_x;
    i32         momentum_y;
    i32         pinch_start_span;
    i32         pinch_factor;      /* percent: 100 = unchanged */
    bool_t      pinch_active;
} axg_recogniser_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void axg_init(axg_recogniser_t *gesture)
{
    if (!gesture) return;
    ax_memset(gesture, 0, sizeof(*gesture));
    gesture->state = AXG_IDLE;
    gesture->pinch_factor = 100;
    gesture->pressure = AXG_PRESSURE_STEPS / 2;
}

/* Distance helpers avoid multiplication where a comparison will do: the squared
 * distance is only needed against a threshold, so the square root is skipped. */
static i32 distance_squared(i32 dx, i32 dy)
{
    return dx * dx + dy * dy;
}

/* ------------------------------------------------------------------ */
/* Sampling                                                            */
/* ------------------------------------------------------------------ */

/* Press begins a candidate.  The press position is remembered separately from
 * the current position so every threshold is measured from the origin, not from
 * the previous sample. */
axg_intent_t axg_press(axg_recogniser_t *gesture, i32 x, i32 y, i32 fingers,
                        i32 pressure)
{
    if (!gesture) return AXG_INTENT_NONE;
    gesture->state = AXG_PRESSED;
    gesture->start_x = x;
    gesture->start_y = y;
    gesture->last_x = x;
    gesture->last_y = y;
    gesture->total_x = 0;
    gesture->total_y = 0;
    gesture->ticks = 0;
    gesture->velocity_x = 0;
    gesture->velocity_y = 0;
    gesture->fingers = fingers < 1 ? 1 : fingers;
    gesture->pressure = pressure;
    gesture->pinch_active = (bool_t)0;
    gesture->pinch_factor = 100;
    return AXG_INTENT_NONE;
}

/* Move updates the velocity estimate with a one-pole filter.  A raw
 * last-minus-current difference is too noisy to fling from; a filtered value
 * gives the same feel with far fewer wild flings from a jittery sample. */
axg_intent_t axg_move(axg_recogniser_t *gesture, i32 x, i32 y, i32 fingers)
{
    if (!gesture) return AXG_INTENT_NONE;
    if (gesture->state == AXG_IDLE) return AXG_INTENT_NONE;

    i32 dx = x - gesture->last_x;
    i32 dy = y - gesture->last_y;
    gesture->total_x += dx;
    gesture->total_y += dy;
    gesture->velocity_x = (gesture->velocity_x * 3 + dx * 8) / 11;
    gesture->velocity_y = (gesture->velocity_y * 3 + dy * 8) / 11;
    gesture->last_x = x;
    gesture->last_y = y;
    gesture->ticks++;

    if (fingers >= 2 && gesture->fingers >= 2) {
        /* Two fingers moving apart is a pinch; the span is the squared distance
         * between the fingers, so a grow in either axis counts. */
        i32 span = (x - gesture->start_x) * (x - gesture->start_x) +
                   (y - gesture->start_y) * (y - gesture->start_y);
        if (!gesture->pinch_active && span > 400) {
            gesture->pinch_active = (bool_t)1;
            gesture->pinch_start_span = span;
            gesture->state = AXG_PINCHING;
            return AXG_INTENT_PINCH;
        }
        if (gesture->pinch_active) {
            gesture->pinch_factor =
                (span * 100) / (gesture->pinch_start_span > 0
                                    ? gesture->pinch_start_span : 1);
            return AXG_INTENT_PINCH;
        }
    }

    if (gesture->state == AXG_PRESSED) {
        if (distance_squared(gesture->total_x, gesture->total_y) >
            AXG_SCROLL_THRESHOLD * AXG_SCROLL_THRESHOLD) {
            gesture->state = AXG_SCROLLING;
        }
    }
    if (gesture->state == AXG_SCROLLING) {
        if (gesture->ticks > AXG_TAP_MAX_TICKS &&
            distance_squared(gesture->total_x, gesture->total_y) >
            AXG_SWIPE_MIN_DISTANCE * AXG_SWIPE_MIN_DISTANCE) {
            i32 cross = gesture->total_x < 0 ? -gesture->total_x
                                             : gesture->total_x;
            i32 vertical = gesture->total_y < 0 ? -gesture->total_y
                                                : gesture->total_y;
            if (cross > vertical * 3) {
                gesture->state = AXG_SWIPING;
            }
        }
        return AXG_INTENT_SCROLL;
    }
    if (gesture->state == AXG_SWIPING) return AXG_INTENT_NONE;
    return AXG_INTENT_NONE;
}

/* Release decides what the gesture meant.  A short press with almost no motion
 * is a tap, two quick taps are a double tap, and a flicked scroll becomes a
 * fling whose momentum carries on after the pointer lifts. */
axg_intent_t axg_release(axg_recogniser_t *gesture, i32 x, i32 y)
{
    if (!gesture) return AXG_INTENT_NONE;
    axg_intent_t intent = AXG_INTENT_NONE;

    if (gesture->pinch_active) {
        gesture->pinch_active = (bool_t)0;
        gesture->state = AXG_IDLE;
        return AXG_INTENT_PINCH;
    }

    if (gesture->state == AXG_SWIPING) {
        i32 horizontal = gesture->total_x < 0 ? -gesture->total_x
                                              : gesture->total_x;
        i32 vertical = gesture->total_y < 0 ? -gesture->total_y
                                            : gesture->total_y;
        if (vertical > horizontal) {
            intent = gesture->total_y < 0 ? AXG_INTENT_SWIPE_UP
                                          : AXG_INTENT_SWIPE_DOWN;
        } else {
            intent = gesture->total_x < 0 ? AXG_INTENT_SWIPE_LEFT
                                          : AXG_INTENT_SWIPE_RIGHT;
        }
    } else if (gesture->state == AXG_SCROLLING) {
        i32 speed = gesture->velocity_x < 0 ? -gesture->velocity_x
                                            : gesture->velocity_x;
        if (speed > AXG_FLING_MIN_VELOCITY) {
            gesture->momentum = (bool_t)1;
            gesture->momentum_x = gesture->velocity_x;
            gesture->momentum_y = gesture->velocity_y;
            intent = AXG_INTENT_FLING;
        } else {
            intent = AXG_INTENT_DRAG;
        }
    } else if (gesture->state == AXG_PRESSED &&
               distance_squared(gesture->total_x, gesture->total_y) <=
                   AXG_TAP_MAX_MOVE * AXG_TAP_MAX_MOVE) {
        i32 dx = x - gesture->last_tap_x;
        i32 dy = y - gesture->last_tap_y;
        if (gesture->tap_ticks > 0 && gesture->tap_ticks < 20 &&
            distance_squared(dx, dy) < 64 * 64) {
            intent = AXG_INTENT_DOUBLE_TAP;
            gesture->tap_ticks = 0;
        } else {
            intent = AXG_INTENT_TAP;
            gesture->last_tap_x = x;
            gesture->last_tap_y = y;
            gesture->tap_ticks = 1;
        }
    }
    gesture->state = intent == AXG_INTENT_FLING ? AXG_IDLE : AXG_RELEASED;
    return intent;
}

/* ------------------------------------------------------------------ */
/* Momentum                                                            */
/* ------------------------------------------------------------------ */

/* One momentum tick.  Returns the scroll delta to apply and whether motion
 * continues.  Decay is 90%, and motion stops once the velocity is below the
 * fling threshold; the tail of an exponential is invisible but keeps the
 * compositor awake. */
bool_t axg_momentum_tick(axg_recogniser_t *gesture, i32 *dx, i32 *dy)
{
    if (!gesture || !gesture->momentum) return (bool_t)0;
    gesture->momentum_x = (gesture->momentum_x * 9) / 10;
    gesture->momentum_y = (gesture->momentum_y * 9) / 10;
    if (dx) *dx = gesture->momentum_x;
    if (dy) *dy = gesture->momentum_y;
    i32 speed = gesture->momentum_x < 0 ? -gesture->momentum_x
                                        : gesture->momentum_x;
    i32 speed_y = gesture->momentum_y < 0 ? -gesture->momentum_y
                                          : gesture->momentum_y;
    if (speed < 2 && speed_y < 2) {
        gesture->momentum = (bool_t)0;
        gesture->momentum_x = 0;
        gesture->momentum_y = 0;
        return (bool_t)0;
    }
    return (bool_t)1;
}

void axg_stop_momentum(axg_recogniser_t *gesture)
{
    if (!gesture) return;
    gesture->momentum = (bool_t)0;
    gesture->momentum_x = 0;
    gesture->momentum_y = 0;
}

/* Pressure drives a small optical thickening in the shell's pressed states.  It
 * is quantised to four steps because a continuous value would cause a repaint
 * per sample for a difference nobody can see. */
i32 axg_pressure_step(const axg_recogniser_t *gesture)
{
    if (!gesture) return 0;
    i32 step = gesture->pressure / 32;
    if (step < 0) step = 0;
    if (step > AXG_PRESSURE_STEPS - 1) step = AXG_PRESSURE_STEPS - 1;
    return step;
}

/* Long press: a press held past a threshold without moving becomes a context
 * gesture instead of a tap.  The caller polls this; it is not returned from
 * move because it depends only on time. */
bool_t axg_is_long_press(const axg_recogniser_t *gesture)
{
    if (!gesture) return (bool_t)0;
    if (gesture->state != AXG_PRESSED) return (bool_t)0;
    if (gesture->ticks < 24) return (bool_t)0;
    return distance_squared(gesture->total_x, gesture->total_y) <=
           AXG_TAP_MAX_MOVE * AXG_TAP_MAX_MOVE;
}

/* Tick the tap-double-tap window so a stale first tap cannot pair with a tap
 * from a minute later. */
void axg_tick(axg_recogniser_t *gesture)
{
    if (!gesture) return;
    if (gesture->tap_ticks > 0) {
        gesture->tap_ticks++;
        if (gesture->tap_ticks > 20) {
            gesture->tap_ticks = 0;
        }
    }
    if (gesture->state == AXG_RELEASED) gesture->state = AXG_IDLE;
}

const char *axg_intent_name(axg_intent_t intent)
{
    switch (intent) {
    case AXG_INTENT_TAP: return "tap";
    case AXG_INTENT_DOUBLE_TAP: return "double-tap";
    case AXG_INTENT_SCROLL: return "scroll";
    case AXG_INTENT_FLING: return "fling";
    case AXG_INTENT_PINCH: return "pinch";
    case AXG_INTENT_SWIPE_LEFT: return "swipe-left";
    case AXG_INTENT_SWIPE_RIGHT: return "swipe-right";
    case AXG_INTENT_SWIPE_UP: return "swipe-up";
    case AXG_INTENT_SWIPE_DOWN: return "swipe-down";
    case AXG_INTENT_DRAG: return "drag";
    case AXG_INTENT_NONE:
    default: return "none";
    }
}

void axg_dump(const axg_recogniser_t *gesture)
{
    if (!gesture) return;
    printk("gesture: state=%d total=%d,%d velocity=%d,%d momentum=%d\n",
           (i32)gesture->state, gesture->total_x, gesture->total_y,
           gesture->velocity_x, gesture->velocity_y, gesture->momentum);
}

