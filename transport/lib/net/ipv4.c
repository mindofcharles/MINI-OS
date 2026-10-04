#include "ipv4.h"

#include "address.h"
#include "byteorder.h"
#include "checksum.h"
#include "icmp.h"

#include <string.h>

_Static_assert(NET_IPV4_PAYLOAD_MAX == 1480,
               "IPv4 payload capacity must match the Ethernet MTU");

int net_ipv4_decode(const struct net_context *context,
                    const struct net_ethernet_view *ethernet,
                    struct net_ipv4_view *packet)
{
    struct net_ipv4_view parsed;
    struct net_ipv4_addr next_hop;
    const unsigned char *ip;
    unsigned int header_length;
    unsigned int total_length;
    net_u16 fragment;

    if (context == 0 || ethernet == 0 || packet == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (ethernet->payload == 0 ||
        ethernet->payload_length < NET_IPV4_HEADER_SIZE ||
        ethernet->payload_length > NET_IPV4_MTU) {
        return NET_IPV4_MALFORMED;
    }
    ip = ethernet->payload;
    if ((ip[0] >> 4) != 4U || (ip[0] & 15U) < 5U) {
        return NET_IPV4_MALFORMED;
    }
    header_length = (unsigned int)(ip[0] & 15U) * 4U;
    total_length = net_read_be16(ip + 2U);
    if (header_length > ethernet->payload_length ||
        total_length < header_length ||
        total_length > ethernet->payload_length) {
        return NET_IPV4_MALFORMED;
    }
    if (header_length != NET_IPV4_HEADER_SIZE) {
        return NET_IPV4_UNSUPPORTED;
    }
    fragment = net_read_be16(ip + 6U);
    if ((fragment & 0x8000U) != 0U || ip[8] == 0U ||
        !net_checksum_valid(ip, header_length)) {
        return NET_IPV4_MALFORMED;
    }
    if ((fragment & 0x3FFFU) != 0U) {
        return NET_IPV4_UNSUPPORTED;
    }
    memcpy(parsed.source.octets, ip + 12U, 4U);
    memcpy(parsed.destination.octets, ip + 16U, 4U);
    if (net_ipv4_select_next_hop(&context->config, &parsed.source,
                                  &next_hop) != 0) {
        return NET_IPV4_MALFORMED;
    }
    if (memcmp(ethernet->destination.octets,
               context->device_mac.octets, 6U) != 0 ||
        memcmp(parsed.destination.octets,
               context->config.address.octets, 4U) != 0 ||
        ip[9] != NET_IPV4_PROTOCOL_ICMP) {
        return NET_IPV4_UNSUPPORTED;
    }
    parsed.payload = ip + header_length;
    parsed.payload_length = total_length - header_length;
    parsed.protocol = ip[9];
    parsed.ttl = ip[8];
    *packet = parsed;
    return NET_IPV4_ACCEPT;
}

int net_ipv4_handle(struct net_context *context,
                    const struct net_ethernet_view *ethernet)
{
    struct net_ipv4_view packet;
    int classification = net_ipv4_decode(context, ethernet, &packet);

    if (classification == NET_IPV4_MALFORMED) {
        ++context->counters.malformed_frames;
        return 0;
    }
    if (classification == NET_IPV4_UNSUPPORTED) {
        ++context->counters.unsupported_frames;
        return 0;
    }
    if (classification < 0) {
        return classification;
    }
    return net_icmp_handle(context, ethernet, &packet);
}

int net_ipv4_send(struct net_context *context,
                  const struct net_ipv4_addr *destination,
                  const struct net_mac_addr *next_hop_mac,
                  net_u8 protocol, unsigned int payload_length)
{
    struct net_ipv4_addr selected_hop;
    unsigned char *ip;

    if (context == 0 || destination == 0 || next_hop_mac == 0 ||
        payload_length > NET_IPV4_PAYLOAD_MAX ||
        protocol != NET_IPV4_PROTOCOL_ICMP) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized || !context->tx_prepared) {
        return NET_ERR_STATE;
    }
    if (net_ipv4_select_next_hop(&context->config, destination,
                                  &selected_hop) != 0) {
        return NET_ERR_NO_ROUTE;
    }
    ip = context->tx_frame + NET_ETHERNET_HEADER_SIZE;
    ip[0] = 0x45U;
    ip[1] = 0U;
    net_write_be16(ip + 2U,
                   (net_u16)(NET_IPV4_HEADER_SIZE + payload_length));
    net_write_be16(ip + 4U, 0U);
    net_write_be16(ip + 6U, 0x4000U);
    ip[8] = 64U;
    ip[9] = protocol;
    net_write_be16(ip + 10U, 0U);
    memcpy(ip + 12U, context->config.address.octets, 4U);
    memcpy(ip + 16U, destination->octets, 4U);
    net_write_be16(ip + 10U,
                   net_checksum_compute(ip, NET_IPV4_HEADER_SIZE));
    return net_ethernet_send(context, next_hop_mac, NET_ETHERTYPE_IPV4,
                             NET_IPV4_HEADER_SIZE + payload_length);
}
