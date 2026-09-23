#include "deterministic_backend.h"

#include "../../transport/lib/net/arp.h"
#include "../../transport/lib/net/byteorder.h"
#include "../../transport/lib/net/checksum.h"
#include "../../transport/lib/net/icmp.h"
#include "../../transport/lib/net/ipv4.h"
#include "../../transport/lib/net/net_platform.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const struct net_ipv4_addr local_ip = {{10U, 0U, 2U, 15U}};
static const struct net_ipv4_addr gateway_ip = {{10U, 0U, 2U, 2U}};
static const struct net_ipv4_addr remote_ip = {{203U, 0U, 113U, 9U}};
static const struct net_ipv4_addr other_ip = {{10U, 0U, 2U, 3U}};
static const struct net_mac_addr local_mac = {
    {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U}
};
static const struct net_mac_addr gateway_mac = {
    {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U}
};
static const struct net_mac_addr broadcast_mac = {
    {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU}
};

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Phase D IPv4 test failed: %s\n", message);
        return 0;
    }
    return 1;
}

static int initialize(void)
{
    struct net_config config = {
        {{10U, 0U, 2U, 15U}},
        {{255U, 255U, 255U, 0U}},
        {{10U, 0U, 2U, 2U}},
        NET_ARP_CACHE_TTL_DEFAULT_MS,
        NET_ARP_RETRY_INTERVAL_MIN_MS,
        NET_ARP_RETRY_COUNT_DEFAULT
    };

    phase_d_backend_reset();
    memset(&net_global_context, 0, sizeof(net_global_context));
    return require(net_init(&config) == 0, "initialize stack");
}

static void fix_ip_checksum(unsigned char *frame)
{
    unsigned char *ip = frame + NET_ETHERNET_HEADER_SIZE;

    net_write_be16(ip + 10U, 0U);
    net_write_be16(ip + 10U,
                   net_checksum_compute(ip, NET_IPV4_HEADER_SIZE));
}

static void fix_icmp_checksum(unsigned char *frame, unsigned int length)
{
    unsigned char *icmp = frame + NET_ETHERNET_HEADER_SIZE +
                          NET_IPV4_HEADER_SIZE;

    net_write_be16(icmp + 2U, 0U);
    net_write_be16(icmp + 2U, net_checksum_compute(icmp, length));
}

static unsigned int make_echo_frame(unsigned char *frame,
                                    const struct net_mac_addr *ethernet_dest,
                                    const struct net_mac_addr *ethernet_source,
                                    const struct net_ipv4_addr *source,
                                    const struct net_ipv4_addr *destination,
                                    unsigned int type, net_u16 identifier,
                                    net_u16 sequence, const void *payload,
                                    unsigned int payload_length)
{
    unsigned char *ip = frame + NET_ETHERNET_HEADER_SIZE;
    unsigned char *icmp = ip + NET_IPV4_HEADER_SIZE;
    unsigned int icmp_length = NET_ICMP_HEADER_SIZE + payload_length;
    unsigned int length = NET_ETHERNET_HEADER_SIZE +
                          NET_IPV4_HEADER_SIZE + icmp_length;

    memset(frame, 0xA5, NET_FRAME_MAX);
    memcpy(frame, ethernet_dest->octets, 6U);
    memcpy(frame + 6U, ethernet_source->octets, 6U);
    net_write_be16(frame + 12U, NET_ETHERTYPE_IPV4);
    ip[0] = 0x45U;
    ip[1] = 0U;
    net_write_be16(ip + 2U,
                   (net_u16)(NET_IPV4_HEADER_SIZE + icmp_length));
    net_write_be16(ip + 4U, 0x1234U);
    net_write_be16(ip + 6U, 0x4000U);
    ip[8] = 64U;
    ip[9] = NET_IPV4_PROTOCOL_ICMP;
    memcpy(ip + 12U, source->octets, 4U);
    memcpy(ip + 16U, destination->octets, 4U);
    fix_ip_checksum(frame);
    icmp[0] = (unsigned char)type;
    icmp[1] = 0U;
    net_write_be16(icmp + 4U, identifier);
    net_write_be16(icmp + 6U, sequence);
    if (payload_length != 0U) {
        memcpy(icmp + NET_ICMP_HEADER_SIZE, payload, payload_length);
    }
    fix_icmp_checksum(frame, icmp_length);
    return length < NET_FRAME_MIN ? NET_FRAME_MIN : length;
}

