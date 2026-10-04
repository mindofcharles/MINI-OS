#include "icmp.h"

#include "byteorder.h"
#include "checksum.h"
#include "net_platform.h"

#include <string.h>

int net_icmp_parse(const struct net_ipv4_view *packet,
                   struct net_icmp_echo_view *echo)
{
    struct net_icmp_echo_view parsed;
    const unsigned char *icmp;
    int classification;

    if (packet == 0 || echo == 0) {
        return NET_ERR_INVALID;
    }
    if (packet->payload == 0 ||
        packet->payload_length < NET_ICMP_HEADER_SIZE ||
        packet->payload_length > NET_IPV4_PAYLOAD_MAX) {
        return NET_ICMP_MALFORMED;
    }
    icmp = packet->payload;
    if (!net_checksum_valid(icmp, packet->payload_length)) {
        return NET_ICMP_MALFORMED;
    }
    if (icmp[0] == 8U) {
        classification = NET_ICMP_ECHO_REQUEST;
    } else if (icmp[0] == 0U) {
        classification = NET_ICMP_ECHO_REPLY;
    } else {
        return NET_ICMP_UNSUPPORTED;
    }
    if (icmp[1] != 0U) {
        return NET_ICMP_MALFORMED;
    }
    parsed.identifier = net_read_be16(icmp + 4U);
    parsed.sequence = net_read_be16(icmp + 6U);
    parsed.payload = icmp + NET_ICMP_HEADER_SIZE;
    parsed.payload_length = packet->payload_length - NET_ICMP_HEADER_SIZE;
    *echo = parsed;
    return classification;
}

static int send_echo(struct net_context *context,
                     const struct net_ipv4_addr *destination,
                     const struct net_mac_addr *next_hop_mac,
                     net_u8 type, net_u16 identifier, net_u16 sequence,
                     const void *payload, unsigned int payload_length)
{
    unsigned char *ip;
    unsigned char *icmp;
    unsigned int icmp_length;

    if (context == 0 || destination == 0 || next_hop_mac == 0 ||
        payload_length > NET_ICMP_ECHO_PAYLOAD_MAX ||
        (payload_length != 0U && payload == 0)) {
        return NET_ERR_INVALID;
    }
    ip = net_ethernet_begin_tx(context);
    if (ip == 0) {
        return NET_ERR_STATE;
    }
    icmp = ip + NET_IPV4_HEADER_SIZE;
    icmp_length = NET_ICMP_HEADER_SIZE + payload_length;
    icmp[0] = type;
    icmp[1] = 0U;
    net_write_be16(icmp + 2U, 0U);
    net_write_be16(icmp + 4U, identifier);
    net_write_be16(icmp + 6U, sequence);
    if (payload_length != 0U) {
        memcpy(icmp + NET_ICMP_HEADER_SIZE, payload, payload_length);
    }
    net_write_be16(icmp + 2U, net_checksum_compute(icmp, icmp_length));
    if (type == 8U && context->pending_echo.active) {
        context->pending_echo.sent_ms = net_clock_now_ms();
    }
    return net_ipv4_send(context, destination, next_hop_mac,
                          NET_IPV4_PROTOCOL_ICMP, icmp_length);
}

int net_icmp_send_request(struct net_context *context,
                          const struct net_ipv4_addr *destination,
                          const struct net_mac_addr *next_hop_mac,
                          net_u16 identifier, net_u16 sequence,
                          const void *payload, unsigned int payload_length)
{
    return send_echo(context, destination, next_hop_mac, 8U,
                     identifier, sequence, payload, payload_length);
}

int net_icmp_handle(struct net_context *context,
                    const struct net_ethernet_view *ethernet,
                    const struct net_ipv4_view *packet)
{
    struct net_icmp_echo_view echo;
    struct net_pending_echo *pending;
    net_u32 now;
    int classification;
    int result;

    if (context == 0 || ethernet == 0 || packet == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    classification = net_icmp_parse(packet, &echo);
    if (classification == NET_ICMP_MALFORMED) {
        ++context->counters.malformed_frames;
        return 0;
    }
    if (classification == NET_ICMP_UNSUPPORTED) {
        ++context->counters.unsupported_frames;
        return 0;
    }
    if (classification < 0) {
        return classification;
    }
    if (classification == NET_ICMP_ECHO_REQUEST) {
        ++context->counters.echo_requests;
        result = send_echo(context, &packet->source,
                           &ethernet->source, 0U, echo.identifier,
                           echo.sequence, echo.payload, echo.payload_length);
        return result == 0 ? 1 : result;
    }
    ++context->counters.echo_replies;
    pending = &context->pending_echo;
    if (!pending->active || pending->matched ||
        memcmp(packet->source.octets,
               pending->destination.octets, 4U) != 0 ||
        memcmp(packet->destination.octets,
               context->config.address.octets, 4U) != 0 ||
        echo.identifier != pending->identifier ||
        echo.sequence != pending->sequence ||
        echo.payload_length != pending->payload_length ||
        (echo.payload_length != 0U &&
         memcmp(echo.payload, pending->payload,
                echo.payload_length) != 0)) {
        return 0;
    }
    now = net_clock_now_ms();
    if (pending->timeout_ms != 0U &&
        net_timeout_expired(pending->started_ms, now,
                            pending->timeout_ms)) {
        return 0;
    }
    pending->reply_source = packet->source;
    pending->elapsed_ms = net_elapsed_ms(pending->sent_ms, now);
    pending->matched = 1;
    return 1;
}
