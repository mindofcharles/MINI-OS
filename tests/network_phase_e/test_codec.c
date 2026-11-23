#include "../network_common/deterministic_backend.h"

#include "../../transport/lib/net/checksum.h"
#include "../../transport/lib/net/tcp/codec.h"
#include "../../transport/lib/net/tcp/internal.h"
#include "../../transport/lib/net/tcp/sequence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "E2 TCP codec failure at line %d: %s\n", \
            __LINE__, #condition); return 0; } } while (0)

static const struct net_ipv4_addr peer_ip = {{10U, 0U, 2U, 2U}};
static const struct net_ipv4_addr local_ip = {{10U, 0U, 2U, 15U}};

/* Fixed wire bytes calculated independently from the TCP and IPv4 layouts. */
static const net_u8 odd_wire[] = {
    0x12U, 0x34U, 0xC0U, 0x01U, 0xFFU, 0xFFU, 0xFFU, 0xFEU,
    0x01U, 0x02U, 0x03U, 0x04U, 0x50U, 0x19U, 0x10U, 0x00U,
    0xB1U, 0xD7U, 0x00U, 0x00U, 0x00U, 0xA5U, 0xFFU
};

static const net_u8 syn_wire[] = {
    0x00U, 0x16U, 0xC0U, 0x01U, 0x89U, 0xABU, 0xCDU, 0xEFU,
    0x00U, 0x00U, 0x00U, 0x00U, 0x60U, 0x02U, 0x10U, 0x00U,
    0x5BU, 0xFFU, 0x00U, 0x00U, 0x02U, 0x04U, 0x02U, 0x18U
};

static const net_u8 zero_checksum_wire[] = {
    0x00U, 0x16U, 0xC0U, 0x01U, 0x00U, 0x00U, 0x00U, 0x11U,
    0x00U, 0x00U, 0x00U, 0x13U, 0x50U, 0x10U, 0xD7U, 0x88U,
    0x00U, 0x00U, 0x00U, 0x00U
};

static const struct tcp_segment_view odd_segment = {
    .payload = odd_wire + 20U,
    .payload_length = 3U,
    .sequence = 0xFFFFFFFEU,
    .acknowledgment = 0x01020304U,
    .source_port = 0x1234U,
    .destination_port = 0xC001U,
    .window = 4096U,
    .flags = TCP_FLAG_ACK | TCP_FLAG_PSH | TCP_FLAG_FIN
};

static const struct tcp_segment_view syn_segment = {
    .sequence = 0x89ABCDEFU,
    .source_port = 22U,
    .destination_port = 0xC001U,
    .window = 4096U,
    .mss = 536U,
    .flags = TCP_FLAG_SYN
};

static const struct tcp_segment_view zero_checksum_segment = {
    .sequence = 17U,
    .acknowledgment = 19U,
    .source_port = 22U,
    .destination_port = 0xC001U,
    .window = 0xD788U,
    .flags = TCP_FLAG_ACK
};

static struct net_ipv4_view ipv4_payload(const net_u8 *wire, unsigned int length)
{
    return (struct net_ipv4_view){
        .source = peer_ip,
        .destination = local_ip,
        .payload = wire,
        .payload_length = length,
        .protocol = 6U
    };
}

/* Independent oracle for synthetic cases. Add bytes by parity, fold at the end. */
static unsigned int reference_sum(const net_u8 *wire, unsigned int length)
{
    unsigned int sum = 0U;

    for (unsigned int index = 0U; index < length; ++index) {
        unsigned int shift = index % 2U == 0U ? 8U : 0U;
        sum += (unsigned int)wire[index] << shift;
    }
    return sum;
}

static net_u16 reference_tcp_checksum(const net_u8 *wire, unsigned int length)
{
    unsigned int sum = reference_sum(peer_ip.octets, 4U) +
                       reference_sum(local_ip.octets, 4U) + 6U + length +
                       reference_sum(wire, length);

    while (sum > 0xFFFFU) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (net_u16)(sum ^ 0xFFFFU);
}

static void seal_segment(net_u8 *wire, unsigned int length)
{
    net_u16 checksum;

    wire[16] = 0U;
    wire[17] = 0U;
    checksum = reference_tcp_checksum(wire, length);
    wire[16] = (net_u8)(checksum >> 8);
    wire[17] = (net_u8)checksum;
}