static void view_from_frame(const unsigned char *frame,
                            unsigned int length,
                            struct net_ethernet_view *view)
{
    memset(view, 0, sizeof(*view));
    memcpy(view->destination.octets, frame, 6U);
    memcpy(view->source.octets, frame + 6U, 6U);
    view->payload = frame + NET_ETHERNET_HEADER_SIZE;
    view->payload_length = length - NET_ETHERNET_HEADER_SIZE;
    view->ethertype = NET_ETHERTYPE_IPV4;
}

static int queue_echo(const struct net_ipv4_addr *source,
                      net_u16 identifier, net_u16 sequence,
                      const void *payload, unsigned int payload_length)
{
    unsigned char frame[NET_FRAME_MAX];
    unsigned int length = make_echo_frame(frame, &local_mac,
                                          &gateway_mac, source, &local_ip,
                                          0U, identifier, sequence,
                                          payload, payload_length);

    return require(phase_d_backend_queue_receive(frame, length) == 0,
                   "queue Echo Reply");
}

static void make_gateway_arp_reply(unsigned char *frame)
{
    memset(frame, 0, NET_FRAME_MIN);
    memcpy(frame, local_mac.octets, 6U);
    memcpy(frame + 6U, gateway_mac.octets, 6U);
    net_write_be16(frame + 12U, NET_ETHERTYPE_ARP);
    net_write_be16(frame + 14U, 1U);
    net_write_be16(frame + 16U, NET_ETHERTYPE_IPV4);
    frame[18] = 6U;
    frame[19] = 4U;
    net_write_be16(frame + 20U, NET_ARP_REPLY);
    memcpy(frame + 22U, gateway_mac.octets, 6U);
    memcpy(frame + 28U, gateway_ip.octets, 4U);
    memcpy(frame + 32U, local_mac.octets, 6U);
    memcpy(frame + 38U, local_ip.octets, 4U);
}

static int test_ipv4_decode(void)
{
    unsigned char frame[NET_FRAME_MAX];
    unsigned char *ip = frame + NET_ETHERNET_HEADER_SIZE;
    struct net_ethernet_view ethernet;
    struct net_ipv4_view parsed;
    struct net_ipv4_view sentinel;
    unsigned int length;

    if (!initialize()) {
        return 0;
    }
    length = make_echo_frame(frame, &local_mac, &gateway_mac,
                              &gateway_ip, &local_ip, 8U, 7U, 9U,
                              "A", 1U);
    view_from_frame(frame, length, &ethernet);
    memset(&parsed, 0xA5, sizeof(parsed));
    sentinel = parsed;
    for (length = 0U; length < NET_IPV4_HEADER_SIZE; ++length) {
        ethernet.payload_length = length;
        if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                      &parsed) == NET_IPV4_MALFORMED &&
                     memcmp(&parsed, &sentinel, sizeof(parsed)) == 0,
                     "every truncated IPv4 fixed header is rejected")) {
            return 0;
        }
    }
    ethernet.payload_length = NET_FRAME_MIN - NET_ETHERNET_HEADER_SIZE;
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_ACCEPT &&
                 parsed.payload_length == 9U &&
                 parsed.payload[8] == 'A' &&
                 parsed.ttl == 64U,
                 "IPv4 total length excludes Ethernet padding")) {
        return 0;
    }
    ip[0] = 0x44U;
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_MALFORMED,
                 "IHL below five")) {
        return 0;
    }
    ip[0] = 0x46U;
    net_write_be16(ip + 2U, 24U);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_UNSUPPORTED,
                 "IP options deliberately unsupported")) {
        return 0;
    }
    ip[0] = 0x45U;
    net_write_be16(ip + 2U, 19U);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_MALFORMED,
                 "total length below header")) {
        return 0;
    }
    net_write_be16(ip + 2U, 47U);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_MALFORMED,
                 "total length beyond Ethernet data")) {
        return 0;
    }
    net_write_be16(ip + 2U, 29U);
    fix_ip_checksum(frame);
    ip[10] ^= 1U;
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_MALFORMED,
                 "bad IPv4 checksum")) {
        return 0;
    }
    fix_ip_checksum(frame);
    net_write_be16(ip + 6U, 0x8000U);
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_MALFORMED,
                 "reserved fragment flag")) {
        return 0;
    }
    net_write_be16(ip + 6U, 0x2000U);
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_UNSUPPORTED,
                 "more-fragments flag")) {
        return 0;
    }
    net_write_be16(ip + 6U, 1U);
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_UNSUPPORTED,
                 "nonzero fragment offset")) {
        return 0;
    }
    net_write_be16(ip + 6U, 0x4000U);
    ip[8] = 0U;
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_MALFORMED,
                 "zero TTL")) {
        return 0;
    }
    ip[8] = 1U;
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_ACCEPT,
                 "TTL one reaches local host")) {
        return 0;
    }
    ip[9] = 6U;
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_UNSUPPORTED,
                 "non-ICMP protocol")) {
        return 0;
    }
    ip[9] = NET_IPV4_PROTOCOL_ICMP;
    memcpy(ip + 12U, remote_ip.octets, 4U);
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_ACCEPT,
                 "off-link unicast source is valid")) {
        return 0;
    }
    memcpy(ethernet.destination.octets, broadcast_mac.octets, 6U);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_UNSUPPORTED,
                 "broadcast Ethernet with unicast IP is dropped")) {
        return 0;
    }
    memcpy(ethernet.destination.octets, local_mac.octets, 6U);
    memcpy(ip + 12U, local_ip.octets, 4U);
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_MALFORMED,
                 "local address cannot be remote source")) {
        return 0;
    }
    ip[12] = 0U;
    ip[13] = 0U;
    ip[14] = 0U;
    ip[15] = 0U;
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_MALFORMED,
                 "zero source is invalid")) {
        return 0;
    }
    ip[12] = 224U;
    fix_ip_checksum(frame);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &parsed) == NET_IPV4_MALFORMED,
                 "multicast source is invalid")) {
        return 0;
    }
    memcpy(ip + 12U, gateway_ip.octets, 4U);
    memcpy(ip + 16U, other_ip.octets, 4U);
    fix_ip_checksum(frame);
    return require(net_ipv4_decode(&net_global_context, &ethernet,
                                    &parsed) == NET_IPV4_UNSUPPORTED,
                   "nonlocal destination is not delivered");
}

