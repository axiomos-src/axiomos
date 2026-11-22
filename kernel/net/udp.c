/* AxiomOS - UDP sockets.
 *
 * Port based demultiplexing on top of the IPv4 layer, with a pseudo-header
 * checksum, a small socket table and a receive queue per socket.  No blocking
 * I/O: callers poll the queue, which is what the shell's DNS and NTP clients do.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXUD_MAX_SOCKETS    16
#define AXUD_QUEUE_DEPTH    8
#define AXUD_MAX_DATAGRAM   512
#define AXUD_EPHEMERAL_BASE 49152

typedef struct axud_datagram
{
    u32 source;
    u16 source_port;
    u16 length;
    u8  payload[AXUD_MAX_DATAGRAM];
} axud_datagram_t;

typedef struct axud_socket
{
    u16 port;
    u8  queue_head;
    u8  queue_count;
    u32 received;
    u32 dropped;
    u32 checksum_errors;
    axud_datagram_t queue[AXUD_QUEUE_DEPTH];
    bool_t bound;
    bool_t connected;
    u32 peer;
    u16 peer_port;
} axud_socket_t;

typedef struct axud_state
{
    axud_socket_t sockets[AXUD_MAX_SOCKETS];
    u8  count;
    u16 next_ephemeral;
    u32 sent;
    u32 delivered;
    u32 no_listener;
    u32 bad_checksum;
    bool_t ready;
} axud_state_t;

static axud_state_t axud_state;

static void axud_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u16 axud_checksum(u32 source, u32 destination, const u8 *segment, u32 length)
{
    u32 sum = 0;
    u32 i;
    sum += (source & 0xFFFF) + (source >> 16);
    sum += (destination & 0xFFFF) + (destination >> 16);
    sum += 17u;
    sum += length;
    for (i = 0; i + 1 < length; i += 2)
        sum += (u32)segment[i] | ((u32)segment[i + 1] << 8);
    if (length & 1)
        sum += (u32)segment[length - 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static axud_socket_t *axud_find(u16 port)
{
    u32 i;
    for (i = 0; i < axud_state.count; i++)
    {
        if (axud_state.sockets[i].bound && axud_state.sockets[i].port == port)
            return &axud_state.sockets[i];
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axud_init(void)
{
    axud_zero(&axud_state, (u32)sizeof(axud_state));
    axud_state.next_ephemeral = AXUD_EPHEMERAL_BASE;
    axud_state.ready = true;
}

u32 axud_open(u16 port)
{
    axud_socket_t *socket;
    if (axud_state.count >= AXUD_MAX_SOCKETS)
        return 0xFFFFFFFFu;
    if (port != 0 && axud_find(port) != NULL_PTR)
        return 0xFFFFFFFFu;
    socket = &axud_state.sockets[axud_state.count];
    axud_zero(socket, (u32)sizeof(*socket));
    if (port == 0)
    {
        port = axud_state.next_ephemeral;
        axud_state.next_ephemeral++;
        if (axud_state.next_ephemeral == 0)
            axud_state.next_ephemeral = AXUD_EPHEMERAL_BASE;
    }
    socket->port = port;
    socket->bound = true;
    axud_state.count++;
    return axud_state.count - 1;
}

bool_t axud_close(u32 handle)
{
    if (handle >= axud_state.count)
        return false;
    axud_state.sockets[handle].bound = false;
    return true;
}

bool_t axud_connect(u32 handle, u32 peer, u16 peer_port)
{
    if (handle >= axud_state.count)
        return false;
    axud_state.sockets[handle].connected = true;
    axud_state.sockets[handle].peer = peer;
    axud_state.sockets[handle].peer_port = peer_port;
    return true;
}

bool_t axud_send(u32 handle, u32 destination, u16 destination_port,
                 const u8 *payload, u32 length)
{
    u8 datagram[AXUD_MAX_DATAGRAM + 8];
    u16 checksum;
    u32 i;
    if (handle >= axud_state.count || payload == NULL_PTR)
        return false;
    if (length > AXUD_MAX_DATAGRAM)
        return false;
    if (length + 8 > AXIOM_ARRAY_LEN(datagram))
        return false;
    datagram[0] = (u8)(axud_state.sockets[handle].port & 0xFF);
    datagram[1] = (u8)((axud_state.sockets[handle].port >> 8) & 0xFF);
    datagram[2] = (u8)(destination_port & 0xFF);
    datagram[3] = (u8)((destination_port >> 8) & 0xFF);
    datagram[4] = (u8)((length + 8) & 0xFF);
    datagram[5] = (u8)(((length + 8) >> 8) & 0xFF);
    datagram[6] = 0;
    datagram[7] = 0;
    for (i = 0; i < length; i++)
        datagram[8 + i] = payload[i];
    checksum = axud_checksum(0, destination, datagram, length + 8);
    datagram[6] = (u8)(checksum & 0xFF);
    datagram[7] = (u8)((checksum >> 8) & 0xFF);
    axud_state.sent++;
    return true;
}

bool_t axud_receive(u32 source, u32 destination, const u8 *segment, u32 length)
{
    u16 source_port;
    u16 destination_port;
    axud_socket_t *socket;
    u32 i;
    if (segment == NULL_PTR || length < 8)
    {
        axud_state.bad_checksum++;
        return false;
    }
    source_port = (u16)((u32)segment[0] | ((u32)segment[1] << 8));
    destination_port = (u16)((u32)segment[2] | ((u32)segment[3] << 8));
    if (segment[6] != 0 || segment[7] != 0)
    {
        if (axud_checksum(source, destination, segment, length) != 0)
        {
            axud_state.bad_checksum++;
            return false;
        }
    }
    socket = axud_find(destination_port);
    if (socket == NULL_PTR)
    {
        axud_state.no_listener++;
        return false;
    }
    if (socket->queue_count >= AXUD_QUEUE_DEPTH)
    {
        socket->dropped++;
        return false;
    }
    {
        axud_datagram_t *slot = &socket->queue[
            (socket->queue_head + socket->queue_count) % AXUD_QUEUE_DEPTH];
        u32 payload_length = length - 8;
        if (payload_length > AXUD_MAX_DATAGRAM)
            payload_length = AXUD_MAX_DATAGRAM;
        slot->source = source;
        slot->source_port = source_port;
        slot->length = (u16)payload_length;
        for (i = 0; i < payload_length; i++)
            slot->payload[i] = segment[8 + i];
        socket->queue_count++;
    }
    socket->received++;
    axud_state.delivered++;
    return true;
}

const u8 *axud_peek(u32 handle, u16 *length, u16 *source_port)
{
    axud_socket_t *socket;
    axud_datagram_t *slot;
    if (handle >= axud_state.count)
        return NULL_PTR;
    socket = &axud_state.sockets[handle];
    if (socket->queue_count == 0)
        return NULL_PTR;
    slot = &socket->queue[socket->queue_head];
    *length = slot->length;
    *source_port = slot->source_port;
    return slot->payload;
}

void axud_consume(u32 handle)
{
    axud_socket_t *socket;
    if (handle >= axud_state.count)
        return;
    socket = &axud_state.sockets[handle];
    if (socket->queue_count == 0)
        return;
    socket->queue_head = (u8)((socket->queue_head + 1) % AXUD_QUEUE_DEPTH);
    socket->queue_count--;
}

u16 axud_port(u32 handle)
{
    if (handle >= axud_state.count)
        return 0;
    return axud_state.sockets[handle].port;
}

u32 axud_pending(u32 handle)
{
    if (handle >= axud_state.count)
        return 0;
    return axud_state.sockets[handle].queue_count;
}

u32 axud_socket_count(void)
{
    return axud_state.count;
}

void axud_report(void)
{
    u32 i;
    printk("axud: sockets=%u sent=%u delivered=%u no_listener=%u bad_csum=%u\n",
           (unsigned)axud_state.count, (unsigned)axud_state.sent,
           (unsigned)axud_state.delivered, (unsigned)axud_state.no_listener,
           (unsigned)axud_state.bad_checksum);
    for (i = 0; i < axud_state.count; i++)
    {
        const axud_socket_t *socket = &axud_state.sockets[i];
        if (!socket->bound)
            continue;
        printk("  port %u: received=%u pending=%u dropped=%u%s\n",
               (unsigned)socket->port, (unsigned)socket->received,
               (unsigned)socket->queue_count, (unsigned)socket->dropped,
               socket->connected ? " connected" : "");
    }
}

bool_t axud_selftest(void)
{
    u32 handle;
    u16 length = 0;
    u16 source_port = 0;
    u8 payload[16];
    u8 segment[32];
    u32 i;
    axud_init();
    handle = axud_open(5353);
    if (handle != 0)
        return false;
    if (axud_open(5353) != 0xFFFFFFFFu)
        return false;
    for (i = 0; i < 16; i++)
        payload[i] = (u8)(0x30 + i);
    if (!axud_send(handle, 0x08080808u, 53, payload, 16))
        return false;
    segment[0] = 53;
    segment[1] = 0;
    segment[2] = 0x85;
    segment[3] = 0x14;
    segment[4] = 0x18;
    segment[5] = 0;
    segment[6] = 0;
    segment[7] = 0;
    for (i = 0; i < 16; i++)
        segment[8 + i] = payload[i];
    if (!axud_receive(0x08080808u, 0x0A000001u, segment, 24))
        return false;
    if (axud_pending(handle) != 1)
        return false;
    {
        const u8 *data = axud_peek(handle, &length, &source_port);
        if (data == NULL_PTR || length != 16 || source_port != 53)
            return false;
        if (data[0] != 0x30 || data[15] != 0x3F)
            return false;
    }
    axud_consume(handle);
    if (axud_pending(handle) != 0)
        return false;
    if (axud_port(handle) != 5353)
        return false;
    if (!axud_close(handle))
        return false;
    if (axud_socket_count() != 1)
        return false;
    return true;
}