static int decode_rejected(const struct net_ipv4_view *packet, int expected)
{
    struct tcp_segment_view decoded;
    unsigned char saved[sizeof(decoded)];

    memset(&decoded, 0xA5, sizeof(decoded));
    memcpy(saved, &decoded, sizeof(saved));
    return tcp_segment_decode(packet, &decoded) == expected &&
           memcmp(saved, &decoded, sizeof(saved)) == 0;
}

static int test_checksum_spans(void)
{
    static const net_u8 bytes[] = {
        0x00U, 0x01U, 0xF2U, 0x03U, 0xF4U, 0xF5U, 0xF6U, 0xF7U, 0xF8U
    };
    struct net_checksum checksum = {0};
    unsigned int length = sizeof(bytes);

    for (unsigned int first = 0U; first <= length; ++first) {
        for (unsigned int second = first; second <= length; ++second) {
            struct net_checksum split = {0};

            net_checksum_add(&split, bytes, first);
            net_checksum_add(&split, bytes + first, second - first);
            net_checksum_add(&split, 0, 0U);
            net_checksum_add(&split, bytes + second, length - second);
            CHECK(net_checksum_finish(&split) == 0x2A0CU);
        }
    }

    CHECK(net_checksum_finish(&checksum) == 0xFFFFU);
    for (unsigned int index = 0U; index < length - 1U; ++index) {
        net_checksum_add(&checksum, bytes + index, 1U);
    }
    CHECK(net_checksum_finish(&checksum) == 0x220DU);
    net_checksum_add(&checksum, bytes + length - 1U, 1U);
    net_checksum_add(&checksum, 0, 0U);
    CHECK(net_checksum_finish(&checksum) == 0x2A0CU);
    return 1;
}

static int test_known_decode(void)
{
    _Alignas(4) net_u8 unaligned[sizeof(odd_wire) + 1U];
    struct net_ipv4_view packet = ipv4_payload(unaligned + 1U, sizeof(odd_wire));
    struct tcp_segment_view decoded;
    net_u32 next;

    memcpy(unaligned + 1U, odd_wire, sizeof(odd_wire));
    CHECK(tcp_segment_decode(&packet, &decoded) == TCP_SEGMENT_VALID);
    CHECK(decoded.source_port == 0x1234U && decoded.destination_port == 0xC001U);
    CHECK(decoded.sequence == 0xFFFFFFFEU && decoded.acknowledgment == 0x01020304U);
    CHECK(decoded.window == 4096U && decoded.flags == 0x19U && decoded.mss == 0U);
    CHECK(decoded.payload == unaligned + 21U && decoded.payload_length == 3U);
    CHECK(memcmp(decoded.payload, odd_wire + 20U, 3U) == 0);
    CHECK(tcp_segment_sequence_length(&decoded) == 4U);
    next = decoded.sequence + tcp_segment_sequence_length(&decoded);
    CHECK(next == 2U && tcp_seq_compare(next, decoded.sequence) == TCP_SEQ_AFTER);

    packet = ipv4_payload(syn_wire, sizeof(syn_wire));
    CHECK(tcp_segment_decode(&packet, &decoded) == TCP_SEGMENT_VALID);
    CHECK(decoded.source_port == 22U && decoded.destination_port == 0xC001U);
    CHECK(decoded.sequence == 0x89ABCDEFU && decoded.acknowledgment == 0U);
    CHECK(decoded.window == 4096U && decoded.flags == TCP_FLAG_SYN);
    CHECK(decoded.mss == 536U && decoded.payload_length == 0U);
    CHECK(decoded.payload == syn_wire + sizeof(syn_wire));
    CHECK(tcp_segment_sequence_length(&decoded) == 1U);

    packet = ipv4_payload(zero_checksum_wire, sizeof(zero_checksum_wire));
    CHECK(tcp_segment_decode(&packet, &decoded) == TCP_SEGMENT_VALID);
    CHECK(decoded.flags == TCP_FLAG_ACK && decoded.mss == 0U);
    CHECK(tcp_segment_sequence_length(&decoded) == 0U);
    return 1;
}