static int test_icmp_decode_and_send(void)
{
    unsigned char frame[NET_FRAME_MAX];
    unsigned char *ip;
    unsigned char *icmp;
    struct net_ethernet_view ethernet;
    struct net_ipv4_view ipv4;
    struct net_icmp_echo_view echo;
    struct net_icmp_echo_view sentinel;
    unsigned char maximum_payload[NET_ICMP_ECHO_PAYLOAD_MAX];
    const unsigned char *sent;
    unsigned int length;
    unsigned int index;

    if (!initialize()) {
        return 0;
    }
    length = make_echo_frame(frame, &local_mac, &gateway_mac,
                              &gateway_ip, &local_ip, 8U, 0x1234U,
                              0x5678U, "abc", 3U);
    view_from_frame(frame, length, &ethernet);
    if (!require(net_ipv4_decode(&net_global_context, &ethernet,
                                  &ipv4) == NET_IPV4_ACCEPT,
                 "parse ICMP-bearing IPv4")) {
        return 0;
    }
    memset(&echo, 0xA5, sizeof(echo));
    sentinel = echo;
    for (length = 0U; length < NET_ICMP_HEADER_SIZE; ++length) {
        ipv4.payload_length = length;
        if (!require(net_icmp_parse(&ipv4, &echo) ==
                         NET_ICMP_MALFORMED &&
                     memcmp(&echo, &sentinel, sizeof(echo)) == 0,
                     "every truncated Echo header is rejected")) {
            return 0;
        }
    }
    ipv4.payload_length = 11U;
    if (!require(net_icmp_parse(&ipv4, &echo) ==
                     NET_ICMP_ECHO_REQUEST &&
                 echo.identifier == 0x1234U &&
                 echo.sequence == 0x5678U &&
                 echo.payload_length == 3U &&
                 memcmp(echo.payload, "abc", 3U) == 0,
                 "odd-length Echo decoded without padding")) {
        return 0;
    }
    icmp = frame + NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE;
    icmp[8] ^= 1U;
    if (!require(net_icmp_parse(&ipv4, &echo) ==
                     NET_ICMP_MALFORMED,
                 "bad ICMP checksum")) {
        return 0;
    }
    icmp[8] ^= 1U;
    icmp[1] = 1U;
    fix_icmp_checksum(frame, 11U);
    if (!require(net_icmp_parse(&ipv4, &echo) ==
                     NET_ICMP_MALFORMED,
                 "nonzero Echo code")) {
        return 0;
    }
    icmp[1] = 0U;
    icmp[0] = 3U;
    fix_icmp_checksum(frame, 11U);
    if (!require(net_icmp_parse(&ipv4, &echo) ==
                     NET_ICMP_UNSUPPORTED,
                 "ICMP errors are outside D5")) {
        return 0;
    }
    if (!require(net_icmp_send_request(&net_global_context, &gateway_ip,
                                       &gateway_mac, 0x1234U, 0x5678U,
                                       "abc", 3U) == 0,
                 "send odd-length Echo Request")) {
        return 0;
    }
    sent = phase_d_backend_transmit_frame(0U);
    ip = (unsigned char *)sent + NET_ETHERNET_HEADER_SIZE;
    icmp = ip + NET_IPV4_HEADER_SIZE;
    if (!require(phase_d_backend_transmit_length(0U) == NET_FRAME_MIN &&
                 memcmp(sent, gateway_mac.octets, 6U) == 0 &&
                 memcmp(sent + 6U, local_mac.octets, 6U) == 0 &&
                 net_read_be16(sent + 12U) == NET_ETHERTYPE_IPV4 &&
                 ip[0] == 0x45U && ip[8] == 64U && ip[9] == 1U &&
                 net_read_be16(ip + 2U) == 31U &&
                 net_read_be16(ip + 4U) == 0U &&
                 net_read_be16(ip + 6U) == 0x4000U &&
                 net_checksum_valid(ip, 20U) &&
                 net_checksum_valid(icmp, 11U) &&
                 icmp[0] == 8U && icmp[1] == 0U &&
                 net_read_be16(icmp + 4U) == 0x1234U &&
                 net_read_be16(icmp + 6U) == 0x5678U &&
                 memcmp(icmp + 8U, "abc", 3U) == 0,
                 "exact outgoing IPv4 and ICMP fields")) {
        return 0;
    }
    for (index = 45U; index < NET_FRAME_MIN; ++index) {
        if (!require(sent[index] == 0U, "zero Ethernet padding")) {
            return 0;
        }
    }
    for (index = 0U; index < sizeof(maximum_payload); ++index) {
        maximum_payload[index] = (unsigned char)index;
    }
    if (!require(net_icmp_send_request(&net_global_context, &gateway_ip,
                                       &gateway_mac, 1U, 2U,
                                       maximum_payload,
                                       sizeof(maximum_payload)) == 0 &&
                 phase_d_backend_transmit_length(1U) == NET_FRAME_MAX,
                 "maximum Echo payload fits one full Ethernet frame")) {
        return 0;
    }
    sent = phase_d_backend_transmit_frame(1U);
    ip = (unsigned char *)sent + NET_ETHERNET_HEADER_SIZE;
    icmp = ip + NET_IPV4_HEADER_SIZE;
    return require(net_read_be16(ip + 2U) == NET_IPV4_MTU &&
                   net_checksum_valid(ip, NET_IPV4_HEADER_SIZE) &&
                   net_checksum_valid(icmp, NET_IPV4_PAYLOAD_MAX) &&
                   memcmp(icmp + NET_ICMP_HEADER_SIZE, maximum_payload,
                          sizeof(maximum_payload)) == 0 &&
                   net_icmp_send_request(&net_global_context, &gateway_ip,
                                          &gateway_mac, 1U, 2U, 0,
                                          NET_ICMP_ECHO_PAYLOAD_MAX + 1U) ==
                       NET_ERR_INVALID,
                   "full-size checksum, payload, and oversize rejection");
}

