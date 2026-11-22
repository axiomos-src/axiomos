/* AxiomOS - SNTP client and clock discipline.
 *
 * The kernel keeps the RTC as the wall clock and the TSC as the monotonic clock.
 * This module asks an SNTP server for the offset once per boot and, optionally,
 * once per day, then feeds a simple proportional controller that slews the
 * correction instead of stepping it so timers never jump backwards.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXNTP_PORT            123
#define AXNTP_MAX_SERVERS     4
#define AXNTP_MAX_SAMPLES     8
#define AXNTP_HEADER_BYTES     48
#define AXNTP_EPOCH_DELTA      2208988800ull
#define AXNTP_MS_PER_DAY       86400000ull
#define AXNTP_SLEW_PPM_MAX     500
#define AXNTP_POLL_INTERVAL_MS 3600000ull
#define AXNTP_TIMEOUT_MS       1500

#define AXNTP_MODE_CLIENT     3
#define AXNTP_MODE_SERVER     4
#define AXNTP_LI_NONE         0
#define AXNTP_LI_LEAP         1
#define AXNTP_LI_UNSYNC       3

#define AXNTP_STATE_IDLE      0
#define AXNTP_STATE_REQUEST   1
#define AXNTP_STATE_SAMPLING  2
#define AXNTP_STATE_LOCKED    3
#define AXNTP_STATE_ERROR     4

typedef struct axntp_server
{
    u32  address;
    u32  requests;
    u32  replies;
    u32  timeouts;
    u32  last_offset_ms;
    u32  last_round_trip_ms;
    u16  port;
    u8   stratum;
    u8   generation;
    bool_t reachable;
    bool_t preferred;
} axntp_server_t;

typedef struct axntp_sample
{
    u64  client_send_ms;
    u64  server_receive_ms;
    u64  server_send_ms;
    u64  client_receive_ms;
    i64  offset_ms;
    u32  round_trip_ms;
    bool_t valid;
} axntp_sample_t;

typedef struct axntp_state
{
    axntp_server_t servers[AXNTP_MAX_SERVERS];
    axntp_sample_t samples[AXNTP_MAX_SAMPLES];
    u64  last_poll_ms;
    u64  next_poll_ms;
    i64  offset_ms;
    i64  slew_per_ms;
    i64  residual_ms;
    u32  server_count;
    u32  sample_count;
    u32  polls;
    u32  failures;
    u32  leaps;
    u32  rejections;
    u32  discipline_steps;
    u32  now_ms;
    u32  last_stratum;
    u32  best_server;
    u32  state;
    bool_t locked;
    bool_t polling;
} axntp_state_t;

static axntp_state_t axntp_state;

static void axntp_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u8 axntp_abs_diff_u8(u8 left, u8 right)
{
    return left > right ? (u8)(left - right) : (u8)(right - left);
}

/* NTP timestamps are seconds since 1900 with a 32 bit fraction. */
static u64 axntp_timestamp_to_ms(u32 seconds, u32 fraction)
{
    u64 seconds_since_1970;
    u64 milliseconds;
    if (seconds < AXNTP_EPOCH_DELTA)
        return 0;
    seconds_since_1970 = (u64)(seconds - AXNTP_EPOCH_DELTA);
    milliseconds = ((u64)fraction * 1000ull) >> 32;
    return seconds_since_1970 * 1000ull + milliseconds;
}

static u32 axntp_read_u32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | (u32)data[3];
}

static void axntp_write_u32(u8 *data, u32 value)
{
    data[0] = (u8)(value >> 24);
    data[1] = (u8)(value >> 16);
    data[2] = (u8)(value >> 8);
    data[3] = (u8)value;
}

/* ------------------------------------------------------------- packet build */

u32 axntp_build_request(u8 *packet, u32 capacity, u32 transmit_seconds, u32 transmit_fraction)
{
    if (packet == NULL_PTR || capacity < AXNTP_HEADER_BYTES)
        return 0;
    for (u32 i = 0; i < AXNTP_HEADER_BYTES; i++)
        packet[i] = 0;
    packet[0] = (u8)((AXNTP_LI_NONE << 6) | (0 << 3) | AXNTP_MODE_CLIENT);
    packet[1] = 0;
    packet[2] = 6;
    packet[3] = 0xEC;
    packet[8] = 0;
    packet[9] = 0;
    packet[10] = 0;
    packet[11] = 1;
    axntp_write_u32(packet + 40, transmit_seconds);
    axntp_write_u32(packet + 44, transmit_fraction);
    return AXNTP_HEADER_BYTES;
}

