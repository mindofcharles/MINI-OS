#include "internal.h"

#include <string.h>

unsigned int tcp_ring_offset(unsigned int head, unsigned int distance)
{
    return (head + distance) % TCP_QUEUE_CAPACITY;
}

/* The caller bounds the logical span to one queue and supplies its offset. */
void tcp_ring_write(net_u8 *queue, unsigned int offset, const void *data,
                     unsigned int length)
{
    const net_u8 *bytes = data;
    unsigned int first = TCP_QUEUE_CAPACITY - offset;

    if (length < first) {
        first = length;
    }
    if (first != 0U) {
        memcpy(queue + offset, bytes, first);
    }
    if (length > first) {
        memcpy(queue, bytes + first, length - first);
    }
}

void tcp_ring_read(const net_u8 *queue, unsigned int offset, void *data,
                    unsigned int length)
{
    net_u8 *bytes = data;
    unsigned int first = TCP_QUEUE_CAPACITY - offset;

    if (length < first) {
        first = length;
    }
    if (first != 0U) {
        memcpy(bytes, queue + offset, first);
    }
    if (length > first) {
        memcpy(bytes + first, queue, length - first);
    }
}