static int test_echo_server(void)
{
    unsigned char frame[NET_FRAME_MAX];
    const unsigned char *sent;
    const unsigned char *ip;
    const unsigned char *icmp;
    unsigned int length;

    if (!initialize()) {
        return 0;
    }
    length = make_echo_frame(frame, &local_mac, &gateway_mac,
                              &gateway_ip, &local_ip, 8U, 7U, 9U,
                              "A", 1U);
    frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE + 8U] ^= 1U;
    if (!require(phase_d_backend_queue_receive(frame, length) == 0 &&
                 net_poll(0U) == 0 &&
                 phase_d_backend_transmit_count() == 0U &&
                 net_global_context.counters.malformed_frames == 1U,
                 "corrupt request does not receive a reply")) {
        return 0;
    }
    length = make_echo_frame(frame, &local_mac, &gateway_mac,
                              &gateway_ip, &local_ip, 8U, 7U, 9U,
                              "A", 1U);
    if (!require(phase_d_backend_queue_receive(frame, length) == 0 &&
                 net_poll(0U) == 1 &&
                 phase_d_backend_transmit_count() == 1U &&
                 net_global_context.counters.echo_requests == 1U,
                 "valid request receives one reply")) {
        return 0;
    }
    sent = phase_d_backend_transmit_frame(0U);
    ip = sent + NET_ETHERNET_HEADER_SIZE;
    icmp = ip + NET_IPV4_HEADER_SIZE;
    return require(phase_d_backend_transmit_length(0U) == NET_FRAME_MIN &&
                   memcmp(sent, gateway_mac.octets, 6U) == 0 &&
                   memcmp(ip + 12U, local_ip.octets, 4U) == 0 &&
                   memcmp(ip + 16U, gateway_ip.octets, 4U) == 0 &&
                   net_read_be16(ip + 2U) == 29U &&
                   net_checksum_valid(ip, 20U) &&
                   icmp[0] == 0U && icmp[1] == 0U &&
                   net_read_be16(icmp + 4U) == 7U &&
                   net_read_be16(icmp + 6U) == 9U &&
                   icmp[8] == 'A' && net_checksum_valid(icmp, 9U) &&
                   sent[43] == 0U,
                   "reply preserves exact Echo data and omits padding");
}

