#ifndef MINI_OS_NET_IPV4_H
#define MINI_OS_NET_IPV4_H

#include "ethernet.h"

enum {
    NET_IPV4_PROTOCOL_ICMP = 1,
    NET_IPV4_PAYLOAD_MAX = NET_IPV4_MTU - NET_IPV4_HEADER_SIZE
};

enum net_ipv4_class {
    NET_IPV4_MALFORMED = 0,
    NET_IPV4_ACCEPT = 1,
    NET_IPV4_UNSUPPORTED = 2
};

struct net_ipv4_view {
    struct net_ipv4_addr source;
    struct net_ipv4_addr destination;
    const unsigned char *payload;
    unsigned int payload_length;
    net_u8 protocol;
    net_u8 ttl;
};

/* Rejected packets leave output unchanged. */
int net_ipv4_decode(const struct net_context *context,
                    const struct net_ethernet_view *ethernet,
                    struct net_ipv4_view *packet);
int net_ipv4_handle(struct net_context *context,
                    const struct net_ethernet_view *ethernet);

/* The caller builds the payload in the prepared transmit frame at offset 34. */
int net_ipv4_send(struct net_context *context,
                  const struct net_ipv4_addr *destination,
                  const struct net_mac_addr *next_hop_mac,
                  net_u8 protocol, unsigned int payload_length);

#endif
