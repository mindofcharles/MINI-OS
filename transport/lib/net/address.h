#ifndef MINI_OS_NET_ADDRESS_H
#define MINI_OS_NET_ADDRESS_H

#include "net.h"

/* These validators return zero or NET_ERR_INVALID. */
int net_config_validate(const struct net_config *config);
int net_mac_validate_unicast(const unsigned char *octets);

/* Private static-route helpers; only peers on the configured link use ARP. */
int net_ipv4_is_on_link_peer(const struct net_config *config,
                             const struct net_ipv4_addr *address);
int net_ipv4_select_next_hop(const struct net_config *config,
                              const struct net_ipv4_addr *destination,
                              struct net_ipv4_addr *next_hop);

#endif