static int test_ping_match(void)
{
    static const char payload[] = "abc";
    struct net_ping_result result;
    net_u16 identifier;
    unsigned int length;

    if (!initialize() ||
        !require(net_arp_cache_store(&net_global_context,
                                     &gateway_ip, &gateway_mac) == 0,
                 "prime gateway cache")) {
        return 0;
    }
    identifier = (net_u16)(net_global_context.next_echo_identifier + 1U);
    if (!queue_echo(&other_ip, identifier, 5U, payload, 3U) ||
        !queue_echo(&gateway_ip, (net_u16)(identifier + 1U), 5U,
                    payload, 3U) ||
        !queue_echo(&gateway_ip, identifier, 6U, payload, 3U) ||
        !queue_echo(&gateway_ip, identifier, 5U, "abd", 3U) ||
        !queue_echo(&gateway_ip, identifier, 5U, "ab", 2U) ||
        !queue_echo(&gateway_ip, identifier, 5U, payload, 3U)) {
        return 0;
    }
    phase_d_backend_set_receive_clock_step(10U);
    memset(&result, 0xA5, sizeof(result));
    if (!require(net_ping(&gateway_ip, 5U, payload, 3U,
                           100U, &result) == 0 &&
                 result.sequence == 5U &&
                 result.payload_length == 3U &&
                 result.elapsed_ms == 60U &&
                 memcmp(result.source.octets,
                        gateway_ip.octets, 4U) == 0 &&
                 phase_d_backend_transmit_count() == 1U &&
                 net_global_context.counters.echo_replies == 6U &&
                 !net_global_context.pending_echo.active &&
                 !net_global_context.service_active,
                 "only fully matching Echo Reply completes Ping")) {
        return 0;
    }
    length = phase_d_backend_transmit_length(0U);
    return require(length == NET_FRAME_MIN &&
                   phase_d_backend_transmit_frame(0U)[34] == 8U &&
                   net_read_be16(phase_d_backend_transmit_frame(0U) +
                                  38U) == identifier,
                   "Ping sends the selected identifier and payload");
}