bool_t axntp_parse_reply(const u8 *packet, u32 length, axntp_sample_t *sample,
                         const axntp_server_t *server)
{
    u32 mode;
    u32 stratum;
    u32 leap;
    u32 originating;
    u32 reference;
    u64 origin_ms;
    u64 receive_ms;
    u64 transmit_ms;
    u64 destination_ms;
    i64 offset;
    u64 round_trip;
    if (packet == NULL_PTR || sample == NULL_PTR || length < AXNTP_HEADER_BYTES)
        return false;
    leap = (packet[0] >> 6) & 0x03;
    mode = packet[0] & 0x07;
    stratum = packet[1];
    if (mode != AXNTP_MODE_SERVER)
    {
        axntp_state.rejections++;
        return false;
    }
    if (stratum == 0 || stratum > 15)
    {
        axntp_state.rejections++;
        return false;
    }
    if (leap == AXNTP_LI_UNSYNC)
    {
        axntp_state.rejections++;
        return false;
    }
    reference = axntp_read_u32(packet + 4);
    originating = axntp_read_u32(packet + 24);
    origin_ms = axntp_timestamp_to_ms(axntp_read_u32(packet + 16), axntp_read_u32(packet + 20));
    receive_ms = axntp_timestamp_to_ms(axntp_read_u32(packet + 32), axntp_read_u32(packet + 36));
    transmit_ms = axntp_timestamp_to_ms(axntp_read_u32(packet + 40), axntp_read_u32(packet + 44));
    destination_ms = sample->client_receive_ms;
    if (sample->client_send_ms == 0 || destination_ms <= sample->client_send_ms)
        return false;
    offset = (i64)(receive_ms - sample->client_send_ms) + (i64)(transmit_ms - destination_ms);
    offset = offset / 2;
    round_trip = (u64)((destination_ms - sample->client_send_ms) - (transmit_ms - receive_ms));
    sample->server_receive_ms = receive_ms;
    sample->server_send_ms = transmit_ms;
    sample->offset_ms = offset;
    sample->round_trip_ms = (u32)round_trip;
    sample->valid = true;
    AXIOM_UNUSED_VAR(server);
    AXIOM_UNUSED_VAR(reference);
    AXIOM_UNUSED_VAR(originating);
    AXIOM_UNUSED_VAR(axntp_abs_diff_u8);
    return true;
}

/* -------------------------------------------------------------- discipline */

void axntp_init(void)
{
    axntp_zero(&axntp_state, (u32)sizeof(axntp_state));
    axntp_state.state = AXNTP_STATE_IDLE;
    axntp_state.next_poll_ms = AXNTP_POLL_INTERVAL_MS;
}

bool_t axntp_add_server(u32 address, u16 port)
{
    if (axntp_state.server_count >= AXNTP_MAX_SERVERS || address == 0)
        return false;
    axntp_server_t *server = &axntp_state.servers[axntp_state.server_count];
    axntp_zero(server, (u32)sizeof(*server));
    server->address = address;
    server->port = port == 0 ? AXNTP_PORT : port;
    server->stratum = 15;
    server->generation = (u8)axntp_state.server_count;
    server->preferred = axntp_state.server_count == 0;
    axntp_state.server_count++;
    return true;
}

bool_t axntp_start_poll(u64 now_ms)
{
    if (axntp_state.server_count == 0)
        return false;
    if (axntp_state.polling)
        return false;
    axntp_state.polling = true;
    axntp_state.state = AXNTP_STATE_REQUEST;
    axntp_state.last_poll_ms = now_ms;
    axntp_state.polls++;
    return true;
}

bool_t axntp_record_sample(u32 server_index, i64 offset_ms, u32 round_trip_ms)
{
    if (server_index >= axntp_state.server_count)
        return false;
    if (offset_ms > 86400000ll || offset_ms < -86400000ll)
    {
        axntp_state.rejections++;
        return false;
    }
    axntp_server_t *server = &axntp_state.servers[server_index];
    server->replies++;
    server->reachable = true;
    server->last_offset_ms = (u32)(offset_ms < 0 ? -offset_ms : offset_ms);
    server->last_round_trip_ms = round_trip_ms;
    for (u32 i = 0; i < axntp_state.sample_count; i++)
    {
        axntp_sample_t *existing = &axntp_state.samples[i];
        if (!existing->valid)
            continue;
        if (existing->offset_ms == offset_ms && existing->round_trip_ms == round_trip_ms)
            return true;
    }
    u32 slot = axntp_state.sample_count % AXNTP_MAX_SAMPLES;
    axntp_sample_t *sample = &axntp_state.samples[slot];
    sample->offset_ms = offset_ms;
    sample->round_trip_ms = round_trip_ms;
    sample->valid = true;
    axntp_state.sample_count++;
    if (!axntp_state.locked && axntp_state.sample_count >= 2)
    {
        axntp_state.offset_ms = offset_ms;
        axntp_state.locked = true;
        axntp_state.state = AXNTP_STATE_LOCKED;
        axntp_state.discipline_steps++;
        axntp_state.best_server = server_index;
    }
    return true;
}

