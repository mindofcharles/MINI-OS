#include "codec.h"

#include "../byteorder.h"
#include "../checksum.h"

#include <string.h>

enum {
    TCP_OPTION_END = 0,
    TCP_OPTION_NOP = 1,
    TCP_OPTION_MSS = 2,
    TCP_OPTION_MSS_SIZE = 4,
    TCP_SEND_FLAGS = TCP_FLAG_FIN | TCP_FLAG_SYN | TCP_FLAG_RST |
                     TCP_FLAG_PSH | TCP_FLAG_ACK
};

static net_u16 segment_checksum(const struct net_ipv4_addr *source,
                                const struct net_ipv4_addr *destination,
                                const net_u8 *wire, unsigned int length)
{
    struct net_checksum checksum = {0};
    net_u8 pseudo_header_tail[4] = {0U, NET_IPV4_PROTOCOL_TCP, 0U, 0U};

    net_write_be16(pseudo_header_tail + 2U, (net_u16)length);
    net_checksum_add(&checksum, source->octets, sizeof(source->octets));
    net_checksum_add(&checksum, destination->octets, sizeof(destination->octets));
    net_checksum_add(&checksum, pseudo_header_tail, sizeof(pseudo_header_tail));
    net_checksum_add(&checksum, wire, length);
    return net_checksum_finish(&checksum);
}

static int parse_options(const net_u8 *option, unsigned int remaining,
                         struct tcp_segment_view *segment)
{
    while (remaining != 0U) {
        unsigned int kind = option[0];
        unsigned int length;

        if (kind == TCP_OPTION_END) {
            for (unsigned int index = 1U; index < remaining; ++index) {
                if (option[index] != 0U) {
                    return 0;
                }
            }
            return 1;
        }
        if (kind == TCP_OPTION_NOP) {
            ++option;
            --remaining;
            continue;
        }
        if (remaining < 2U) {
            return 0;
        }
        length = option[1];
        if (length < 2U || length > remaining) {
            return 0;
        }
        if (kind == TCP_OPTION_MSS) {
            net_u16 mss;

            if (length != TCP_OPTION_MSS_SIZE) {
                return 0;
            }
            mss = net_read_be16(option + 2U);
            /* Duplicate advertisements can only lower the peer's limit. */
            if ((segment->flags & TCP_FLAG_SYN) != 0U && mss != 0U &&
                (segment->mss == 0U || mss < segment->mss)) {
                segment->mss = mss;
            }
        }
        option += length;
        remaining -= length;
    }
    return 1;
}

int tcp_segment_decode(const struct net_ipv4_view *packet,
                       struct tcp_segment_view *segment)
{
    struct tcp_segment_view parsed;
    const net_u8 *wire;
    unsigned int header_length;

    if (packet == 0 || segment == 0 || packet->protocol != NET_IPV4_PROTOCOL_TCP) {
        return NET_ERR_INVALID;
    }
    if (packet->payload == 0 || packet->payload_length < TCP_HEADER_MIN_SIZE ||
        packet->payload_length > NET_IPV4_PAYLOAD_MAX) {
        return TCP_SEGMENT_MALFORMED;
    }
    wire = packet->payload;
    header_length = (unsigned int)(wire[12] >> 4) * 4U;
    if (header_length < TCP_HEADER_MIN_SIZE ||
        header_length > packet->payload_length) {
        return TCP_SEGMENT_MALFORMED;
    }
    parsed.source_port = net_read_be16(wire);
    parsed.destination_port = net_read_be16(wire + 2U);
    if (parsed.source_port == 0U || parsed.destination_port == 0U ||
        segment_checksum(&packet->source, &packet->destination, wire,
                         packet->payload_length) != 0U) {
        return TCP_SEGMENT_MALFORMED;
    }
    parsed.sequence = net_read_be32(wire + 4U);
    parsed.acknowledgment = net_read_be32(wire + 8U);
    parsed.flags = wire[13];
    parsed.window = net_read_be16(wire + 14U);
    parsed.mss = 0U;
    if (!parse_options(wire + TCP_HEADER_MIN_SIZE,
                       header_length - TCP_HEADER_MIN_SIZE, &parsed)) {
        return TCP_SEGMENT_MALFORMED;
    }
    parsed.payload = wire + header_length;
    parsed.payload_length = packet->payload_length - header_length;
    *segment = parsed;
    return TCP_SEGMENT_VALID;
}

int tcp_segment_encode(net_u8 *wire, unsigned int capacity,
                       const struct net_ipv4_addr *source,
                       const struct net_ipv4_addr *destination,
                       const struct tcp_segment_view *segment)
{
    unsigned int header_length = TCP_HEADER_MIN_SIZE;
    unsigned int length;

    if (wire == 0 || source == 0 || destination == 0 || segment == 0 ||
        segment->source_port == 0U || segment->destination_port == 0U ||
        (segment->flags & TCP_SEND_FLAGS) != segment->flags ||
        (segment->payload_length != 0U && segment->payload == 0)) {
        return NET_ERR_INVALID;
    }
    if (segment->mss != 0U) {
        if ((segment->flags & TCP_FLAG_SYN) == 0U ||
            segment->mss > TCP_IPV4_DATA_MAX) {
            return NET_ERR_INVALID;
        }
        header_length += TCP_OPTION_MSS_SIZE;
    }
    if (segment->payload_length > NET_IPV4_PAYLOAD_MAX - header_length) {
        return NET_ERR_INVALID;
    }
    length = header_length + segment->payload_length;
    if (length > capacity) {
        return NET_ERR_INVALID;
    }

    net_write_be16(wire, segment->source_port);
    net_write_be16(wire + 2U, segment->destination_port);
    net_write_be32(wire + 4U, segment->sequence);
    net_write_be32(wire + 8U, segment->acknowledgment);
    wire[12] = (net_u8)((header_length / 4U) << 4);
    wire[13] = segment->flags;
    net_write_be16(wire + 14U, segment->window);
    net_write_be16(wire + 16U, 0U);
    net_write_be16(wire + 18U, 0U);
    if (segment->mss != 0U) {
        net_u8 *option = wire + TCP_HEADER_MIN_SIZE;

        option[0] = TCP_OPTION_MSS;
        option[1] = TCP_OPTION_MSS_SIZE;
        net_write_be16(option + 2U, segment->mss);
    }
    if (segment->payload_length != 0U && segment->payload != wire + header_length) {
        memcpy(wire + header_length, segment->payload, segment->payload_length);
    }
    net_write_be16(wire + 16U,
                   segment_checksum(source, destination, wire, length));
    return (int)length;
}

unsigned int tcp_segment_sequence_length(const struct tcp_segment_view *segment)
{
    return segment->payload_length + ((segment->flags & TCP_FLAG_SYN) != 0U) +
           ((segment->flags & TCP_FLAG_FIN) != 0U);
}
