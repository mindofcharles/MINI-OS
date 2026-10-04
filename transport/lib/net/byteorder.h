#ifndef MINI_OS_NET_BYTEORDER_H
#define MINI_OS_NET_BYTEORDER_H

#include "internal.h"

net_u16 net_read_be16(const void *buffer);
net_u32 net_read_be32(const void *buffer);
void net_write_be16(void *buffer, net_u16 value);
void net_write_be32(void *buffer, net_u32 value);

#endif
