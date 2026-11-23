#ifndef MINI_OS_TCP_SEQUENCE_H
#define MINI_OS_TCP_SEQUENCE_H

#include "../internal.h"

enum tcp_seq_order {
    TCP_SEQ_BEFORE,
    TCP_SEQ_EQUAL,
    TCP_SEQ_AFTER,
    TCP_SEQ_UNORDERED
};

/* Two positions exactly half a sequence space apart have no defined order. */
enum tcp_seq_order tcp_seq_compare(net_u32 left, net_u32 right);

/* Half-open modular interval. A zero or half-space-or-larger length is empty. */
int tcp_seq_in_range(net_u32 sequence, net_u32 start, net_u32 length);

#endif
