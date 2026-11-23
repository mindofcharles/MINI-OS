#include "checksum.h"

static net_u32 fold_carry(net_u32 sum)
{
    return (sum & 0xFFFFU) + (sum >> 16);
}

void net_checksum_add(struct net_checksum *checksum, const void *buffer,
                      unsigned int length)
{
    const net_u8 *bytes = buffer;
    net_u32 sum = checksum->sum;

    if (length == 0U) {
        return;
    }
    if (checksum->odd) {
        sum = fold_carry(sum + *bytes++);
        --length;
    }
    while (length >= 2U) {
        sum += ((net_u32)bytes[0] << 8) | (net_u32)bytes[1];
        sum = fold_carry(sum);
        bytes += 2;
        length -= 2U;
    }
    if (length != 0U) {
        sum += (net_u32)bytes[0] << 8;
        sum = fold_carry(sum);
    }
    checksum->sum = sum;
    checksum->odd = length != 0U;
}

net_u16 net_checksum_finish(const struct net_checksum *checksum)
{
    return (net_u16)~checksum->sum;
}

net_u16 net_checksum_compute(const void *buffer, unsigned int length)
{
    struct net_checksum checksum = {0};

    net_checksum_add(&checksum, buffer, length);
    return net_checksum_finish(&checksum);
}

int net_checksum_valid(const void *buffer, unsigned int length)
{
    return net_checksum_compute(buffer, length) == 0U;
}
