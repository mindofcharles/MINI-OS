#ifndef MINI_OS_NET_CHECKSUM_H
#define MINI_OS_NET_CHECKSUM_H

#include "internal.h"

/* Zero-initialize an accumulator before adding spans in wire order. */
struct net_checksum {
    net_u32 sum;
    int odd;
};

/*
 * An unmatched high byte is paired with the first byte of the next span.
 * Buffer may be null for an empty span.
 */
void net_checksum_add(struct net_checksum *checksum, const void *buffer,
                      unsigned int length);
net_u16 net_checksum_finish(const struct net_checksum *checksum);

/* Buffer may be null only when length is zero. */
net_u16 net_checksum_compute(const void *buffer, unsigned int length);
int net_checksum_valid(const void *buffer, unsigned int length);

#endif
