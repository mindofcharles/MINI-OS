#ifndef MINI_OS_NET_ADDRESS_H
#define MINI_OS_NET_ADDRESS_H

#include "net.h"

/* These validators return zero or NET_ERR_INVALID. */
int net_config_validate(const struct net_config *config);
int net_mac_validate_unicast(const unsigned char *octets);

#endif
