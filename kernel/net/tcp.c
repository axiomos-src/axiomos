/* AxiomOS - TCP connection management.
 *
 * Tracks connections in a simple table with the classic state machine, but
 * without retransmission timers: each segment is acknowledged synchronously by
 * the caller, which is enough for the shell's HTTP and the update checker.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXTC_MAX_CONNECTIONS 16
#define AXTC_TX_BUFFER       1024
#define AXTC_RX_BUFFER       2048
#define AXTC_WINDOW          4096

enum axtc_state
{
    AXTC_STATE_CLOSED = 0,
    AXTC_STATE_LISTEN,
    AXTC_STATE_SYN_SENT,
    AXTC_STATE_SYN_RECEIVED,
    AXTC_STATE_ESTABLISHED,
    AXTC_STATE_FIN_WAIT,
    AXTC_STATE_CLOSE_WAIT,
    AXTC_STATE_LAST_ACK,
    AXTC_STATE_TIME_WAIT
};

typedef struct axtc_header
{
    u16 source_port;
    u16 destination_port;
    u32 sequence;
    u32 acknowledgement;
    u8  data_offset;
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urgent;
} axtc_header_t;

#define AXTC_FIN 0x01
#define AXTC_SYN 0x02
#define AXTC_RST 0x04
#define AXTC_PSH 0x08
#define AXTC_ACK 0x10

typedef struct axtc_connection
{
    u32 local;
    u32 remote;
    u16 local_port;
    u16 remote_port;
    u32 sequence;
    u32 acknowledgement;
    u8  state;
    u8  tx_length;
    u16 window;
    u32 retransmits;
    u32 bytes_in;
    u32 bytes_out;
    u32 resets;
    u8  tx_buffer[AXTC_TX_BUFFER];
    u8  rx_buffer[AXTC_RX_BUFFER];
    u16 rx_length;
    bool_t in_use;
} axtc_connection_t;

typedef struct axtc_layer
{
    axtc_connection_t connections[AXTC_MAX_CONNECTIONS];
    u8   count;
    u32  segments_in;
    u32  segments_out;
    u32  checksum_errors;
    u32  refused;
    bool_t ready;
} axtc_layer_t;

static axtc_layer_t axtc_layer;

static void axtc_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u16 axtc_checksum(u32 source, u32 destination, const u8 *segment, u32 length)
{
    u32 sum = 0;
    u32 i;
    sum += (source & 0xFFFF) + (source >> 16);
    sum += (destination & 0xFFFF) + (destination >> 16);
    sum += 6u;
    sum += length;
    for (i = 0; i + 1 < length; i += 2)
        sum += (u32)segment[i] | ((u32)segment[i + 1] << 8);
    if (length & 1)
        sum += (u32)segment[length - 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static axtc_connection_t *axtc_find(u16 local_port, u16 remote_port, u32 remote)
{
    u32 i;
    for (i = 0; i < AXTC_MAX_CONNECTIONS; i++)
    {
        axtc_connection_t *connection = &axtc_layer.connections[i];
        if (!connection->in_use)
            continue;
        if (connection->local_port == local_port &&
            connection->remote_port == remote_port &&
            connection->remote == remote)
            return connection;
    }
    return NULL_PTR;
}

static axtc_connection_t *axtc_slot(void)
{
    u32 i;
    for (i = 0; i < AXTC_MAX_CONNECTIONS; i++)
    {
        if (!axtc_layer.connections[i].in_use)
            return &axtc_layer.connections[i];
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axtc_init(void)
{
    axtc_zero(&axtc_layer, (u32)sizeof(axtc_layer));
    axtc_layer.ready = true;
}

u32 axtc_listen(u16 port)
{
    axtc_connection_t *connection = axtc_slot();
    if (connection == NULL_PTR)
        return 0xFFFFFFFFu;
    axtc_zero(connection, (u32)sizeof(*connection));
    connection->local_port = port;
    connection->state = AXTC_STATE_LISTEN;
    connection->window = AXTC_WINDOW;
    connection->in_use = true;
    axtc_layer.count++;
    return (u32)(connection - axtc_layer.connections);
}

u32 axtc_connect(u32 local, u16 local_port, u32 remote, u16 remote_port)
{
    axtc_connection_t *connection = axtc_slot();
    if (connection == NULL_PTR)
        return 0xFFFFFFFFu;
    axtc_zero(connection, (u32)sizeof(*connection));
    connection->local = local;
    connection->remote = remote;
    connection->local_port = local_port;
    connection->remote_port = remote_port;
    connection->sequence = 0x1000;
    connection->state = AXTC_STATE_SYN_SENT;
    connection->window = AXTC_WINDOW;
    connection->in_use = true;
    axtc_layer.count++;
    axtc_layer.segments_out++;
    return (u32)(connection - axtc_layer.connections);
}

u8 axtc_state(u32 handle)
{
    if (handle >= AXTC_MAX_CONNECTIONS)
        return AXTC_STATE_CLOSED;
    return axtc_layer.connections[handle].state;
}

bool_t axtc_send(u32 handle, const u8 *payload, u32 length)
{
    axtc_connection_t *connection;
    u32 i;
    if (handle >= AXTC_MAX_CONNECTIONS || payload == NULL_PTR)
        return false;
    connection = &axtc_layer.connections[handle];
    if (!connection->in_use || connection->state != AXTC_STATE_ESTABLISHED)
        return false;
    if (length > AXTC_TX_BUFFER - connection->tx_length)
        return false;
    for (i = 0; i < length; i++)
        connection->tx_buffer[connection->tx_length + i] = payload[i];
    connection->tx_length = (u8)(connection->tx_length + (u8)length);
    connection->bytes_out += length;
    connection->sequence += length;
    axtc_layer.segments_out++;
    return true;
}

bool_t axtc_available_data(u32 handle, const u8 **data, u16 *length)
{
    axtc_connection_t *connection;
    if (handle >= AXTC_MAX_CONNECTIONS)
        return false;
    connection = &axtc_layer.connections[handle];
    if (!connection->in_use || connection->rx_length == 0)
        return false;
    *data = connection->rx_buffer;
    *length = connection->rx_length;
    return true;
}

void axtc_consume(u32 handle, u16 length)
{
    axtc_connection_t *connection;
    u16 remaining;
    u16 i;
    if (handle >= AXTC_MAX_CONNECTIONS)
        return;
    connection = &axtc_layer.connections[handle];
    if (length >= connection->rx_length)
    {
        connection->rx_length = 0;
        return;
    }
    remaining = (u16)(connection->rx_length - length);
    for (i = 0; i < remaining; i++)
        connection->rx_buffer[i] = connection->rx_buffer[length + i];
    connection->rx_length = remaining;
}

bool_t axtc_receive(u32 source, u32 destination, const u8 *segment, u32 length)
{
    const axtc_header_t *header;
    axtc_connection_t *connection;
    u32 header_length;
    u32 payload_length;
    u32 i;
    if (segment == NULL_PTR || length < 20)
    {
        axtc_layer.checksum_errors++;
        return false;
    }
    header = (const axtc_header_t *)segment;
    header_length = (u32)(header->data_offset >> 4) * 4;
    if (header_length < 20 || header_length > length)
        return false;
    if (header->checksum != 0 &&
        axtc_checksum(source, destination, segment, length) != 0)
    {
        axtc_layer.checksum_errors++;
        return false;
    }
    axtc_layer.segments_in++;
    payload_length = length - header_length;
    connection = axtc_find(header->destination_port, header->source_port, source);
    if (connection == NULL_PTR)
    {
        axtc_layer.refused++;
        return false;
    }
    if (header->flags & AXTC_RST)
    {
        connection->state = AXTC_STATE_CLOSED;
        connection->resets++;
        connection->in_use = false;
        return true;
    }
    switch (connection->state)
    {
    case AXTC_STATE_SYN_SENT:
        if (header->flags & AXTC_SYN)
        {
            connection->acknowledgement = header->sequence + 1;
            connection->state = AXTC_STATE_ESTABLISHED;
        }
        break;
    case AXTC_STATE_LISTEN:
        if (header->flags & AXTC_SYN)
        {
            connection->remote = source;
            connection->remote_port = header->source_port;
            connection->acknowledgement = header->sequence + 1;
            connection->state = AXTC_STATE_SYN_RECEIVED;
        }
        break;
    case AXTC_STATE_ESTABLISHED:
        if (payload_length > 0)
        {
            if (connection->rx_length + payload_length > AXTC_RX_BUFFER)
                return false;
            for (i = 0; i < payload_length; i++)
                connection->rx_buffer[connection->rx_length + i] =
                    segment[header_length + i];
            connection->rx_length = (u16)(connection->rx_length + payload_length);
            connection->bytes_in += payload_length;
            connection->acknowledgement = header->sequence + payload_length;
        }
        if (header->flags & AXTC_FIN)
        {
            connection->acknowledgement++;
            connection->state = AXTC_STATE_CLOSE_WAIT;
        }
        break;
    default:
        break;
    }
    return true;
}

bool_t axtc_close(u32 handle)
{
    axtc_connection_t *connection;
    if (handle >= AXTC_MAX_CONNECTIONS)
        return false;
    connection = &axtc_layer.connections[handle];
    if (!connection->in_use)
        return false;
    connection->state = AXTC_STATE_FIN_WAIT;
    axtc_layer.segments_out++;
    connection->in_use = false;
    if (axtc_layer.count > 0)
        axtc_layer.count--;
    return true;
}

u32 axtc_count(void)
{
    return axtc_layer.count;
}

void axtc_report(void)
{
    u32 i;
    printk("axtc: connections=%u in=%u out=%u bad_csum=%u refused=%u\n",
           (unsigned)axtc_layer.count, (unsigned)axtc_layer.segments_in,
           (unsigned)axtc_layer.segments_out, (unsigned)axtc_layer.checksum_errors,
           (unsigned)axtc_layer.refused);
    for (i = 0; i < AXTC_MAX_CONNECTIONS; i++)
    {
        const axtc_connection_t *connection = &axtc_layer.connections[i];
        if (!connection->in_use)
            continue;
        printk("  %u -> %u:%u state=%u rx=%u bytes=%u/%u\n",
               (unsigned)connection->local_port, (unsigned)connection->remote,
               (unsigned)connection->remote_port, (unsigned)connection->state,
               (unsigned)connection->rx_length, (unsigned)connection->bytes_in,
               (unsigned)connection->bytes_out);
    }
}

bool_t axtc_selftest(void)
{
    u32 handle;
    u8 segment[64];
    u8 payload[8];
    u32 i;
    axtc_init();
    handle = axtc_connect(0x0A000001u, 40000, 0x0A000002u, 80);
    if (handle == 0xFFFFFFFFu)
        return false;
    if (axtc_state(handle) != AXTC_STATE_SYN_SENT)
        return false;
    axtc_zero(segment, 64);
    segment[0] = 80;
    segment[1] = 0;
    segment[2] = 0x40;
    segment[3] = 0x9C;
    segment[8] = 0x00;
    segment[9] = 0x00;
    segment[10] = 0x01;
    segment[11] = 0x00;
    segment[12] = 0x50;
    segment[13] = 0x02;
    segment[14] = 0x10;
    segment[15] = 0x00;
    if (!axtc_receive(0x0A000002u, 0x0A000001u, segment, 20))
        return false;
    if (axtc_state(handle) != AXTC_STATE_ESTABLISHED)
        return false;
    for (i = 0; i < 8; i++)
        payload[i] = (u8)(0x41 + i);
    if (!axtc_send(handle, payload, 8))
        return false;
    axtc_zero(segment, 64);
    segment[0] = 80;
    segment[1] = 0;
    segment[2] = 0x40;
    segment[3] = 0x9C;
    segment[12] = 0x50;
    segment[13] = 0x18;
    segment[14] = 0x10;
    segment[15] = 0x00;
    segment[20] = 'o';
    segment[21] = 'k';
    if (!axtc_receive(0x0A000002u, 0x0A000001u, segment, 22))
        return false;
    if (axtc_layer.connections[handle].rx_length != 2)
        return false;
    axtc_consume(handle, 2);
    if (axtc_layer.connections[handle].rx_length != 0)
        return false;
    if (!axtc_close(handle))
        return false;
    return true;
}
