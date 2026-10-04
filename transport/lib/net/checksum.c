#include "checksum.h"

static net_u32 fold_carry(net_u32 sum)
{
    return (sum & 0xFFFFU) + (sum >> 16);
}

net_u16 net_checksum_compute(const void *buffer, unsigned int length)
{
    const net_u8 *bytes = buffer;
    net_u32 sum = 0U;

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
    sum = fold_carry(sum);
    return (net_u16)(~sum & 0xFFFFU);
}

int net_checksum_valid(const void *buffer, unsigned int length)
{
    return net_checksum_compute(buffer, length) == 0U;
}