static int test_deadlines_and_zero(void)
{
    unsigned char frame[NET_FRAME_MAX];
    struct net_ping_result result;
    struct net_ping_result sentinel;
    net_u16 identifier;
    unsigned int length;

    if (!initialize() ||
        !require(net_arp_cache_store(&net_global_context,
                                     &gateway_ip, &gateway_mac) == 0,
                 "prime cache for deadline")) {
        return 0;
    }
    memset(&result, 0xA5, sizeof(result));
    sentinel = result;
    identifier = (net_u16)(net_global_context.next_echo_identifier + 1U);
    if (!queue_echo(&gateway_ip, identifier, 99U, 0, 0U) ||
        !queue_echo(&gateway_ip, identifier, 99U, 0, 0U) ||
        !queue_echo(&gateway_ip, identifier, 99U, 0, 0U)) {
        return 0;
    }
    phase_d_backend_set_receive_clock_step(10U);
    if (!require(net_ping(&gateway_ip, 1U, 0, 0U, 25U,
                           &result) == NET_ERR_TIMEOUT &&
                 memcmp(&result, &sentinel, sizeof(result)) == 0 &&
                 phase_d_backend_transmit_count() == 1U &&
                 !net_global_context.pending_echo.active &&
                 !net_global_context.service_active &&
                 net_global_context.counters.received_frames == 3U,
                 "unrelated replies do not extend overall deadline")) {
        return 0;
    }
    if (!initialize()) {
        return 0;
    }
    memset(&result, 0xA5, sizeof(result));
    sentinel = result;
    make_gateway_arp_reply(frame);
    if (!require(phase_d_backend_queue_receive(frame, NET_FRAME_MIN) == 0 &&
                 net_ping(&gateway_ip, 1U, 0, 0U, 0U,
                           &result) == NET_ERR_TIMEOUT &&
                 memcmp(&result, &sentinel, sizeof(result)) == 0 &&
                 phase_d_backend_transmit_count() == 1U &&
                 net_read_be16(phase_d_backend_transmit_frame(0U) + 12U) ==
                     NET_ETHERTYPE_ARP,
                 "zero timeout spends one receive opportunity on ARP")) {
        return 0;
    }
    phase_d_backend_clear_transmits();
    identifier = (net_u16)(net_global_context.next_echo_identifier + 1U);
    length = make_echo_frame(frame, &local_mac, &gateway_mac,
                              &gateway_ip, &local_ip, 0U,
                              identifier, 2U, 0, 0U);
    return require(phase_d_backend_queue_receive(frame, length) == 0 &&
                   net_ping(&gateway_ip, 2U, 0, 0U, 0U,
                             &result) == 0 &&
                   result.sequence == 2U &&
                   phase_d_backend_transmit_count() == 1U &&
                   net_read_be16(phase_d_backend_transmit_frame(0U) + 12U) ==
                       NET_ETHERTYPE_IPV4,
                   "zero timeout cache hit allows one Echo receive");
}

static int test_arp_and_echo_share_deadline(void)
{
    unsigned char frame[NET_FRAME_MAX];
    struct net_ping_result result;
    struct net_ping_result sentinel;
    net_u16 identifier;
    unsigned int length;

    if (!initialize()) {
        return 0;
    }
    memset(&result, 0xA5, sizeof(result));
    sentinel = result;
    identifier = (net_u16)(net_global_context.next_echo_identifier + 1U);
    make_gateway_arp_reply(frame);
    if (!require(phase_d_backend_queue_receive(frame, NET_FRAME_MIN) == 0,
                 "queue ARP Reply before Ping")) {
        return 0;
    }
    length = make_echo_frame(frame, &local_mac, &gateway_mac,
                              &gateway_ip, &local_ip, 0U,
                              identifier, 1U, 0, 0U);
    if (!require(phase_d_backend_queue_receive(frame, length) == 0,
                 "queue Echo Reply after ARP")) {
        return 0;
    }
    phase_d_backend_set_receive_clock_step(20U);
    return require(net_ping(&gateway_ip, 1U, 0, 0U,
                             30U, &result) == NET_ERR_TIMEOUT &&
                   memcmp(&result, &sentinel, sizeof(result)) == 0 &&
                   phase_d_backend_transmit_count() == 2U &&
                   net_read_be16(phase_d_backend_transmit_frame(0U) + 12U) ==
                       NET_ETHERTYPE_ARP &&
                   net_read_be16(phase_d_backend_transmit_frame(1U) + 12U) ==
                       NET_ETHERTYPE_IPV4 &&
                   !net_global_context.service_active &&
                   !net_global_context.pending_echo.active,
                   "ARP and Echo never restart the original deadline");
}

struct cancellation_probe {
    unsigned int calls;
    unsigned int limit;
};

static int cancel_after_calls(void *argument)
{
    struct cancellation_probe *probe = argument;

    ++probe->calls;
    return probe->calls >= probe->limit;
}

static int cancel_on_one_call(void *argument)
{
    struct cancellation_probe *probe = argument;

    ++probe->calls;
    return probe->calls == probe->limit;
}

