#ifndef MINI_OS_NET_CHECKSUM_H
#define MINI_OS_NET_CHECKSUM_H

#include "internal.h"

/* Buffer may be null only when length is zero. */
net_u16 net_checksum_compute(const void *buffer, unsigned int length);
int net_checksum_valid(const void *buffer, unsigned int length);

#endif