/* Median of the collected offsets ignores a single bad sample and one outlier
 * caused by asymmetric routing. */
i64 axntp_median_offset(void)
{
    i64 values[AXNTP_MAX_SAMPLES];
    u32 count = 0;
    for (u32 i = 0; i < axntp_state.sample_count && count < AXNTP_MAX_SAMPLES; i++)
    {
        if (axntp_state.samples[i].valid)
            values[count++] = axntp_state.samples[i].offset_ms;
    }
    if (count == 0)
        return 0;
    for (u32 i = 0; i + 1 < count; i++)
    {
        for (u32 j = 0; j + 1 < count - i; j++)
        {
            if (values[j] > values[j + 1])
            {
                i64 temp = values[j];
                values[j] = values[j + 1];
                values[j + 1] = temp;
            }
        }
    }
    return values[count / 2];
}

void axntp_discipline(u64 now_ms)
{
    i64 median;
    axntp_state.now_ms = (u32)now_ms;
    if (!axntp_state.locked)
        return;
    median = axntp_median_offset();
    axntp_state.residual_ms = median - axntp_state.offset_ms;
    if (median == axntp_state.offset_ms)
        return;
    if (median > axntp_state.offset_ms)
    {
        /* Slew forward, at a bounded rate so monotonic time never jumps. */
        i64 step = (median - axntp_state.offset_ms) / 8;
        if (step <= 0)
            step = 1;
        if (step > AXNTP_SLEW_PPM_MAX)
            step = AXNTP_SLEW_PPM_MAX;
        axntp_state.slew_per_ms = step;
        axntp_state.offset_ms += step;
    }
    else
    {
        i64 step = (axntp_state.offset_ms - median) / 8;
        if (step <= 0)
            step = 1;
        if (step > AXNTP_SLEW_PPM_MAX)
            step = AXNTP_SLEW_PPM_MAX;
        axntp_state.slew_per_ms = -step;
        axntp_state.offset_ms -= step;
    }
    axntp_state.discipline_steps++;
}

u64 axntp_corrected_ms(u64 monotonic_ms)
{
    i64 corrected = (i64)monotonic_ms + axntp_state.offset_ms;
    return corrected < 0 ? 0 : (u64)corrected;
}

u64 axntp_corrected_seconds(u64 monotonic_ms)
{
    return axntp_corrected_ms(monotonic_ms) / 1000ull;
}

void axntp_failover(u32 failed_index)
{
    if (failed_index >= axntp_state.server_count)
        return;
    axntp_state.servers[failed_index].reachable = false;
    axntp_state.servers[failed_index].timeouts++;
    axntp_state.failures++;
    for (u32 i = 0; i < axntp_state.server_count; i++)
    {
        u32 candidate = (failed_index + 1 + i) % axntp_state.server_count;
        if (!axntp_state.servers[candidate].reachable)
            continue;
        axntp_state.best_server = candidate;
        return;
    }
    axntp_state.best_server = (failed_index + 1) % axntp_state.server_count;
}

void axntp_expire(u64 now_ms)
{
    if (!axntp_state.polling)
    {
        if (now_ms >= axntp_state.next_poll_ms)
            axntp_start_poll(now_ms);
        return;
    }
    if (now_ms < axntp_state.last_poll_ms)
        return;
    if (now_ms - axntp_state.last_poll_ms > AXNTP_TIMEOUT_MS)
    {
        axntp_state.polling = false;
        axntp_state.next_poll_ms = now_ms + AXNTP_POLL_INTERVAL_MS;
        axntp_failover(axntp_state.best_server);
        if (!axntp_state.locked)
            axntp_state.state = AXNTP_STATE_ERROR;
    }
}

bool_t axntp_locked(void)
{
    return axntp_state.locked;
}

i64 axntp_offset(void)
{
    return axntp_state.offset_ms;
}

i64 axntp_slew(void)
{
    return axntp_state.slew_per_ms;
}

u32 axntp_state_code(void)
{
    return axntp_state.state;
}

u32 axntp_server_count(void)
{
    return axntp_state.server_count;
}

