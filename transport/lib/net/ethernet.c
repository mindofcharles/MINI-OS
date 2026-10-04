#include "ethernet.h"

#include "address.h"
#include "byteorder.h"

#include <string.h>

_Static_assert(NET_ETHERNET_PAYLOAD_MAX == NET_IPV4_MTU,
               "Ethernet payload capacity must match the raw MTU");
_Static_assert(NET_FRAME_MIN >= NET_ETHERNET_HEADER_SIZE,
               "minimum frame must contain the Ethernet header");

static int mac_is_broadcast(const unsigned char *mac)
{
    static const unsigned char broadcast[6] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
    };

    return memcmp(mac, broadcast, sizeof(broadcast)) == 0;
}

int net_ethernet_decode(struct net_context *context, unsigned int frame_length,
                        struct net_ethernet_view *view)
{
    struct net_ethernet_view parsed;
    const unsigned char *frame;
    int classification;

    if (context == 0 || view == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (frame_length == 0U) {
        return NET_ERR_INVALID;
    }

    ++context->counters.received_frames;
    if (frame_length < NET_FRAME_MIN || frame_length > NET_FRAME_MAX) {
        ++context->counters.malformed_frames;
        return NET_ETHERNET_DROP;
    }

    frame = context->rx_frame;
    if (memcmp(frame, context->device_mac.octets, 6U) != 0 &&
        !mac_is_broadcast(frame)) {
        ++context->counters.unsupported_frames;
        return NET_ETHERNET_DROP;
    }
    if (net_mac_validate_unicast(frame + 6U) != 0) {
        ++context->counters.malformed_frames;
        return NET_ETHERNET_DROP;
    }

    parsed.ethertype = net_read_be16(frame + 12U);
    if (parsed.ethertype == NET_ETHERTYPE_ARP) {
        classification = NET_ETHERNET_ARP;
    } else if (parsed.ethertype == NET_ETHERTYPE_IPV4) {
        classification = NET_ETHERNET_IPV4;
    } else {
        ++context->counters.unsupported_frames;
        return NET_ETHERNET_DROP;
    }

    memcpy(parsed.destination.octets, frame,
           sizeof(parsed.destination.octets));
    memcpy(parsed.source.octets, frame + 6U,
           sizeof(parsed.source.octets));
    parsed.payload = frame + NET_ETHERNET_HEADER_SIZE;
    parsed.payload_length = frame_length - NET_ETHERNET_HEADER_SIZE;
    *view = parsed;
    return classification;
}

unsigned char *net_ethernet_begin_tx(struct net_context *context)
{
    if (context == 0 || !context->initialized) {
        return 0;
    }
    memset(context->tx_frame, 0, sizeof(context->tx_frame));
    context->tx_prepared = 1;
    return context->tx_frame + NET_ETHERNET_HEADER_SIZE;
}

int net_ethernet_send(struct net_context *context,
                      const struct net_mac_addr *destination,
                      net_u16 ethertype, unsigned int payload_length)
{
    struct net_mac_addr target;
    unsigned int logical_length;
    unsigned int wire_length;
    int result;

    if (context == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (destination == 0 || payload_length > NET_ETHERNET_PAYLOAD_MAX ||
        (ethertype != NET_ETHERTYPE_ARP && ethertype != NET_ETHERTYPE_IPV4) ||
        (net_mac_validate_unicast(destination->octets) != 0 &&
         !(ethertype == NET_ETHERTYPE_ARP &&
           mac_is_broadcast(destination->octets)))) {
        return NET_ERR_INVALID;
    }
    if (!context->tx_prepared) {
        return NET_ERR_STATE;
    }

    target = *destination;
    logical_length = NET_ETHERNET_HEADER_SIZE + payload_length;
    wire_length = logical_length < NET_FRAME_MIN ? NET_FRAME_MIN : logical_length;
    memcpy(context->tx_frame, target.octets, sizeof(target.octets));
    memcpy(context->tx_frame + 6U, context->device_mac.octets,
           sizeof(context->device_mac.octets));
    net_write_be16(context->tx_frame + 12U, ethertype);
    if (wire_length > logical_length) {
        memset(context->tx_frame + logical_length, 0,
               wire_length - logical_length);
    }

    context->tx_prepared = 0;
    result = net_send_frame(context->tx_frame, wire_length);
    if (result == (int)wire_length) {
        return 0;
    }
    if (result == SYS_ERR_UNAVAILABLE) {
        return NET_ERR_UNAVAILABLE;
    }
    return NET_ERR_DEVICE;
}
