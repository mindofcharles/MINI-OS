#include "byteorder.h"

net_u16 net_read_be16(const void *buffer)
{
    const net_u8 *bytes = buffer;

    return (net_u16)(((net_u16)bytes[0] << 8) |
                     (net_u16)bytes[1]);
}

net_u32 net_read_be32(const void *buffer)
{
    const net_u8 *bytes = buffer;

    return ((net_u32)bytes[0] << 24) |
           ((net_u32)bytes[1] << 16) |
           ((net_u32)bytes[2] << 8) |
           (net_u32)bytes[3];
}

void net_write_be16(void *buffer, net_u16 value)
{
    net_u8 *bytes = buffer;

    bytes[0] = (net_u8)(value >> 8);
    bytes[1] = (net_u8)value;
}

void net_write_be32(void *buffer, net_u32 value)
{
    net_u8 *bytes = buffer;

    bytes[0] = (net_u8)(value >> 24);
    bytes[1] = (net_u8)(value >> 16);
    bytes[2] = (net_u8)(value >> 8);
    bytes[3] = (net_u8)value;
}
