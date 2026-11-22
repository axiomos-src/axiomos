/*
 * AxiomOS - mouse gestures.
 *
 * A raw mouse stream is a series of positions; what the shell wants is
 * intent.  This module turns positions into the small vocabulary a window
 * system needs: single click, double click, drag, wheel scroll, and edge
 * scroll.  All of it is derived from timestamps supplied by the caller, which
 * keeps the logic testable without a clock.
 *
 * Thresholds are in pixels and milliseconds and are exposed as fields so a
 * future preferences panel can change them without touching the code.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axinput.h>

#define GESTURE_DOUBLE_MS 350
#define GESTURE_TRIPLE_MS 600
#define GESTURE_DRAG_PX 4
#define GESTURE_CLICK_PX 3

typedef enum gesture_kind {
    GESTURE_NONE = 0,
    GESTURE_CLICK,
    GESTURE_DOUBLE_CLICK,
    GESTURE_TRIPLE_CLICK,
    GESTURE_DRAG_START,
    GESTURE_DRAG_MOVE,
    GESTURE_DRAG_END,
    GESTURE_WHEEL_UP,
    GESTURE_WHEEL_DOWN,
    GESTURE_EDGE_SCROLL
} gesture_kind_t;

typedef struct mouse_gesture_config {
    u32 double_click_ms;
    u32 triple_click_ms;
    u32 drag_threshold_px;
    u32 click_slop_px;
    u32 edge_margin_px;
    bool_t edge_scroll;
    bool_t natural_scroll;
} mouse_gesture_config_t;

typedef struct mouse_gesture_state {
    mouse_gesture_config_t config;
    i32 press_x;
    i32 press_y;
    i32 last_x;
    i32 last_y;
    u32 last_button;
    u64 press_time_ns;
    u64 last_release_ns;
    u32 click_streak;
    bool_t dragging;
    bool_t drag_moved;
    i32 drag_origin_x;
    i32 drag_origin_y;
    u32 gestures;
    u32 drag_distance;
} mouse_gesture_state_t;

typedef struct mouse_gesture_event {
    gesture_kind_t kind;
    u32 button;
    i32 x;
    i32 y;
    i32 dx;
    i32 dy;
    u64 timestamp_ns;
} mouse_gesture_event_t;

static mouse_gesture_state_t g_gestures;

void mouse_gestures_init(void)
{
    ax_memset(&g_gestures, 0, sizeof(g_gestures));
    g_gestures.config.double_click_ms = GESTURE_DOUBLE_MS;
    g_gestures.config.triple_click_ms = GESTURE_TRIPLE_MS;
    g_gestures.config.drag_threshold_px = GESTURE_DRAG_PX;
    g_gestures.config.click_slop_px = GESTURE_CLICK_PX;
    g_gestures.config.edge_margin_px = 8;
    g_gestures.config.edge_scroll = (bool_t)1;
}

const mouse_gesture_config_t *mouse_gestures_config(void)
{
    return &g_gestures.config;
}

void mouse_gestures_configure(const mouse_gesture_config_t *config)
{
    if (!config) return;
    g_gestures.config = *config;
    if (g_gestures.config.double_click_ms == 0) {
        g_gestures.config.double_click_ms = GESTURE_DOUBLE_MS;
    }
    if (g_gestures.config.drag_threshold_px == 0) {
        g_gestures.config.drag_threshold_px = GESTURE_DRAG_PX;
    }
}

static u32 distance_squared(i32 ax, i32 ay, i32 bx, i32 by)
{
    i32 dx = ax - bx;
    i32 dy = ay - by;
    return (u32)(dx * dx + dy * dy);
}

/* Feed a motion event.  Returns nonzero and fills `out` when the motion crossed
 * a threshold that changes intent (drag start or drag move). */
