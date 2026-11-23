#ifndef MINI_OS_TCP_CODEC_H
#define MINI_OS_TCP_CODEC_H

#include "../ipv4.h"

enum {
    TCP_HEADER_MIN_SIZE = 20,
    TCP_HEADER_MAX_SIZE = 60,
    TCP_IPV4_DATA_MAX = NET_IPV4_PAYLOAD_MAX - TCP_HEADER_MIN_SIZE
};

enum tcp_flag {
    TCP_FLAG_FIN = 0x01,
    TCP_FLAG_SYN = 0x02,
    TCP_FLAG_RST = 0x04,
    TCP_FLAG_PSH = 0x08,
    TCP_FLAG_ACK = 0x10,
    TCP_FLAG_URG = 0x20,
    TCP_FLAG_ECE = 0x40,
    TCP_FLAG_CWR = 0x80
};

enum tcp_segment_class {
    TCP_SEGMENT_MALFORMED = 0,
    TCP_SEGMENT_VALID = 1
};

struct tcp_segment_view {
    const net_u8 *payload;
    unsigned int payload_length;
    net_u32 sequence;
    net_u32 acknowledgment;
    net_u16 source_port;
    net_u16 destination_port;
    net_u16 window;
    net_u16 mss; /* Zero means there is no usable MSS option. */
    net_u8 flags;
};

/*
 * Decode the exact TCP payload supplied by IPv4. Rejection leaves segment
 * unchanged. The payload view is valid while the underlying frame is valid.
 */
int tcp_segment_decode(const struct net_ipv4_view *packet,
                       struct tcp_segment_view *segment);

/*
 * Write into the caller's IPv4 payload space and return the segment length.
 * Failure leaves wire unchanged. Addresses and metadata live outside wire.
 * The payload can already be in its final position or in a nonoverlapping buffer.
 * Outgoing options consist of a nonzero MSS on SYN when requested.
 */
int tcp_segment_encode(net_u8 *wire, unsigned int capacity,
                       const struct net_ipv4_addr *source,
                       const struct net_ipv4_addr *destination,
                       const struct tcp_segment_view *segment);

/* A validated segment occupies one extra sequence position for each SYN/FIN. */
unsigned int tcp_segment_sequence_length(const struct tcp_segment_view *segment);

#endif
