/*
 * AxiomOS - animation primitives.
 *
 * The desktop needs exactly four kinds of motion: easing curves, scalar tweens,
 * springs, and short timelines.  All four are expressed over integer
 * milliseconds so the frame loop can advance them with the tick counter and
 * never needs floating point (the kernel boots with SSE disabled, so float
 * arithmetic is not an option below ring 3).
 *
 * Everything is a value type.  A widget holds its own axui_tween_t and calls
 * advance once per frame; there is no central animation registry and no
 * allocation.
 */

#include <axiom/lib.h>
#include "ui.h"

/* ------------------------------------------------------------------ */
/* Easing                                                              */
/* ------------------------------------------------------------------ */

/* Cubic Hermite helpers, all in 0..1000 fixed scale.  A curve maps a
 * normalised time t in 0..1000 to a normalised progress value. */
static i32 clamp01(i32 t)
{
    if (t < 0) {
        return 0;
    }
    if (t > 1000) {
        return 1000;
    }
    return t;
}

i32 axui_ease_linear(i32 t)
{
    return clamp01(t);
}

i32 axui_ease_in_quad(i32 t)
{
    t = clamp01(t);
    return (t * t) / 1000;
}

i32 axui_ease_out_quad(i32 t)
{
    t = clamp01(t);
    i32 inv = 1000 - t;
    return 1000 - (inv * inv) / 1000;
}

i32 axui_ease_in_out_quad(i32 t)
{
    t = clamp01(t);
    if (t < 500) {
        return (2 * t * t) / 1000;
    }
    i32 inv = 1000 - t;
    return 1000 - (2 * inv * inv) / 1000;
}

i32 axui_ease_in_cubic(i32 t)
{
    t = clamp01(t);
    return (t * t / 1000 * t) / 1000;
}

i32 axui_ease_out_cubic(i32 t)
{
    t = clamp01(t);
    i32 inv = 1000 - t;
    return 1000 - (inv * inv / 1000 * inv) / 1000;
}

i32 axui_ease_in_out_cubic(i32 t)
{
    t = clamp01(t);
    if (t < 500) {
        return (4 * t * t / 1000 * t) / 1000;
    }
    i32 inv = 1000 - t;
    i32 out = 1000 - (4 * inv * inv / 1000 * inv) / 1000;
    return 1000 - out;
}

/* A cubic-bezier approximation of the classic material standard curve
 * (0.4, 0, 0.2, 1).  Evaluated with four Newton steps, which is ample for
 * display work and keeps the code branch free. */
static i32 bezier_axis(i32 t, i32 p1, i32 p2)
{
    i32 u = 1000 - t;
    /* B(t) = 3u^2 t p1 + 3u t^2 p2 + t^3 */
    i64 a = (i64)3 * u * u / 1000 * t / 1000 * p1 / 1000;
    i64 b = (i64)3 * u * t / 1000 * t / 1000 * p2 / 1000;
    i64 c = (i64)t * t / 1000 * t / 1000;
    return (i32)(a + b + c);
}

i32 axui_ease_standard(i32 t)
{
    t = clamp01(t);
    if (t == 0 || t == 1000) {
        return t;
    }
    i32 guess = t;
    for (i32 i = 0; i < 4; i++) {
        i32 x = bezier_axis(guess, 400, 200);
        i32 error = x - t;
        if (error > -2 && error < 2) {
            break;
        }
        /* Derivative of the x-axis curve, approximated by a finite
         * difference of 8 units. */
        i32 x2 = bezier_axis(guess + 8, 400, 200);
        i32 slope = (x2 - x) / 8;
        if (slope == 0) {
            break;
        }
        guess -= error / slope;
        guess = clamp01(guess);
    }
    return bezier_axis(guess, 0, 1000);
}

/* Overshoot then settle: the curve goes past 1000 around t=700 and comes
 * back.  Implemented as a polynomial, not a real back-ease cubic, so the
 * peak is predictable. */