i32 mouse_gestures_motion(i32 x, i32 y, u32 buttons, u64 now_ns,
                          mouse_gesture_event_t *out)
{
    (void)now_ns;
    g_gestures.last_x = x;
    g_gestures.last_y = y;

    if (!(buttons & AXMOUSE_LEFT)) {
        if (g_gestures.dragging) {
            /* Button released without a release event reaching us. */
            g_gestures.dragging = (bool_t)0;
        }
        return 0;
    }

    if (!g_gestures.dragging) {
        u32 threshold = g_gestures.config.drag_threshold_px;
        if (distance_squared(x, y, g_gestures.press_x, g_gestures.press_y)
            >= threshold * threshold) {
            g_gestures.dragging = (bool_t)1;
            g_gestures.drag_moved = (bool_t)1;
            g_gestures.drag_origin_x = g_gestures.press_x;
            g_gestures.drag_origin_y = g_gestures.press_y;
            g_gestures.gestures++;
            g_gestures.drag_distance = 0;
            if (out) {
                ax_memset(out, 0, sizeof(*out));
                out->kind = GESTURE_DRAG_START;
                out->button = AXMOUSE_LEFT;
                out->x = x;
                out->y = y;
                out->dx = 0;
                out->dy = 0;
                out->timestamp_ns = now_ns;
            }
            return 1;
        }
        return 0;
    }

    i32 dx = x - g_gestures.last_x;
    i32 dy = y - g_gestures.last_y;
    g_gestures.drag_distance += (u32)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
    if (out) {
        ax_memset(out, 0, sizeof(*out));
        out->kind = GESTURE_DRAG_MOVE;
        out->button = AXMOUSE_LEFT;
        out->x = x;
        out->y = y;
        out->dx = dx;
        out->dy = dy;
        out->timestamp_ns = now_ns;
    }
    return 1;
}

/* Feed a button press.  Returns 1 when the press begins a drag candidate. */
i32 mouse_gestures_press(i32 x, i32 y, u32 button, u64 now_ns,
                         mouse_gesture_event_t *out)
{
    g_gestures.press_x = x;
    g_gestures.press_y = y;
    g_gestures.last_button = button;
    g_gestures.press_time_ns = now_ns;
    g_gestures.dragging = (bool_t)0;
    g_gestures.drag_moved = (bool_t)0;
    (void)out;
    return 1;
}

/* Feed a button release and classify it as a click, double or triple.  A drag
 * that moved beyond the slop is reported as a drag end instead. */
i32 mouse_gestures_release(i32 x, i32 y, u32 button, u64 now_ns,
                           mouse_gesture_event_t *out)
{
    if (button != AXMOUSE_LEFT) {
        if (out) {
            ax_memset(out, 0, sizeof(*out));
            out->kind = GESTURE_CLICK;
            out->button = button;
            out->x = x;
            out->y = y;
            out->timestamp_ns = now_ns;
        }
        return 1;
    }

    if (g_gestures.dragging) {
        g_gestures.dragging = (bool_t)0;
        g_gestures.dragging = (bool_t)0;
        if (out) {
            ax_memset(out, 0, sizeof(*out));
            out->kind = GESTURE_DRAG_END;
            out->button = button;
            out->x = x;
            out->y = y;
            out->dx = x - g_gestures.drag_origin_x;
            out->dy = y - g_gestures.drag_origin_y;
            out->timestamp_ns = now_ns;
        }
        g_gestures.gestures++;
        g_gestures.click_streak = 0;
        return 1;
    }

    u32 slop = g_gestures.config.click_slop_px;
    if (distance_squared(x, y, g_gestures.press_x, g_gestures.press_y) > slop * slop) {
        /* Press and release in different places but never crossed the drag
         * threshold: treat as a click at the release point. */
    }

    u64 since_last = now_ns - g_gestures.last_release_ns;
    if (g_gestures.click_streak > 0 &&
        since_last <= (u64)g_gestures.config.double_click_ms * 1000000ULL) {
        g_gestures.click_streak++;
    } else {
        g_gestures.click_streak = 1;
    }
    g_gestures.last_release_ns = now_ns;

    gesture_kind_t kind = GESTURE_CLICK;
    if (g_gestures.click_streak == 2) {
        kind = GESTURE_DOUBLE_CLICK;
    } else if (g_gestures.click_streak >= 3 &&
               since_last <= (u64)g_gestures.config.triple_click_ms * 1000000ULL) {
        kind = GESTURE_TRIPLE_CLICK;
        g_gestures.click_streak = 0;
    }

    if (out) {
        ax_memset(out, 0, sizeof(*out));
        out->kind = kind;
        out->button = button;
        out->x = x;
        out->y = y;
        out->timestamp_ns = now_ns;
    }
    g_gestures.gestures++;
    return 1;
}

