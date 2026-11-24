#include "internal.h"
#include "codec.h"
#include "sequence.h"

#include "../arp.h"
#include "../net_platform.h"

enum {
    TCP_STREAM_NEIGHBOR_TIMEOUT_MS = 120000,
    TCP_CHALLENGE_ACK_INTERVAL_MS = 500
};

_Static_assert(TCP_QUEUE_CAPACITY <= USHRT_MAX, "receive window fits without scaling");
_Static_assert((int)TCP_IPV4_DEFAULT_MSS <= (int)TCP_IPV4_DATA_MAX,
               "the stream segment ceiling fits the IPv4 transmit buffer");

static unsigned int smaller(unsigned int left, unsigned int right)
{
    return left < right ? left : right;
}

void tcp_stream_start(struct tcp_connection *connection)
{
    struct tcp_stream *stream = &connection->stream;

    stream->snd_una = connection->open.iss + 1U;
    stream->rcv_nxt = connection->open.irs + 1U;
    stream->rcv_right = stream->rcv_nxt + TCP_QUEUE_CAPACITY;
    stream->window_seq = connection->open.irs;
    stream->window_ack = stream->snd_una;
    stream->peer_window = connection->open.peer_window;
}

unsigned int tcp_stream_write(struct tcp_connection *connection,
                               const void *data, unsigned int length)
{
    unsigned int copied = smaller(length, TCP_QUEUE_CAPACITY - connection->tx_length);

    tcp_ring_write(connection->tx_queue,
                    tcp_ring_offset(connection->tx_head, connection->tx_length),
                    data, copied);
    connection->tx_length += copied;
    return copied;
}

unsigned int tcp_stream_read(struct tcp_connection *connection,
                              void *data, unsigned int capacity)
{
    unsigned int copied = smaller(capacity, connection->rx_length);

    tcp_ring_read(connection->rx_queue, connection->rx_head, data, copied);
    connection->rx_head = tcp_ring_offset(connection->rx_head, copied);
    connection->rx_length -= copied;
    if (copied != 0U) {
        /* Free space is not receive credit until the update has been sent. */
        connection->stream.ack_pending = 1U;
    }
    return copied;
}

static unsigned int sendable_bytes(const struct tcp_connection *connection)
{
    /* A fixed one-MSS flight cap is temporary until the E5 sender exists. */
    unsigned int mss = smaller(connection->open.peer_mss, TCP_IPV4_DEFAULT_MSS);
    unsigned int limit = smaller(mss, connection->stream.peer_window);

    if (connection->tx_outstanding >= limit) {
        return 0U;
    }
    return smaller(limit - connection->tx_outstanding,
                   connection->tx_length - connection->tx_outstanding);
}

static void finish_challenge(struct tcp_stream *stream)
{
    stream->challenge_pending = 0U;
    stream->last_challenge_ms = net_clock_now_ms();
    stream->challenge_limited = 1U;
}

int tcp_stream_output(struct tcp_connection *connection)
{
    struct tcp_stream *stream = &connection->stream;
    unsigned int length = sendable_bytes(connection);
    int ordinary_output = length != 0U || stream->ack_pending;
    struct net_mac_addr mac;
    int result;

    if (!ordinary_output && !stream->challenge_pending) {
        if (stream->neighbor_pending) {
            /* The peer may have closed its window while a data send waited. */
            tcp_finish_neighbor(connection);
            stream->neighbor_pending = 0U;
        }
        return 0;
    }
    if (net_service_io_expired(&net_global_context)) {
        return 0;
    }
    if (!stream->neighbor_pending) {
        stream->neighbor_started_ms = net_clock_now_ms();
        stream->neighbor_pending = 1U;
    }
    result = tcp_resolve_neighbor(connection, stream->neighbor_started_ms,
                                  TCP_STREAM_NEIGHBOR_TIMEOUT_MS, &mac);
    if (result == NET_ARP_TASK_PENDING) {
        return 0;
    }
    if (result == NET_ERR_TIMEOUT ||
        (result == 0 &&
         net_timeout_expired(stream->neighbor_started_ms, net_clock_now_ms(),
                             TCP_STREAM_NEIGHBOR_TIMEOUT_MS))) {
        if (ordinary_output) {
            return tcp_connection_fail_event(TCP_ERR_NEIGHBOR_TIMEOUT);
        }
        /* An unconfirmed reset alone must not end a live connection. */
        tcp_finish_neighbor(connection);
        stream->neighbor_pending = 0U;
        finish_challenge(stream);
        return 1;
    }
    if (result != 0) {
        return result;
    }
    /* Resolution is complete even if the caller's budget defers raw output. */
    stream->neighbor_pending = 0U;
    if (stream->challenge_pending) {
        /* A challenge is an empty ACK, not a reason to send user data. */
        length = 0U;
    }

    struct tcp_segment_view segment = {
        .sequence = stream->snd_una + connection->tx_outstanding,
        .acknowledgment = stream->rcv_nxt,
        .window = (net_u16)(TCP_QUEUE_CAPACITY - connection->rx_length),
        .flags = TCP_FLAG_ACK,
        .payload_length = length
    };
    if (length != 0U && length == connection->tx_length - connection->tx_outstanding) {
        segment.flags |= TCP_FLAG_PSH;
    }
    result = tcp_transmit(connection, &mac, &segment, connection->tx_outstanding);
    if (result == NET_ETHERNET_TX_DEFERRED) {
        return 0;
    }
    if (result != 0) {
        return result;
    }
    connection->tx_outstanding += length;
    stream->rcv_right = segment.acknowledgment + segment.window;
    stream->ack_pending = 0U;
    if (stream->challenge_pending) {
        finish_challenge(stream);
    }
    return 1;
}

