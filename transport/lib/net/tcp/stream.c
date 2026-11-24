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
    tcp_rto_init(&stream->rto, connection->open.short_syn_timeout);
}

unsigned int tcp_stream_write(struct tcp_connection *connection,
                               const void *data, unsigned int length, net_u32 now_ms)
{
    unsigned int copied = smaller(length, TCP_QUEUE_CAPACITY - connection->tx_length);

    if (connection->tx_length == 0U && copied != 0U) {
        connection->transmit_started_ms = now_ms;
    }
    tcp_ring_write(connection->tx_queue,
                    tcp_ring_offset(connection->tx_head, connection->tx_length),
                    data, copied);
    connection->tx_length += copied;
    return copied;
}

int tcp_stream_transmit_expired(const struct tcp_connection *connection, net_u32 now_ms)
{
    return connection->tx_length != 0U &&
           net_timeout_expired(connection->transmit_started_ms, now_ms,
                               connection->config.transmit_timeout_ms);
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

static unsigned int effective_mss(const struct tcp_connection *connection)
{
    return smaller(connection->open.peer_mss, TCP_IPV4_DEFAULT_MSS);
}

static unsigned int sendable_bytes(const struct tcp_connection *connection)
{
    /* Keep a fixed one-MSS flight cap until congestion control is added. */
    unsigned int limit = smaller(effective_mss(connection), connection->stream.peer_window);

    if (connection->tx_outstanding >= limit) {
        return 0U;
    }
    return smaller(limit - connection->tx_outstanding,
                   connection->tx_length - connection->tx_outstanding);
}

static int retry_bytes(const struct tcp_connection *connection, net_u32 now_ms)
{
    const struct tcp_stream *stream = &connection->stream;
    const struct tcp_rto *rto = &stream->rto;

    /* Do not consume data retries against a closed window. Probing is separate. */
    if (connection->tx_outstanding == 0U || stream->peer_window == 0U ||
        !net_timeout_expired(rto->timer_started_ms, now_ms, rto->wait_ms)) {
        return 0;
    }
    if (rto->retries >= TCP_DATA_RETRY_LIMIT) {
        return TCP_ERR_RETRIES;
    }
    /* The timed prefix excludes any bytes first sent after the timer was armed. */
    return (int)smaller(rto->timer_right - stream->snd_una,
                        smaller(stream->peer_window, effective_mss(connection)));
}

int tcp_stream_after_receive(const struct tcp_connection *connection)
{
    net_u32 now_ms;

    if (connection->stream.rto.retries < TCP_DATA_RETRY_LIMIT ||
        connection->tx_outstanding == 0U || connection->stream.peer_window == 0U) {
        return 0;
    }
    now_ms = net_clock_now_ms();
    if (tcp_stream_transmit_expired(connection, now_ms)) {
        return tcp_connection_fail_event(TCP_ERR_TRANSMIT_TIMEOUT);
    }
    return retry_bytes(connection, now_ms) < 0 ?
           tcp_connection_fail_event(TCP_ERR_RETRIES) : 0;
}

static void finish_stream_neighbor(struct tcp_connection *connection)
{
    tcp_finish_neighbor(connection);
    connection->stream.neighbor_pending = 0U;
}

static void finish_challenge(struct tcp_stream *stream)
{
    stream->challenge_pending = 0U;
    stream->last_challenge_ms = net_clock_now_ms();
    stream->challenge_limited = 1U;
}

int tcp_stream_output(struct tcp_connection *connection, net_u32 now_ms)
{
    struct tcp_stream *stream = &connection->stream;
    struct net_mac_addr mac;
    unsigned int length, offset;
    int result, retry, ordinary_output;

    if (tcp_stream_transmit_expired(connection, now_ms)) {
        return tcp_connection_fail_event(TCP_ERR_TRANSMIT_TIMEOUT);
    }
    retry = retry_bytes(connection, now_ms);
    if (retry < 0) {
        /* A queued ACK may make the final expiry obsolete. Recheck after input. */
        return 0;
    }
    length = retry != 0 ? (unsigned int)retry : sendable_bytes(connection);
    ordinary_output = length != 0U || stream->ack_pending;

    if (!ordinary_output && !stream->challenge_pending) {
        if (stream->neighbor_pending) {
            /* The peer may have closed its window while a data send waited. */
            finish_stream_neighbor(connection);
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
    if (result == 0) {
        now_ms = net_clock_now_ms();
        if (tcp_stream_transmit_expired(connection, now_ms)) {
            return tcp_connection_fail_event(TCP_ERR_TRANSMIT_TIMEOUT);
        }
    }
    if (result == NET_ERR_TIMEOUT ||
        (result == 0 &&
         net_timeout_expired(stream->neighbor_started_ms, now_ms,
                             TCP_STREAM_NEIGHBOR_TIMEOUT_MS))) {
        if (ordinary_output) {
            return tcp_connection_fail_event(TCP_ERR_NEIGHBOR_TIMEOUT);
        }
        /* An unconfirmed reset alone must not end a live connection. */
        finish_stream_neighbor(connection);
        finish_challenge(stream);
        return 1;
    }
    if (result != 0) {
        return result;
    }
    /* Resolution is complete even if the caller's budget defers raw output. */
    stream->neighbor_pending = 0U;
    retry = retry_bytes(connection, now_ms);
    if (retry < 0) {
        return 0;
    }
    offset = connection->tx_outstanding;
    if (stream->challenge_pending) {
        /* A challenge is an empty ACK, not a reason to send user data. */
        length = 0U;
    } else if (retry != 0) {
        offset = 0U;
        length = (unsigned int)retry;
    }

    struct tcp_segment_view segment = {
        .sequence = stream->snd_una + offset,
        .acknowledgment = stream->rcv_nxt,
        .window = (net_u16)(TCP_QUEUE_CAPACITY - connection->rx_length),
        .flags = TCP_FLAG_ACK,
        .payload_length = length
    };
    if (length != 0U && length == connection->tx_length - offset) {
        segment.flags |= TCP_FLAG_PSH;
    }
    struct net_tx_deadline deadline = {
        .started_ms = connection->transmit_started_ms,
        .timeout_ms = connection->config.transmit_timeout_ms
    };
    result = tcp_transmit(connection, &mac, &segment, offset,
                           connection->tx_length != 0U ? &deadline : 0);
    if (result == NET_ETHERNET_TX_EXPIRED) {
        return tcp_connection_fail_event(TCP_ERR_TRANSMIT_TIMEOUT);
    }
    if (result == NET_ETHERNET_TX_DEFERRED) {
        return 0;
    }
    if (result != 0) {
        return result;
    }
    if (length != 0U) {
        net_u32 sent_ms = net_clock_now_ms();

        /* Only a successful raw send changes the flight and timing state. */
        if (retry != 0) {
            tcp_rto_retransmitted(&stream->rto,
                                   stream->snd_una + connection->tx_outstanding, sent_ms);
        } else {
            connection->tx_outstanding += length;
            tcp_rto_new_data(&stream->rto, stream->snd_una + connection->tx_outstanding,
                              sent_ms, offset == 0U);
        }
    }
    stream->rcv_right = segment.acknowledgment + segment.window;
    stream->ack_pending = 0U;
    if (stream->challenge_pending) {
        finish_challenge(stream);
    }
    return 1;
}

static int request_ack(struct tcp_connection *connection, int progress, net_u32 now_ms)
{
    int result;

    if (connection->stream.ack_pending) {
        return progress;
    }
    connection->stream.ack_pending = 1U;
    result = tcp_stream_output(connection, now_ms);
    return result == 0 ? 1 : result;
}

static int request_reset_challenge(struct tcp_connection *connection, net_u32 sequence,
                                    net_u32 now_ms)
{
    struct tcp_stream *stream = &connection->stream;
    int result;

    if (!tcp_seq_in_range(sequence, stream->rcv_nxt, stream->rcv_right - stream->rcv_nxt) ||
        stream->challenge_pending ||
        (stream->challenge_limited &&
         !net_timeout_expired(stream->last_challenge_ms, now_ms,
                             TCP_CHALLENGE_ACK_INTERVAL_MS))) {
        return 0;
    }
    stream->challenge_pending = 1U;
    result = tcp_stream_output(connection, now_ms);
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
                     enum tcp_seq_order order, unsigned int acknowledged, net_u32 now_ms)
{
    struct tcp_stream *stream = &connection->stream;
    int progress = 0;

    if (order == TCP_SEQ_AFTER) {
        connection->tx_head = tcp_ring_offset(connection->tx_head, acknowledged);
        connection->tx_length -= acknowledged;
        connection->tx_outstanding -= acknowledged;
        stream->snd_una = segment->acknowledgment;
        connection->transmit_started_ms = connection->tx_length != 0U ? now_ms : 0U;
        tcp_rto_acknowledged(&stream->rto, stream->snd_una, connection->tx_outstanding, now_ms);
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
    net_u32 now_ms = net_clock_now_ms();
    enum tcp_seq_order order;
    unsigned int length, prefix, acknowledged;
    int progress;

    if (tcp_stream_transmit_expired(connection, now_ms)) {
        return tcp_connection_fail_event(TCP_ERR_TRANSMIT_TIMEOUT);
    }
    if ((segment->flags & TCP_FLAG_RST) != 0U) {
        return segment->sequence == stream->rcv_nxt ?
               tcp_connection_fail_event(TCP_ERR_RESET) :
               request_reset_challenge(connection, segment->sequence, now_ms);
    }
    if ((segment->flags & TCP_FLAG_SYN) != 0U) {
        /* A lost handshake ACK can be replayed without resetting stream state. */
        return tcp_open_is_duplicate(connection, segment) ?
               request_ack(connection, 0, now_ms) : 0;
    }
    if ((segment->flags & TCP_FLAG_ACK) == 0U) {
        return 0;
    }
    length = tcp_segment_sequence_length(segment);
    if (!receive_overlap(stream, segment, length, &prefix)) {
        return request_ack(connection, 0, now_ms);
    }

    order = tcp_seq_compare(segment->acknowledgment, stream->snd_una);
    acknowledged = segment->acknowledgment - stream->snd_una;
    if (order == TCP_SEQ_UNORDERED ||
        (order == TCP_SEQ_AFTER && acknowledged > connection->tx_outstanding)) {
        return request_ack(connection, 0, now_ms);
    }
    progress = apply_ack(connection, segment, order, acknowledged, now_ms);
    if (length == 0U) {
        if (stream->neighbor_pending && !stream->ack_pending &&
            !stream->challenge_pending && sendable_bytes(connection) == 0U &&
            retry_bytes(connection, now_ms) <= 0) {
            /* No output still needs the ARP task after this ACK/window update. */
            finish_stream_neighbor(connection);
        }
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
    return request_ack(connection, progress, now_ms);
}