static int test_header_validation(void)
{
    net_u8 options_wire[TCP_HEADER_MAX_SIZE];
    net_u8 wire[sizeof(odd_wire)];
    struct net_ipv4_view packet;
    struct tcp_segment_view decoded;

    memcpy(options_wire, syn_wire, 20U);
    options_wire[12] = 0xF0U;
    memset(options_wire + 20U, 1, sizeof(options_wire) - 20U);
    seal_segment(options_wire, sizeof(options_wire));
    packet = ipv4_payload(options_wire, sizeof(options_wire));
    CHECK(tcp_segment_decode(&packet, &decoded) == TCP_SEGMENT_VALID);
    CHECK(decoded.payload == options_wire + sizeof(options_wire));
    CHECK(decoded.payload_length == 0U && decoded.mss == 0U);

    /* ASan sees the real end of each successively truncated header. */
    for (unsigned int length = 0U; length < sizeof(options_wire); ++length) {
        net_u8 *storage = malloc(length + 1U);
        int rejected;

        CHECK(storage != 0);
        memcpy(storage + 1U, options_wire, length);
        packet = ipv4_payload(storage + 1U, length);
        rejected = decode_rejected(&packet, TCP_SEGMENT_MALFORMED);
        free(storage);
        CHECK(rejected);
    }

    packet = ipv4_payload(odd_wire, sizeof(odd_wire));
    CHECK(decode_rejected(0, NET_ERR_INVALID));
    CHECK(tcp_segment_decode(&packet, 0) == NET_ERR_INVALID);
    packet.protocol = 1U;
    CHECK(decode_rejected(&packet, NET_ERR_INVALID));
    packet = ipv4_payload(0, sizeof(odd_wire));
    CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));
    packet = ipv4_payload(odd_wire, NET_IPV4_PAYLOAD_MAX + 1U);
    CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));

    packet = ipv4_payload(odd_wire, sizeof(odd_wire));
    packet.source.octets[3] ^= 1U;
    CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));
    packet.source = peer_ip;
    packet.destination.octets[3] ^= 1U;
    CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));

    memcpy(wire, odd_wire, sizeof(wire));
    packet = ipv4_payload(wire, sizeof(wire));
    wire[16] ^= 1U;
    CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));
    wire[16] ^= 1U;
    wire[sizeof(wire) - 1U] ^= 1U;
    CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));
    wire[sizeof(wire) - 1U] ^= 1U;
    wire[12] = 0x40U;
    seal_segment(wire, sizeof(wire));
    CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));

    for (unsigned int offset = 0U; offset <= 2U; offset += 2U) {
        memcpy(wire, odd_wire, sizeof(wire));
        wire[offset] = 0U;
        wire[offset + 1U] = 0U;
        seal_segment(wire, sizeof(wire));
        CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));
    }
    return 1;
}

static int test_option_validation(void)
{
    static const net_u8 options[] = {
        1U, 2U, 4U, 0x05U, 0xB4U, 2U, 4U, 0x02U, 0x18U,
        2U, 4U, 0x04U, 0x00U, 2U, 4U, 0x00U, 0x00U,
        0xACU, 4U, 0xA5U, 0xFFU, 0U, 0U, 0U
    };
    static const net_u8 malformed[][4] = {
        {0xACU, 0U, 1U, 1U},
        {0xACU, 1U, 1U, 1U},
        {0xACU, 5U, 1U, 1U},
        {1U, 1U, 1U, 0xACU},
        {1U, 2U, 4U, 0x02U},
        {2U, 3U, 0x02U, 1U},
        {0U, 1U, 0U, 0U}
    };
    net_u8 mixed[20U + sizeof(options)];
    net_u8 invalid[27];
    net_u8 header_only[sizeof(syn_wire)];
    struct net_ipv4_view packet;
    struct tcp_segment_view decoded;

    memcpy(mixed, syn_wire, 20U);
    memcpy(mixed + 20U, options, sizeof(options));
    mixed[12] = 0xB7U; /* 44-byte header with nonzero reserved bits. */
    mixed[13] = TCP_FLAG_SYN | TCP_FLAG_ECE | TCP_FLAG_CWR;
    seal_segment(mixed, sizeof(mixed));
    packet = ipv4_payload(mixed, sizeof(mixed));
    CHECK(tcp_segment_decode(&packet, &decoded) == TCP_SEGMENT_VALID);
    CHECK(decoded.mss == 536U && decoded.flags == 0xC2U);
    CHECK(decoded.payload_length == 0U && decoded.payload == mixed + sizeof(mixed));

    mixed[13] = TCP_FLAG_ACK;
    seal_segment(mixed, sizeof(mixed));
    CHECK(tcp_segment_decode(&packet, &decoded) == TCP_SEGMENT_VALID);
    CHECK(decoded.mss == 0U);

    memcpy(invalid, syn_wire, sizeof(syn_wire));
    memcpy(invalid + 24U, odd_wire + 20U, 3U);
    for (unsigned int index = 0U; index < sizeof(malformed) / sizeof(malformed[0]);
         ++index) {
        memcpy(invalid + 20U, malformed[index], sizeof(malformed[index]));
        seal_segment(invalid, sizeof(invalid));
        packet = ipv4_payload(invalid, sizeof(invalid));
        CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));

        /* Give ASan a buffer ending exactly at the TCP option boundary. */
        memcpy(header_only, invalid, sizeof(header_only));
        seal_segment(header_only, sizeof(header_only));
        packet = ipv4_payload(header_only, sizeof(header_only));
        CHECK(decode_rejected(&packet, TCP_SEGMENT_MALFORMED));
    }
    return 1;
}

