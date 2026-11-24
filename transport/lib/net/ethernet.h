#ifndef MINI_OS_NET_ETHERNET_H
#define MINI_OS_NET_ETHERNET_H

#include "internal.h"

enum {
    NET_ETHERTYPE_IPV4 = 0x0800,
    NET_ETHERTYPE_ARP = 0x0806,
    NET_ETHERNET_PAYLOAD_MAX = NET_FRAME_MAX - NET_ETHERNET_HEADER_SIZE
};

enum net_ethernet_class {
    NET_ETHERNET_DROP = 0,
    NET_ETHERNET_ARP = 1,
    NET_ETHERNET_IPV4 = 2
};

/* Neither result starts raw transmit. Only caller deferral is resumable. */
enum {
    NET_ETHERNET_TX_DEFERRED = 256,
    NET_ETHERNET_TX_EXPIRED = 257
};

struct net_tx_deadline {
    net_u32 started_ms;
    unsigned int timeout_ms;
};

struct net_ethernet_view {
    struct net_mac_addr destination;
    struct net_mac_addr source;
    const unsigned char *payload;
    unsigned int payload_length;
    net_u16 ethertype;
};

/* A zero-length no-frame report is invalid and does not alter counters. */
/* Call once for each positive raw receive; a rejected frame leaves view unchanged. */
int net_ethernet_decode(struct net_context *context, unsigned int frame_length,
                        struct net_ethernet_view *view);

/* Begin clears the frame and returns space after its header. */
unsigned char *net_ethernet_begin_tx(struct net_context *context);

/* Send consumes one prepared frame even if deferred or the driver fails. */
int net_ethernet_send(struct net_context *context,
                      const struct net_mac_addr *destination,
                      net_u16 ethertype, unsigned int payload_length);
/* A non-null protocol deadline is checked alongside the caller's wait. */
int net_ethernet_send_before(struct net_context *context,
                             const struct net_mac_addr *destination,
                             net_u16 ethertype, unsigned int payload_length,
                             const struct net_tx_deadline *deadline);

#endif