i32 axui_ease_out_back(i32 t)
{
    t = clamp01(t);
    i32 inv = 1000 - t;
    /* progress = 1 - (inv^2 * ((s+1) * inv - s)) with s = 1.7 */
    i64 s = 1700;
    i64 term = (i64)inv * inv / 1000;
    term = term * ((s + 1000) * inv / 1000 - s) / 1000;
    i32 value = 1000 - (i32)term;
    if (value < -200) {
        value = -200;
    }
    if (value > 1400) {
        value = 1400;
    }
    return value;
}

/* Damped oscillation for attention-grabbing elements such as a bell icon. */
i32 axui_ease_out_elastic(i32 t)
{
    t = clamp01(t);
    if (t == 0 || t == 1000) {
        return t;
    }
    /* Three decaying lobes, quantised on a triangular wave to avoid sin(). */
    i32 phase = ((t * 3) / 1000) % 2;    /* 0 or 1 within each lobe */
    i32 triangle = phase == 0 ? (t * 3) % 1000 : 1000 - (t * 3) % 1000;
    i32 decay = 1000 - t;
    i32 amplitude = (decay * decay) / 1000;
    i32 wave = (triangle - 500) * 2;      /* -1000..1000 */
    return 1000 + (wave * amplitude) / 1600;
}

/* Four bounces of decreasing height. */
i32 axui_ease_out_bounce(i32 t)
{
    t = clamp01(t);
    if (t < 700) {
        /* First descent: parabola. */
        i32 u = (t * 1000) / 700;
        return (u * u) / 1000;
    }
    if (t < 900) {
        i32 u = ((t - 700) * 1000) / 200;
        i32 h = 250 - (u * 250) / 1000;
        return 1000 - (u * u * h) / 1000 / 1000;
    }
    i32 u = (t - 900) * 10;
    if (u > 1000) {
        u = 1000;
    }
    return 1000 - ((1000 - u) * (1000 - u) * 62) / 1000 / 1000;
}

/* Step function: hold, then jump at each threshold. */
i32 axui_ease_steps(i32 t, i32 steps)
{
    if (steps < 1) {
        steps = 1;
    }
    t = clamp01(t);
    i32 index = (t * steps) / 1000;
    if (index > steps) {
        index = steps;
    }
    return (index * 1000) / steps;
}

/* Evaluate a curve by identifier, which is what the theme stores so a user
 * can pick "ease out" from a settings page. */
i32 axui_ease_by_id(i32 id, i32 t)
{
    switch (id) {
    case AXUI_EASE_LINEAR:
        return axui_ease_linear(t);
    case AXUI_EASE_IN_QUAD:
        return axui_ease_in_quad(t);
    case AXUI_EASE_OUT_QUAD:
        return axui_ease_out_quad(t);
    case AXUI_EASE_IN_OUT_QUAD:
        return axui_ease_in_out_quad(t);
    case AXUI_EASE_IN_CUBIC:
        return axui_ease_in_cubic(t);
    case AXUI_EASE_OUT_CUBIC:
        return axui_ease_out_cubic(t);
    case AXUI_EASE_STANDARD:
        return axui_ease_standard(t);
    case AXUI_EASE_OUT_BACK:
        return axui_ease_out_back(t);
    case AXUI_EASE_OUT_ELASTIC:
        return axui_ease_out_elastic(t);
    case AXUI_EASE_OUT_BOUNCE:
        return axui_ease_out_bounce(t);
    default:
        return axui_ease_linear(t);
    }
}

