#ifndef MINI_OS_NET_ARP_H
#define MINI_OS_NET_ARP_H

#include "ethernet.h"

enum net_arp_class {
    NET_ARP_MALFORMED = 0,
    NET_ARP_REQUEST = 1,
    NET_ARP_REPLY = 2,
    NET_ARP_UNSUPPORTED = 3
};

struct net_arp_packet {
    struct net_mac_addr sender_mac;
    struct net_ipv4_addr sender_ip;
    struct net_mac_addr target_mac;
    struct net_ipv4_addr target_ip;
};

/* Parsing does not mutate the context or the output on rejection. */
int net_arp_parse(const struct net_ethernet_view *view,
                  struct net_arp_packet *packet);
int net_arp_handle(struct net_context *context,
                   const struct net_ethernet_view *view);
int net_arp_cache_lookup(struct net_context *context,
                         const struct net_ipv4_addr *address,
                         struct net_mac_addr *mac);
int net_arp_cache_store(struct net_context *context,
                        const struct net_ipv4_addr *address,
                        const struct net_mac_addr *mac);
int net_arp_timer(struct net_context *context, net_u32 now);

#endif
