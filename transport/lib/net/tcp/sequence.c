#include "sequence.h"

#define TCP_SEQ_HALF_SPACE 0x80000000U

enum tcp_seq_order tcp_seq_compare(net_u32 left, net_u32 right)
{
    net_u32 distance = left - right;

    if (distance == 0U) {
        return TCP_SEQ_EQUAL;
    }
    if (distance == TCP_SEQ_HALF_SPACE) {
        return TCP_SEQ_UNORDERED;
    }
    return distance < TCP_SEQ_HALF_SPACE ? TCP_SEQ_AFTER : TCP_SEQ_BEFORE;
}

int tcp_seq_in_range(net_u32 sequence, net_u32 start, net_u32 length)
{
    return length < TCP_SEQ_HALF_SPACE && sequence - start < length;
}