static int request_ack(struct tcp_connection *connection, int progress)
{
    int result;

    if (connection->stream.ack_pending) {
        return progress;
    }
    connection->stream.ack_pending = 1U;
    result = tcp_stream_output(connection);
    return result == 0 ? 1 : result;
}

static int request_reset_challenge(struct tcp_connection *connection, net_u32 sequence)
{
    struct tcp_stream *stream = &connection->stream;
    int result;

    if (!tcp_seq_in_range(sequence, stream->rcv_nxt, stream->rcv_right - stream->rcv_nxt) ||
        stream->challenge_pending ||
        (stream->challenge_limited &&
         !net_timeout_expired(stream->last_challenge_ms, net_clock_now_ms(),
                             TCP_CHALLENGE_ACK_INTERVAL_MS))) {
        return 0;
    }
    stream->challenge_pending = 1U;
    result = tcp_stream_output(connection);
    return result == 0 ? 1 : result;
}

static int receive_overlap(const struct tcp_stream *stream,
                            const struct tcp_segment_view *segment,
                            unsigned int length, unsigned int *prefix)
{
    unsigned int window = stream->rcv_right - stream->rcv_nxt;

    *prefix = 0U;
    if (window == 0U) {
        return length == 0U && segment->sequence == stream->rcv_nxt;
    }
    if (tcp_seq_in_range(segment->sequence, stream->rcv_nxt, window)) {
        return 1;
    }
    if (length != 0U &&
        tcp_seq_compare(segment->sequence, stream->rcv_nxt) == TCP_SEQ_BEFORE) {
        *prefix = stream->rcv_nxt - segment->sequence;
        return *prefix < length;
    }
    return 0;
}

static int apply_ack(struct tcp_connection *connection,
                     const struct tcp_segment_view *segment,
                     enum tcp_seq_order order, unsigned int acknowledged)
{
    struct tcp_stream *stream = &connection->stream;
    int progress = 0;

    if (order == TCP_SEQ_AFTER) {
        connection->tx_head = tcp_ring_offset(connection->tx_head, acknowledged);
        connection->tx_length -= acknowledged;
        connection->tx_outstanding -= acknowledged;
        stream->snd_una = segment->acknowledgment;
        progress = 1;
    }
    if (order != TCP_SEQ_BEFORE) {
        enum tcp_seq_order seq_order = tcp_seq_compare(segment->sequence, stream->window_seq);
        enum tcp_seq_order ack_order = tcp_seq_compare(segment->acknowledgment,
                                                       stream->window_ack);

        if (seq_order == TCP_SEQ_AFTER ||
            (seq_order == TCP_SEQ_EQUAL &&
             (ack_order == TCP_SEQ_EQUAL || ack_order == TCP_SEQ_AFTER))) {
            progress |= stream->peer_window != segment->window;
            stream->peer_window = segment->window;
            stream->window_seq = segment->sequence;
            stream->window_ack = segment->acknowledgment;
        }
    }
    return progress;
}

int tcp_stream_input(struct tcp_connection *connection,
                      const struct tcp_segment_view *segment)
{
    struct tcp_stream *stream = &connection->stream;
    enum tcp_seq_order order;
    unsigned int length, prefix, acknowledged;
    int progress;

    if ((segment->flags & TCP_FLAG_RST) != 0U) {
        return segment->sequence == stream->rcv_nxt ?
               tcp_connection_fail_event(TCP_ERR_RESET) :
               request_reset_challenge(connection, segment->sequence);
    }
    if ((segment->flags & TCP_FLAG_SYN) != 0U) {
        /* A lost handshake ACK can be replayed without resetting stream state. */
        return tcp_open_is_duplicate(connection, segment) ? request_ack(connection, 0) : 0;
    }
    if ((segment->flags & TCP_FLAG_ACK) == 0U) {
        return 0;
    }
    length = tcp_segment_sequence_length(segment);
    if (!receive_overlap(stream, segment, length, &prefix)) {
        return request_ack(connection, 0);
    }

    order = tcp_seq_compare(segment->acknowledgment, stream->snd_una);
    acknowledged = segment->acknowledgment - stream->snd_una;
    if (order == TCP_SEQ_UNORDERED ||
        (order == TCP_SEQ_AFTER && acknowledged > connection->tx_outstanding)) {
        return request_ack(connection, 0);
    }
    progress = apply_ack(connection, segment, order, acknowledged);
    if (length == 0U) {
        return progress;
    }
    if (segment->sequence + prefix == stream->rcv_nxt &&
        prefix < segment->payload_length) {
        unsigned int copied = smaller(segment->payload_length - prefix,
                                      stream->rcv_right - stream->rcv_nxt);

        copied = smaller(copied, TCP_QUEUE_CAPACITY - connection->rx_length);
        tcp_ring_write(connection->rx_queue,
                        tcp_ring_offset(connection->rx_head, connection->rx_length),
                        segment->payload + prefix, copied);
        connection->rx_length += copied;
        stream->rcv_nxt += copied;
        progress |= copied != 0U;
    }
    /* Drop gaps and the part outside the advertised window. FIN stays unconsumed
     * until E6, even when this segment supplied the last contiguous payload. */
    return request_ack(connection, progress);
}