static int test_late_reply_and_wait_cancellation(void)
{
    struct net_ping_result result;
    struct net_ping_result sentinel;
    struct cancellation_probe probe;
    net_u16 identifier;

    if (!initialize() ||
        !require(net_arp_cache_store(&net_global_context,
                                     &gateway_ip, &gateway_mac) == 0,
                 "prime cache for late reply")) {
        return 0;
    }
    memset(&result, 0xA5, sizeof(result));
    sentinel = result;
    identifier = (net_u16)(net_global_context.next_echo_identifier + 1U);
    if (!queue_echo(&gateway_ip, identifier, 1U, 0, 0U)) {
        return 0;
    }
    phase_d_backend_set_receive_clock_step(30U);
    if (!require(net_ping(&gateway_ip, 1U, 0, 0U,
                           20U, &result) == NET_ERR_TIMEOUT &&
                 net_global_context.counters.received_frames == 1U &&
                 net_global_context.counters.echo_replies == 0U &&
                 memcmp(&result, &sentinel, sizeof(result)) == 0,
                 "reply observed after deadline cannot complete Ping")) {
        return 0;
    }

    if (!initialize() ||
        !require(net_arp_cache_store(&net_global_context,
                                     &gateway_ip, &gateway_mac) == 0,
                 "prime cache for wait cancellation")) {
        return 0;
    }
    memset(&result, 0xA5, sizeof(result));
    sentinel = result;
    probe.calls = 0U;
    probe.limit = 4U;
    net_set_cancel_callback(cancel_after_calls, &probe);
    if (!require(net_ping(&gateway_ip, 1U, 0, 0U,
                           100U, &result) == NET_ERR_CANCELLED &&
                 phase_d_backend_transmit_count() == 1U &&
                 !net_global_context.service_active &&
                 !net_global_context.pending_echo.active &&
                 memcmp(&result, &sentinel, sizeof(result)) == 0,
                 "cancellation during Echo wait releases active state")) {
        return 0;
    }

    if (!initialize() ||
        !require(net_arp_cache_store(&net_global_context,
                                     &gateway_ip, &gateway_mac) == 0,
                 "prime cache for one-shot cancellation")) {
        return 0;
    }
    memset(&result, 0xA5, sizeof(result));
    sentinel = result;
    identifier = (net_u16)(net_global_context.next_echo_identifier + 1U);
    if (!queue_echo(&gateway_ip, identifier, 1U, 0, 0U)) {
        return 0;
    }
    probe.calls = 0U;
    probe.limit = 4U;
    net_set_cancel_callback(cancel_on_one_call, &probe);
    phase_d_backend_set_receive_clock_step(10U);
    return require(net_ping(&gateway_ip, 1U, 0, 0U,
                             20U, &result) == NET_ERR_CANCELLED &&
                   probe.calls == 4U &&
                   net_global_context.counters.received_frames == 1U &&
                   net_global_context.counters.echo_replies == 0U &&
                   memcmp(&result, &sentinel, sizeof(result)) == 0,
                   "one-shot cancellation after frame decode is not lost");
}

static int test_one_shot_poll_and_arp_cancellation(void)
{
    unsigned char frame[NET_FRAME_MAX];
    struct cancellation_probe probe;
    struct net_mac_addr output = broadcast_mac;
    unsigned int length;

    if (!initialize()) {
        return 0;
    }
    length = make_echo_frame(frame, &local_mac, &gateway_mac,
                              &gateway_ip, &local_ip, 8U, 1U, 1U,
                              0, 0U);
    if (!require(phase_d_backend_queue_receive(frame, length) == 0,
                 "queue Echo Request before one-shot poll cancel")) {
        return 0;
    }
    probe.calls = 0U;
    probe.limit = 2U;
    net_set_cancel_callback(cancel_on_one_call, &probe);
    if (!require(net_poll(0U) == NET_ERR_CANCELLED &&
                 probe.calls == 2U &&
                 net_global_context.counters.received_frames == 1U &&
                 net_global_context.counters.echo_requests == 0U &&
                 phase_d_backend_transmit_count() == 0U &&
                 !net_global_context.service_active,
                 "one-shot cancellation reaches public polling result")) {
        return 0;
    }

    if (!initialize()) {
        return 0;
    }
    make_gateway_arp_reply(frame);
    if (!require(phase_d_backend_queue_receive(frame, NET_FRAME_MIN) == 0,
                 "queue ARP Reply before one-shot resolve cancel")) {
        return 0;
    }
    probe.calls = 0U;
    probe.limit = 3U;
    net_set_cancel_callback(cancel_on_one_call, &probe);
    return require(net_resolve_arp(&gateway_ip, &output, 100U) ==
                       NET_ERR_CANCELLED &&
                   probe.calls == 3U &&
                   net_global_context.counters.received_frames == 1U &&
                   phase_d_backend_transmit_count() == 1U &&
                   memcmp(output.octets, broadcast_mac.octets, 6U) == 0 &&
                   !net_global_context.pending_arp.active &&
                   !net_global_context.service_active,
                   "one-shot cancellation reaches public ARP result");
}