u32 axntp_quality(void)
{
    u32 best = 0xFFFFFFFFu;
    for (u32 i = 0; i < axntp_state.sample_count && i < AXNTP_MAX_SAMPLES; i++)
    {
        if (!axntp_state.samples[i].valid)
            continue;
        if (axntp_state.samples[i].round_trip_ms < best)
            best = axntp_state.samples[i].round_trip_ms;
    }
    return best == 0xFFFFFFFFu ? 0 : best;
}

void axntp_store_leap(bool_t leap_second, u32 direction)
{
    if (!leap_second)
        return;
    axntp_state.leaps++;
    if (direction != 0)
        axntp_state.offset_ms += direction > 0 ? 1 : -1;
}

void axntp_report(void)
{
    printk("axntp: servers=%u locked=%u state=%u offset=%d slew=%d rtt=%u\n",
           (unsigned)axntp_state.server_count, (unsigned)axntp_state.locked,
           (unsigned)axntp_state.state, (int)axntp_state.offset_ms,
           (int)axntp_state.slew_per_ms, (unsigned)axntp_quality());
    printk("axntp: polls=%u failures=%u rejections=%u steps=%u leaps=%u samples=%u\n",
           (unsigned)axntp_state.polls, (unsigned)axntp_state.failures,
           (unsigned)axntp_state.rejections, (unsigned)axntp_state.discipline_steps,
           (unsigned)axntp_state.leaps, (unsigned)axntp_state.sample_count);
    for (u32 i = 0; i < axntp_state.server_count; i++)
    {
        const axntp_server_t *server = &axntp_state.servers[i];
        printk("  server %u addr=0x%x port=%u replies=%u timeouts=%u rtt=%u\n",
               (unsigned)i, (unsigned)server->address, (unsigned)server->port,
               (unsigned)server->replies, (unsigned)server->timeouts,
               (unsigned)server->last_round_trip_ms);
    }
}

bool_t axntp_selftest(void)
{
    u8 packet[64];
    axntp_sample_t sample;
    axntp_init();
    if (!axntp_add_server(0x0A000002u, 123))
        return false;
    if (!axntp_add_server(0x0A000003u, 123))
        return false;
    if (axntp_server_count() != 2)
        return false;
    if (axntp_add_server(0, 0))
        return false;
    if (axntp_build_request(packet, 64, 3900000000u, 0) != AXNTP_HEADER_BYTES)
        return false;
    if ((packet[0] & 0x07) != AXNTP_MODE_CLIENT)
        return false;
    if (packet[2] != 6)
        return false;
    if (!axntp_parse_reply(packet, 48, &sample, NULL_PTR))
        return false;
    axntp_zero(&sample, (u32)sizeof(sample));
    sample.client_send_ms = 1000;
    sample.client_receive_ms = 1400;
    for (u32 i = 0; i < AXNTP_HEADER_BYTES; i++)
        packet[i] = 0;
    packet[0] = (u8)((AXNTP_LI_NONE << 6) | AXNTP_MODE_SERVER);
    packet[1] = 2;
    axntp_write_u32(packet + 16, 0);
    axntp_write_u32(packet + 20, 0);
    axntp_write_u32(packet + 32, (u32)(AXNTP_EPOCH_DELTA + 1));
    axntp_write_u32(packet + 36, 0x80000000u);
    axntp_write_u32(packet + 40, (u32)(AXNTP_EPOCH_DELTA + 1));
    axntp_write_u32(packet + 44, 0x80000000u);
    if (!axntp_parse_reply(packet, 48, &sample, NULL_PTR))
        return false;
    if (sample.offset_ms != 600)
        return false;
    if (sample.round_trip_ms != 400)
        return false;
    if (!axntp_record_sample(0, 600, 400))
        return false;
    if (!axntp_record_sample(0, 600, 400))
        return false;
    if (!axntp_record_sample(1, 580, 410))
        return false;
    if (!axntp_locked())
        return false;
    if (axntp_state_code() != AXNTP_STATE_LOCKED)
        return false;
    if (axntp_median_offset() < 450)
        return false;
    axntp_record_sample(1, 1200, 380);
    axntp_discipline(5000);
    if (axntp_slew() == 0)
        return false;
    if (axntp_corrected_ms(1000) <= 1000)
        return false;
    if (axntp_corrected_seconds(1000) == 0)
        return false;
    axntp_failover(0);
    if (axntp_state.failures == 0)
        return false;
    axntp_expire(9000);
    if (axntp_state.polling == false && axntp_state.next_poll_ms == 0)
        return false;
    axntp_store_leap(true, 1);
    if (axntp_state.leaps == 0)
        return false;
    if (axntp_quality() == 0)
        return false;
    return true;
}