static int test_known_encode(void)
{
    static const struct {
        const struct tcp_segment_view *segment;
        const net_u8 *wire;
        unsigned int length;
    } vectors[] = {
        {&odd_segment, odd_wire, sizeof(odd_wire)},
        {&syn_segment, syn_wire, sizeof(syn_wire)},
        {&zero_checksum_segment, zero_checksum_wire, sizeof(zero_checksum_wire)}
    };
    _Alignas(4) net_u8 storage[sizeof(syn_wire) + 2U];
    net_u8 *wire = storage + 1U;

    for (unsigned int index = 0U; index < sizeof(vectors) / sizeof(vectors[0]);
         ++index) {
        unsigned int length = vectors[index].length;

        memset(storage, 0xA5, sizeof(storage));
        CHECK(tcp_segment_encode(wire, length, &peer_ip, &local_ip,
                                 vectors[index].segment) == (int)length);
        CHECK(memcmp(wire, vectors[index].wire, length) == 0);
        CHECK(storage[0] == 0xA5U && wire[length] == 0xA5U);
    }
    return 1;
}

static int test_encode_bounds(void)
{
    net_u8 storage[NET_IPV4_PAYLOAD_MAX + 2U];
    net_u8 saved[sizeof(storage)];
    net_u8 *wire = storage + 1U;
    struct tcp_segment_view segment = odd_segment;
    struct net_ipv4_view packet;
    struct tcp_segment_view decoded;

    memset(storage, 0xA5, sizeof(storage));
    CHECK(tcp_segment_encode(wire, sizeof(odd_wire) - 1U, &peer_ip, &local_ip,
                             &segment) == NET_ERR_INVALID);
    segment.payload_length = UINT_MAX;
    CHECK(tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX, &peer_ip, &local_ip,
                             &segment) == NET_ERR_INVALID);
    segment = odd_segment;
    segment.payload = 0;
    CHECK(tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX, &peer_ip, &local_ip,
                             &segment) == NET_ERR_INVALID);
    segment = odd_segment;
    segment.flags |= TCP_FLAG_URG;
    CHECK(tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX, &peer_ip, &local_ip,
                             &segment) == NET_ERR_INVALID);
    segment = odd_segment;
    segment.source_port = 0U;
    CHECK(tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX, &peer_ip, &local_ip,
                             &segment) == NET_ERR_INVALID);
    segment = odd_segment;
    segment.mss = 536U;
    CHECK(tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX, &peer_ip, &local_ip,
                             &segment) == NET_ERR_INVALID);
    segment = syn_segment;
    segment.mss = TCP_IPV4_DATA_MAX + 1U;
    CHECK(tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX, &peer_ip, &local_ip,
                             &segment) == NET_ERR_INVALID);
    for (unsigned int index = 0U; index < sizeof(storage); ++index) {
        CHECK(storage[index] == 0xA5U);
    }

    segment = odd_segment;
    segment.payload = wire + 20U;
    segment.payload_length = TCP_IPV4_DATA_MAX;
    CHECK(tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX, &peer_ip, &local_ip,
                             &segment) == NET_IPV4_PAYLOAD_MAX);
    CHECK(reference_tcp_checksum(wire, NET_IPV4_PAYLOAD_MAX) == 0U);
    CHECK(storage[0] == 0xA5U && wire[NET_IPV4_PAYLOAD_MAX] == 0xA5U);
    packet = ipv4_payload(wire, NET_IPV4_PAYLOAD_MAX);
    CHECK(tcp_segment_decode(&packet, &decoded) == TCP_SEGMENT_VALID);
    CHECK(decoded.payload_length == 1460U && decoded.payload == wire + 20U);
    for (unsigned int index = 0U; index < decoded.payload_length; ++index) {
        CHECK(decoded.payload[index] == 0xA5U);
    }

    segment = syn_segment;
    segment.payload = wire + 24U;
    segment.payload_length = NET_IPV4_PAYLOAD_MAX - 24U;
    CHECK(tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX, &peer_ip, &local_ip,
                             &segment) == NET_IPV4_PAYLOAD_MAX);
    CHECK(reference_tcp_checksum(wire, NET_IPV4_PAYLOAD_MAX) == 0U);
    CHECK(storage[0] == 0xA5U && wire[NET_IPV4_PAYLOAD_MAX] == 0xA5U);
    CHECK(tcp_segment_decode(&packet, &decoded) == TCP_SEGMENT_VALID);
    CHECK(decoded.payload_length == 1456U && decoded.payload == wire + 24U);
    CHECK(decoded.mss == 536U);

    memcpy(saved, storage, sizeof(saved));
    ++segment.payload_length;
    CHECK(tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX + 1U, &peer_ip, &local_ip,
                             &segment) == NET_ERR_INVALID);
    CHECK(memcmp(storage, saved, sizeof(storage)) == 0);
    return 1;
}