static int test_offlink_wrap_and_errors(void)
{
    struct net_ping_result result;
    struct net_ping_result sentinel;
    net_u16 identifier;

    if (!initialize()) {
        return 0;
    }
    memset(&result, 0xA5, sizeof(result));
    sentinel = result;
    if (!require(net_ping(0, 1U, 0, 0U, 1U, &result) ==
                     NET_ERR_INVALID &&
                 net_ping(&remote_ip, 0x10000U, 0, 0U, 1U,
                           &result) == NET_ERR_INVALID &&
                 net_ping(&remote_ip, 1U, 0,
                           NET_ICMP_ECHO_PAYLOAD_MAX + 1U,
                           1U, &result) == NET_ERR_INVALID &&
                 net_ping(&remote_ip, 1U, 0, 0U,
                           NET_TIMEOUT_MAX_MS + 1U,
                           &result) == NET_ERR_INVALID &&
                 net_ping(&local_ip, 1U, 0, 0U, 1U,
                           &result) == NET_ERR_NO_ROUTE &&
                 memcmp(&result, &sentinel, sizeof(result)) == 0,
                 "invalid input and no route preserve result")) {
        return 0;
    }
    phase_d_backend_clock_set(UINT_MAX - 5U);
    if (!require(net_arp_cache_store(&net_global_context,
                                     &gateway_ip, &gateway_mac) == 0,
                 "prime gateway across clock wrap")) {
        return 0;
    }
    identifier = (net_u16)(net_global_context.next_echo_identifier + 1U);
    if (!queue_echo(&remote_ip, identifier, 3U, "xy", 2U)) {
        return 0;
    }
    phase_d_backend_set_receive_clock_step(10U);
    if (!require(net_ping(&remote_ip, 3U, "xy", 2U,
                           20U, &result) == 0 &&
                 result.elapsed_ms == 10U &&
                 memcmp(result.source.octets,
                        remote_ip.octets, 4U) == 0 &&
                 memcmp(phase_d_backend_transmit_frame(0U),
                        gateway_mac.octets, 6U) == 0,
                 "off-link Ping uses gateway and accepts remote source")) {
        return 0;
    }
    if (!initialize() ||
        !require(net_arp_cache_store(&net_global_context,
                                     &gateway_ip, &gateway_mac) == 0,
                 "prime cache for errors")) {
        return 0;
    }
    memset(&result, 0xA5, sizeof(result));
    sentinel = result;
    phase_d_backend_set_cancelled(1);
    if (!require(net_ping(&gateway_ip, 1U, 0, 0U,
                           10U, &result) == NET_ERR_CANCELLED &&
                 phase_d_backend_transmit_count() == 0U &&
                 !net_global_context.service_active &&
                 memcmp(&result, &sentinel, sizeof(result)) == 0,
                 "cancellation leaves result and state unchanged")) {
        return 0;
    }
    phase_d_backend_set_cancelled(0);
    phase_d_backend_set_next_send_error(SYS_ERR_UNAVAILABLE);
    return require(net_ping(&gateway_ip, 1U, 0, 0U,
                             10U, &result) == NET_ERR_UNAVAILABLE &&
                   !net_global_context.pending_echo.active &&
                   !net_global_context.service_active &&
                   memcmp(&result, &sentinel, sizeof(result)) == 0,
                   "send error clears active Ping state");
}

int main(void)
{
    if (!test_ipv4_decode() || !test_icmp_decode_and_send() ||
        !test_echo_server() || !test_ping_match() ||
        !test_deadlines_and_zero() ||
        !test_arp_and_echo_share_deadline() ||
        !test_late_reply_and_wait_cancellation() ||
        !test_one_shot_poll_and_arp_cancellation() ||
        !test_offlink_wrap_and_errors()) {
        return 1;
    }
    puts("Phase D IPv4, ICMP, and Ping tests passed");
    return 0;
}
