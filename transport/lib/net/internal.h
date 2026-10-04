#ifndef MINI_OS_NET_INTERNAL_H
#define MINI_OS_NET_INTERNAL_H

#include <limits.h>

#include "net.h"
#include "raw.h"

typedef unsigned char net_u8;
typedef unsigned short net_u16;
typedef unsigned int net_u32;

#define NET_STATIC_ASSERT(name, expression) \
    _Static_assert((expression), #name)

NET_STATIC_ASSERT(byte_is_8_bits, CHAR_BIT == 8);
NET_STATIC_ASSERT(net_u8_is_8_bits, UCHAR_MAX == 0xFFU);
NET_STATIC_ASSERT(net_u16_is_16_bits, USHRT_MAX == 0xFFFFU);
NET_STATIC_ASSERT(net_u32_is_32_bits, UINT_MAX == 0xFFFFFFFFU);
NET_STATIC_ASSERT(raw_frame_size_is_expected, NET_FRAME_MAX == 1514);

#undef NET_STATIC_ASSERT

enum {
    NET_ETHERNET_HEADER_SIZE = 14,
    NET_ARP_PACKET_SIZE = 28,
    NET_IPV4_HEADER_SIZE = 20,
    NET_ICMP_HEADER_SIZE = 8,
    NET_ARP_CACHE_CAPACITY = 4
};

struct net_arp_cache_entry {
    struct net_ipv4_addr address;
    struct net_mac_addr mac;
    net_u32 updated_ms;
    int valid;
};

struct net_pending_arp {
    struct net_ipv4_addr address;
    struct net_mac_addr resolved_mac;
    net_u32 last_request_ms;
    unsigned int attempts;
    int active;
    int resolved;
};

struct net_pending_echo {
    struct net_ipv4_addr destination;
    struct net_ipv4_addr reply_source;
    const net_u8 *payload;
    net_u32 started_ms;
    net_u32 sent_ms;
    net_u32 timeout_ms;
    net_u32 elapsed_ms;
    unsigned int payload_length;
    net_u16 identifier;
    net_u16 sequence;
    int active;
    int matched;
};

struct net_protocol_counters {
    net_u32 received_frames;
    net_u32 malformed_frames;
    net_u32 unsupported_frames;
    net_u32 arp_cache_updates;
    net_u32 echo_requests;
    net_u32 echo_replies;
};

struct net_context {
    unsigned char tx_frame[NET_FRAME_MAX];
    unsigned char rx_frame[NET_FRAME_MAX];
    struct net_arp_cache_entry arp_cache[NET_ARP_CACHE_CAPACITY];
    struct net_config config;
    struct net_mac_addr device_mac;
    struct net_pending_arp pending_arp;
    struct net_pending_echo pending_echo;
    struct net_protocol_counters counters;
    net_u32 arp_last_request_ms;
    net_u16 next_echo_identifier;
    int arp_request_seen;
    int initialized;
    int tx_prepared;
    int service_active;
};

extern struct net_context net_global_context;

#endif