static int test_sequence_numbers(void)
{
    CHECK(tcp_seq_compare(17U, 17U) == TCP_SEQ_EQUAL);
    CHECK(tcp_seq_compare(0U, UINT_MAX) == TCP_SEQ_AFTER);
    CHECK(tcp_seq_compare(UINT_MAX, 0U) == TCP_SEQ_BEFORE);
    CHECK(tcp_seq_compare(0x7FFFFFFFU, 0U) == TCP_SEQ_AFTER);
    CHECK(tcp_seq_compare(0x80000000U, 0U) == TCP_SEQ_UNORDERED);
    CHECK(tcp_seq_compare(0U, 0x80000000U) == TCP_SEQ_UNORDERED);
    CHECK(tcp_seq_compare(0x80000001U, 0U) == TCP_SEQ_BEFORE);
    CHECK(tcp_seq_compare(5U, 0x80000005U) == TCP_SEQ_UNORDERED);

    CHECK(tcp_seq_in_range(0xFFFFFFFEU, 0xFFFFFFFEU, 4U));
    CHECK(tcp_seq_in_range(0xFFFFFFFFU, 0xFFFFFFFEU, 4U));
    CHECK(tcp_seq_in_range(0U, 0xFFFFFFFEU, 4U));
    CHECK(tcp_seq_in_range(1U, 0xFFFFFFFEU, 4U));
    CHECK(!tcp_seq_in_range(2U, 0xFFFFFFFEU, 4U));
    CHECK(!tcp_seq_in_range(0xFFFFFFFDU, 0xFFFFFFFEU, 4U));
    CHECK(!tcp_seq_in_range(19U, 19U, 0U));
    CHECK(tcp_seq_in_range(0x7FFFFFFEU, 0U, 0x7FFFFFFFU));
    CHECK(!tcp_seq_in_range(0x7FFFFFFFU, 0U, 0x7FFFFFFFU));
    CHECK(!tcp_seq_in_range(0U, 0U, 0x80000000U));
    CHECK(!tcp_seq_in_range(0U, 0U, UINT_MAX));
    return 1;
}