const char *axui_ease_name(i32 id)
{
    switch (id) {
    case AXUI_EASE_LINEAR:       return "linear";
    case AXUI_EASE_IN_QUAD:      return "in-quad";
    case AXUI_EASE_OUT_QUAD:     return "out-quad";
    case AXUI_EASE_IN_OUT_QUAD:  return "in-out-quad";
    case AXUI_EASE_IN_CUBIC:     return "in-cubic";
    case AXUI_EASE_OUT_CUBIC:    return "out-cubic";
    case AXUI_EASE_STANDARD:     return "standard";
    case AXUI_EASE_OUT_BACK:     return "out-back";
    case AXUI_EASE_OUT_ELASTIC:  return "out-elastic";
    case AXUI_EASE_OUT_BOUNCE:   return "out-bounce";
    default:                     return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Tween                                                               */
/* ------------------------------------------------------------------ */

void axui_tween_init(axui_tween_t *tween, i32 from, i32 to, i32 duration_ms,
                     i32 ease)
{
    if (!tween) {
        return;
    }
    tween->from = from;
    tween->to = to;
    tween->duration = duration_ms > 0 ? duration_ms : 1;
    tween->elapsed = duration_ms <= 0 ? duration_ms : 0;
    tween->ease = ease;
    tween->running = (bool_t)(duration_ms > 0);
    tween->value = from;
}

void axui_tween_to(axui_tween_t *tween, i32 target, i32 duration_ms)
{
    if (!tween) {
        return;
    }
    /* Retarget from the current position so an interrupted animation does not
     * jump back to its original start. */
    tween->from = tween->value;
    tween->to = target;
    tween->elapsed = 0;
    tween->duration = duration_ms > 0 ? duration_ms : 1;
    tween->running = (bool_t)1;
}

/* Advance by dt milliseconds; returns the value after the step. */
i32 axui_tween_advance(axui_tween_t *tween, i32 dt)
{
    if (!tween) {
        return 0;
    }
    if (tween->running) {
        tween->elapsed += dt;
        if (tween->elapsed >= tween->duration) {
            tween->elapsed = tween->duration;
            tween->running = (bool_t)0;
        }
        i32 t = (tween->elapsed * 1000) / tween->duration;
        i32 eased = axui_ease_by_id(tween->ease, t);
        tween->value = tween->from + ((tween->to - tween->from) * eased) / 1000;
    }
    return tween->value;
}

i32 axui_tween_value(const axui_tween_t *tween)
{
    return tween ? tween->value : 0;
}

bool_t axui_tween_done(const axui_tween_t *tween)
{
    return tween ? (bool_t)!tween->running : (bool_t)1;
}

void axui_tween_finish(axui_tween_t *tween)
{
    if (!tween) {
        return;
    }
    tween->elapsed = tween->duration;
    tween->value = tween->to;
    tween->running = (bool_t)0;
}

/* Progress in 0..1000, useful for driving colour lerps in step with a
 * scalar tween. */
i32 axui_tween_progress(const axui_tween_t *tween)
{
    if (!tween) {
        return 0;
    }
    if (tween->duration <= 0) {
        return 1000;
    }
    return (tween->elapsed * 1000) / tween->duration;
}

/* ------------------------------------------------------------------ */
/* Rect and colour tweening                                            */
/* ------------------------------------------------------------------ */

/* A four-channel tween for geometry; each field runs on its own scalar and
 * they all share the duration, so a resize stays rectangular throughout. */
void axui_rect_tween_init(axui_rect_tween_t *tween, gui_rect_t from,
                          i32 duration_ms, i32 ease)
{
    if (!tween) {
        return;
    }
    tween->duration = duration_ms;
    tween->ease = ease;
    axui_tween_init(&tween->x, from.x, from.x, duration_ms, ease);
    axui_tween_init(&tween->y, from.y, from.y, duration_ms, ease);
    axui_tween_init(&tween->w, from.w, from.w, duration_ms, ease);
    axui_tween_init(&tween->h, from.h, from.h, duration_ms, ease);
}

void axui_rect_tween_to(axui_rect_tween_t *tween, gui_rect_t to, i32 duration_ms)
{
    if (!tween) {
        return;
    }
    axui_tween_to(&tween->x, to.x, duration_ms);
    axui_tween_to(&tween->y, to.y, duration_ms);
    axui_tween_to(&tween->w, to.w, duration_ms);
    axui_tween_to(&tween->h, to.h, duration_ms);
}

gui_rect_t axui_rect_tween_advance(axui_rect_tween_t *tween, i32 dt)
{
    if (!tween) {
        return gui_rect(0, 0, 0, 0);
    }
    axui_tween_advance(&tween->x, dt);
    axui_tween_advance(&tween->y, dt);
    axui_tween_advance(&tween->w, dt);
    axui_tween_advance(&tween->h, dt);
    return gui_rect(tween->x.value, tween->y.value, tween->w.value, tween->h.value);
}

/* Colour interpolation in 0..1000 steps, so it can follow the same progress
 * value as a scalar tween. */
u32 axui_colour_tween(u32 from, u32 to, i32 progress)
{
    i32 t = progress < 0 ? 0 : (progress > 1000 ? 1000 : progress);
    return axui_lerp_colour(from, to, t / 10);
}

/* Alpha-only fade, which is what most window transitions need. */
u32 axui_fade_anim(u32 colour, i32 progress)
{
    i32 t = progress < 0 ? 0 : (progress > 1000 ? 1000 : progress);
    u32 alpha = (u32)((t * 255) / 1000);
    return axui_colour_alpha(colour, (u8)alpha);
}

/* ------------------------------------------------------------------ */
/* Spring                                                              */
/* ------------------------------------------------------------------ */

void axui_spring_init(axui_spring_t *spring, i32 value, i32 stiffness, i32 damping)
{
    if (!spring) {
        return;
    }
    spring->value = value;
    spring->target = value;
    spring->velocity = 0;
    spring->stiffness = stiffness > 0 ? stiffness : 180;
    spring->damping = damping > 0 ? damping : 22;
}

void axui_spring_to(axui_spring_t *spring, i32 target)
{
    if (spring) {
        spring->target = target;
    }
}

/* One integration step.  The unit is a 60 Hz frame; the stiffness values are
 * tuned for that cadence.  Velocity is expressed in 1/16 pixel per frame to
 * keep the whole system in integers. */
i32 axui_spring_step(axui_spring_t *spring, i32 frames)
{
    if (!spring) {
        return 0;
    }
    for (i32 i = 0; i < frames; i++) {
        /* displacement in sixteenths (s16), rest in s16. */
        i32 displacement = (spring->target - spring->value) * 16;
        i32 force = (displacement * spring->stiffness) / 1000;
        /* Damping acts on velocity, in the same scale. */
        i32 damping = (spring->velocity * spring->damping) / 1000;
        spring->velocity += force - damping;
        /* Clamp runaway oscillation, which a badly tuned spring can produce. */
        if (spring->velocity > 5120) {
            spring->velocity = 5120;
        }
        if (spring->velocity < -5120) {
            spring->velocity = -5120;
        }
        spring->value += spring->velocity / 128;

        if (displacement > -16 && displacement < 16 && spring->velocity > -8 &&
            spring->velocity < 8) {
            spring->value = spring->target;
            spring->velocity = 0;
            break;
        }
    }
    return spring->value;
}

bool_t axui_spring_settled(const axui_spring_t *spring)
{
    return spring ? (bool_t)(spring->value == spring->target && spring->velocity == 0)
                  : (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Timeline                                                            */
/* ------------------------------------------------------------------ */

/* A timeline is a list of stages, each a duration and a callback.  The
 * desktop uses it for the boot reveal: panel in, taskbar up, widgets
 * staggered.  Stages are values in a fixed array to keep it allocation free. */
void axui_timeline_init(axui_timeline_t *timeline, bool_t loop)
{
    if (!timeline) {
        return;
    }
    ax_memset(timeline, 0, sizeof(*timeline));
    timeline->loop = loop;
}

i32 axui_timeline_add(axui_timeline_t *timeline, i32 duration_ms,
                      axui_stage_fn_t fn, void *context)
{
    if (!timeline || timeline->count >= AXUI_TIMELINE_MAX) {
        return -1;
    }
    i32 index = timeline->count++;
    timeline->stages[index].duration = duration_ms > 0 ? duration_ms : 1;
    timeline->stages[index].fn = fn;
    timeline->stages[index].context = context;
    return index;
}

/* Advance the timeline; returns the stage index that is active after the
 * step, or -1 when the timeline has finished. */
i32 axui_timeline_advance(axui_timeline_t *timeline, i32 dt)
{
    if (!timeline || timeline->paused) {
        return timeline->current;
    }
    i32 guard = 0;
    while (timeline->current >= 0 && timeline->current < timeline->count && guard++ < 64) {
        axui_timeline_stage_t *stage = &timeline->stages[timeline->current];
        if (!stage->done) {
            stage->elapsed += dt;
            dt = 0;     /* only the first stage in a chain absorbs the dt */
            if (stage->elapsed >= stage->duration) {
                stage->done = (bool_t)1;
                if (stage->fn) {
                    stage->fn(stage->context, 1000);
                }
            } else {
                if (!stage->started) {
                    stage->started = (bool_t)1;
                    if (stage->fn) {
                        stage->fn(stage->context, 0);
                    }
                }
                if (stage->fn) {
                    i32 progress = (stage->elapsed * 1000) / stage->duration;
                    stage->fn(stage->context, progress);
                }
                return timeline->current;
            }
        }
        timeline->current++;
        if (timeline->current >= timeline->count) {
            if (timeline->loop) {
                for (i32 i = 0; i < timeline->count; i++) {
                    timeline->stages[i].elapsed = 0;
                    timeline->stages[i].started = (bool_t)0;
                    timeline->stages[i].done = (bool_t)0;
                }
                timeline->current = 0;
            } else {
                timeline->current = -1;
            }
        }
    }
    return timeline->current;
}

bool_t axui_timeline_done(const axui_timeline_t *timeline)
{
    if (!timeline) {
        return (bool_t)1;
    }
    return (bool_t)(timeline->current < 0 || timeline->current >= timeline->count);
}

void axui_timeline_reset(axui_timeline_t *timeline)
{
    if (!timeline) {
        return;
    }
    for (i32 i = 0; i < timeline->count; i++) {
        timeline->stages[i].elapsed = 0;
        timeline->stages[i].started = (bool_t)0;
        timeline->stages[i].done = (bool_t)0;
    }
    timeline->current = 0;
}

/* ------------------------------------------------------------------ */
/* Frame pacing                                                        */
/* ------------------------------------------------------------------ */

/* The desktop targets 60 Hz.  A frame budget helper keeps the paint loop from
 * overruning a slow frame: widgets ask for the elapsed milliseconds since the
 * previous frame and clamp them, so one long frame does not teleport every
 * animation to its end. */
void axui_clock_init(axui_clock_t *clock)
{
    if (!clock) {
        return;
    }
    ax_memset(clock, 0, sizeof(*clock));
}

/* Advance the clock and return the clamped delta in milliseconds. */
i32 axui_clock_advance(axui_clock_t *clock, u64 ticks)
{
    if (!clock) {
        return 16;
    }
    i64 delta = (i64)ticks - (i64)clock->last_tick;
    clock->last_tick = ticks;
    if (delta <= 0) {
        delta = 16;
    }
    if (delta > 48) {
        /* More than three frames: pretend one frame elapsed, but count it. */
        clock->dropped += (i32)(delta / 16) - 1;
        delta = 16;
    }
    clock->last_delta = (i32)delta;
    clock->frames++;
    return clock->last_delta;
}

/* Smoothed frames per second, computed once per second worth of frames. */
i32 axui_clock_fps(const axui_clock_t *clock)
{
    if (!clock || clock->last_delta <= 0) {
        return 60;
    }
    return 1000 / clock->last_delta;
}
