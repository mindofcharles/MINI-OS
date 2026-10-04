#ifndef MINI_OS_NET_ICMP_H
#define MINI_OS_NET_ICMP_H

#include "ipv4.h"

enum net_icmp_class {
    NET_ICMP_MALFORMED = 0,
    NET_ICMP_ECHO_REPLY = 1,
    NET_ICMP_ECHO_REQUEST = 2,
    NET_ICMP_UNSUPPORTED = 3
};

struct net_icmp_echo_view {
    const unsigned char *payload;
    unsigned int payload_length;
    net_u16 identifier;
    net_u16 sequence;
};

/* Rejected messages leave output unchanged. */
int net_icmp_parse(const struct net_ipv4_view *packet,
                   struct net_icmp_echo_view *echo);
int net_icmp_handle(struct net_context *context,
                    const struct net_ethernet_view *ethernet,
                    const struct net_ipv4_view *packet);
int net_icmp_send_request(struct net_context *context,
                          const struct net_ipv4_addr *destination,
                          const struct net_mac_addr *next_hop_mac,
                          net_u16 identifier, net_u16 sequence,
                          const void *payload, unsigned int payload_length);

#endif