static int test_ipv4_codec_path(void)
{
    static const net_u8 ipv4_header[] = {
        0x45U, 0x00U, 0x00U, 0x2BU, 0x00U, 0x00U, 0x40U, 0x00U,
        0x40U, 0x06U, 0x22U, 0xBDU, 0x0AU, 0x00U, 0x02U, 0x02U,
        0x0AU, 0x00U, 0x02U, 0x0FU
    };
    static const struct net_config config = {
        {{10U, 0U, 2U, 15U}}, {{255U, 255U, 255U, 0U}}, {{10U, 0U, 2U, 2U}},
        1000U, 1000U, 2U
    };
    static const struct tcp_endpoint endpoint = {{{10U, 0U, 2U, 2U}}, 0x1234U};
    static const struct net_mac_addr peer_mac = {{2U, 0U, 0U, 0U, 0U, 1U}};
    struct net_context *context = &net_global_context;
    struct tcp_config tcp_config;
    struct tcp_status status;
    struct tcp_segment_view segment = odd_segment;
    tcp_handle handle = TCP_INVALID_HANDLE;
    net_u8 frame[NET_FRAME_MIN];
    net_u8 *ip = frame + 14U;
    net_u8 *tcp = ip + 20U;
    net_u8 *tx_ip;
    net_u8 *tx_tcp;
    int length;

    network_test_backend_reset();
    memset(context, 0, sizeof(*context));
    memset(&tcp_global_context, 0, sizeof(tcp_global_context));
    CHECK(net_init(&config) == 0);
    CHECK(tcp_config_defaults(&tcp_config) == 0);
    CHECK(tcp_connection_allocate(&endpoint, &tcp_config, 0U, &handle) == 0);

    memset(frame, 0xA5, sizeof(frame));
    memcpy(frame, context->device_mac.octets, 6U);
    memcpy(frame + 6U, peer_mac.octets, 6U);
    frame[12] = 0x08U;
    frame[13] = 0U;
    memcpy(ip, ipv4_header, sizeof(ipv4_header));
    memcpy(tcp, odd_wire, sizeof(odd_wire));
    CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
    tcp[16] ^= 1U;
    CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
    tcp[16] ^= 1U;

    /* Receive only the declared IP bytes, despite nonzero Ethernet padding. */
    CHECK(net_poll(0U) == 0);
    CHECK(context->counters.unsupported_frames == 1U);
    CHECK(context->counters.malformed_frames == 0U);
    CHECK(net_poll(0U) == 0);
    CHECK(context->counters.unsupported_frames == 1U);
    CHECK(context->counters.malformed_frames == 1U);
    CHECK(network_test_backend_transmit_count() == 0U);
    CHECK(tcp_status(handle, &status) == 0 && status.state == TCP_STATE_CONNECTING);

    /* Exercise the existing transmit buffer from this private test fixture. */
    tx_ip = net_ethernet_begin_tx(context);
    CHECK(tx_ip != 0);
    tx_tcp = tx_ip + NET_IPV4_HEADER_SIZE;
    memcpy(tx_tcp + 20U, odd_wire + 20U, 3U);
    segment.payload = tx_tcp + 20U;
    segment.source_port = 0xC001U;
    segment.destination_port = 0x1234U;
    length = tcp_segment_encode(tx_tcp, NET_IPV4_PAYLOAD_MAX,
                                &local_ip, &peer_ip, &segment);
    CHECK(length == (int)sizeof(odd_wire));
    CHECK(net_ipv4_send(context, &peer_ip, &peer_mac, NET_IPV4_PROTOCOL_TCP,
                        (unsigned int)length) == 0);

    /* Reverse the address and port pairs in the independent receive vector. */
    memcpy(frame, peer_mac.octets, 6U);
    memcpy(frame + 6U, context->device_mac.octets, 6U);
    memcpy(ip + 12U, ipv4_header + 16U, 4U);
    memcpy(ip + 16U, ipv4_header + 12U, 4U);
    memcpy(tcp, odd_wire + 2U, 2U);
    memcpy(tcp + 2U, odd_wire, 2U);
    memset(tcp + sizeof(odd_wire), 0, sizeof(frame) - 14U - 20U - sizeof(odd_wire));
    CHECK(network_test_backend_transmit_count() == 1U);
    CHECK(network_test_backend_transmit_length(0U) == sizeof(frame));
    CHECK(memcmp(network_test_backend_transmit_frame(0U), frame, sizeof(frame)) == 0);
    CHECK(tcp_abort(handle) == 0 && tcp_release(handle) == 0 && tcp_deinit() == 0);
    return 1;
}

int test_tcp_codec(void)
{
    if (!test_checksum_spans() || !test_known_decode() ||
        !test_header_validation() || !test_option_validation() ||
        !test_known_encode() || !test_encode_bounds() ||
        !test_sequence_numbers() || !test_ipv4_codec_path()) {
        return 0;
    }
    puts("E2 TCP codec, wire vectors and sequence tests: PASS");
    return 1;
}