/* Classify a wheel notch.  Natural scrolling flips the sign, which is what a
 * trackpad user expects and a mouse user does not. */
i32 mouse_gestures_wheel(i32 delta, mouse_gesture_event_t *out)
{
    if (delta == 0) {
        return 0;
    }
    if (g_gestures.config.natural_scroll) {
        delta = -delta;
    }
    if (out) {
        ax_memset(out, 0, sizeof(*out));
        out->kind = delta > 0 ? GESTURE_WHEEL_UP : GESTURE_WHEEL_DOWN;
        out->dy = delta;
    }
    g_gestures.gestures++;
    return 1;
}

/* Edge scrolling: a pointer held within the margin of a scrollable surface
 * produces a scroll delta proportional to how deep into the margin it is. */
i32 mouse_gestures_edge_scroll(i32 x, i32 y, i32 width, i32 height,
                               mouse_gesture_event_t *out)
{
    if (!g_gestures.config.edge_scroll) {
        return 0;
    }
    i32 margin = (i32)g_gestures.config.edge_margin_px;
    i32 delta_x = 0;
    i32 delta_y = 0;
    if (x < margin) delta_x = -(margin - x);
    if (x > width - margin) delta_x = x - (width - margin);
    if (y < margin) delta_y = -(margin - y);
    if (y > height - margin) delta_y = y - (height - margin);
    if (delta_x == 0 && delta_y == 0) {
        return 0;
    }
    if (out) {
        ax_memset(out, 0, sizeof(*out));
        out->kind = GESTURE_EDGE_SCROLL;
        out->dx = delta_x;
        out->dy = delta_y;
    }
    return 1;
}

const char *mouse_gesture_name(gesture_kind_t kind)
{
    switch (kind) {
    case GESTURE_CLICK: return "click";
    case GESTURE_DOUBLE_CLICK: return "double-click";
    case GESTURE_TRIPLE_CLICK: return "triple-click";
    case GESTURE_DRAG_START: return "drag-start";
    case GESTURE_DRAG_MOVE: return "drag-move";
    case GESTURE_DRAG_END: return "drag-end";
    case GESTURE_WHEEL_UP: return "wheel-up";
    case GESTURE_WHEEL_DOWN: return "wheel-down";
    case GESTURE_EDGE_SCROLL: return "edge-scroll";
    default: return "none";
    }
}

u32 mouse_gesture_count(void)
{
    return g_gestures.gestures;
}

bool_t mouse_gestures_dragging(void)
{
    return g_gestures.dragging;
}

u32 mouse_gestures_drag_distance(void)
{
    return g_gestures.drag_distance;
}

void mouse_gestures_reset(void)
{
    mouse_gesture_config_t config = g_gestures.config;
    ax_memset(&g_gestures, 0, sizeof(g_gestures));
    g_gestures.config = config;
}

void mouse_gestures_dump(void)
{
    printk("gestures: %u total, dragging=%d distance=%u streak=%u\n",
           g_gestures.gestures, g_gestures.dragging, g_gestures.drag_distance,
           g_gestures.click_streak);
}
