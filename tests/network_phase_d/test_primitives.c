#include "../../transport/lib/net/byteorder.h"
#include "../../transport/lib/net/checksum.h"

#include <stdio.h>
#include <string.h>

_Alignas(8) static unsigned char maximum_frame[NET_FRAME_MAX + 1U];

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Phase D primitive test failed: %s\n", message);
        return 0;
    }
    return 1;
}

static int test_byte_order(void)
{
    _Alignas(8) unsigned char bytes[12];
    _Alignas(8) static const unsigned char known[7] = {
        0xA5U, 0x12U, 0x34U, 0x89U, 0xABU, 0xCDU, 0xEFU
    };

    memset(bytes, 0xA5, sizeof(bytes));
    net_write_be16(bytes + 1, 0x1234U);
    net_write_be32(bytes + 5, 0x89ABCDEFU);
    if (!require(bytes[0] == 0xA5U && bytes[3] == 0xA5U &&
                 bytes[4] == 0xA5U && bytes[9] == 0xA5U &&
                 bytes[11] == 0xA5U,
                 "writes preserve surrounding bytes") ||
        !require(bytes[1] == 0x12U && bytes[2] == 0x34U,
                 "16-bit wire representation") ||
        !require(bytes[5] == 0x89U && bytes[6] == 0xABU &&
                 bytes[7] == 0xCDU && bytes[8] == 0xEFU,
                 "32-bit wire representation") ||
        !require(net_read_be16(bytes + 1) == 0x1234U,
                 "unaligned 16-bit read") ||
        !require(net_read_be32(bytes + 5) == 0x89ABCDEFU,
                 "unaligned 32-bit read") ||
        !require(net_read_be16(known + 1) == 0x1234U,
                 "known 16-bit value") ||
        !require(net_read_be32(known + 3) == 0x89ABCDEFU,
                 "known 32-bit value")) {
        return 0;
    }

    net_write_be16(bytes + 1, 0U);
    net_write_be32(bytes + 5, 0xFFFFFFFFU);
    return require(net_read_be16(bytes + 1) == 0U,
                   "zero 16-bit round trip") &&
           require(net_read_be32(bytes + 5) == 0xFFFFFFFFU,
                   "maximum 32-bit round trip");
}

static int test_checksum_vectors(void)
{
    static const unsigned char even[] = {
        0x00U, 0x01U, 0xF2U, 0x03U, 0xF4U, 0xF5U, 0xF6U, 0xF7U
    };
    static const unsigned char odd[] = {
        0x00U, 0x01U, 0xF2U, 0x03U, 0xF4U, 0xF5U, 0xF6U, 0xF7U, 0xF8U
    };
    unsigned char header[] = {
        0x45U, 0x00U, 0x00U, 0x73U, 0x00U, 0x00U, 0x40U, 0x00U,
        0x40U, 0x11U, 0x00U, 0x00U, 0xC0U, 0xA8U, 0x00U, 0x01U,
        0xC0U, 0xA8U, 0x00U, 0xC7U
    };
    unsigned char odd_packet[] = {
        0x08U, 0x00U, 0x00U, 0x00U, 0x12U, 0x34U, 0x00U, 0x01U,
        0xA5U
    };

    if (!require(net_checksum_compute(0, 0U) == 0xFFFFU,
                 "empty checksum") ||
        !require(net_checksum_compute("\x01", 1U) == 0xFEFFU,
                 "single-byte checksum") ||
        !require(net_checksum_compute(even, sizeof(even)) == 0x220DU,
                 "known even checksum") ||
        !require(net_checksum_compute(odd, sizeof(odd)) == 0x2A0CU,
                 "known odd checksum") ||
        !require(net_checksum_compute(header, sizeof(header)) == 0xB861U,
                 "known IPv4 checksum")) {
        return 0;
    }

    net_write_be16(header + 10, net_checksum_compute(header, sizeof(header)));
    if (!require(net_checksum_valid(header, sizeof(header)),
                 "generated checksum validates")) {
        return 0;
    }
    net_write_be16(odd_packet + 2,
                   net_checksum_compute(odd_packet, sizeof(odd_packet)));
    if (!require(net_checksum_valid(odd_packet, sizeof(odd_packet)),
                 "generated odd checksum validates")) {
        return 0;
    }
    odd_packet[8] ^= 1U;
    if (!require(!net_checksum_valid(odd_packet, sizeof(odd_packet)),
                 "odd corruption invalidates checksum")) {
        return 0;
    }
    header[19] ^= 1U;
    return require(!net_checksum_valid(header, sizeof(header)),
                   "corruption invalidates checksum");
}

static int test_checksum_bounds(void)
{
    unsigned int index;

    maximum_frame[0] = 0xA5U;
    for (index = 0U; index < NET_FRAME_MAX; ++index) {
        maximum_frame[index + 1U] =
            (unsigned char)((index * 37U + 11U) & 0xFFU);
    }
    if (!require(net_checksum_compute(maximum_frame + 1, NET_FRAME_MAX) ==
                     0xEB7FU,
                 "maximum unaligned checksum") ||
        !require(net_checksum_compute(maximum_frame + 1,
                                      NET_FRAME_MAX - 1U) == 0xEC37U,
                 "maximum odd checksum") ||
        !require(maximum_frame[0] == 0xA5U,
                 "checksum does not alter input")) {
        return 0;
    }
    return 1;
}

int main(void)
{
    if (!test_byte_order() || !test_checksum_vectors() ||
        !test_checksum_bounds()) {
        return 1;
    }
    puts("Phase D primitive tests passed.");
    return 0;
}
