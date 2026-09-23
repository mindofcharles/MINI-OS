#include "deterministic_backend.h"

#include "../../transport/lib/net/address.h"
#include "../../transport/lib/net/arp.h"
#include "../../transport/lib/net/byteorder.h"
#include "../../transport/lib/net/net_platform.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const struct net_ipv4_addr local_ip = {{10U, 0U, 2U, 15U}};
static const struct net_ipv4_addr peer_ip = {{10U, 0U, 2U, 2U}};
static const struct net_ipv4_addr other_ip = {{10U, 0U, 2U, 3U}};
static const struct net_ipv4_addr zero_ip = {{0U, 0U, 0U, 0U}};
static const struct net_mac_addr local_mac = {
    {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U}
};
static const struct net_mac_addr peer_mac = {
    {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U}
};
static const struct net_mac_addr other_mac = {
    {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U}
};
static const struct net_mac_addr zero_mac = {{0}};
static const struct net_mac_addr broadcast_mac = {
    {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU}
};

struct cancel_counter {
    unsigned int calls;
    unsigned int limit;
};

struct reentrant_probe {
    unsigned int calls;
    int poll_result;
    int resolve_result;
};

static int cancel_after_calls(void *context)
{
    struct cancel_counter *counter = context;

    ++counter->calls;
    return counter->calls >= counter->limit;
}

static int try_reentrant_calls(void *context)
{
    struct reentrant_probe *probe = context;

    ++probe->calls;
    if (probe->calls == 1U) {
        struct net_mac_addr output = other_mac;

        probe->poll_result = net_poll(0U);
        probe->resolve_result = net_resolve_arp(&peer_ip, &output, 0U);
    }
    return 0;
}

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Phase D ARP test failed: %s\n", message);
        return 0;
    }
    return 1;
}

static struct net_config valid_config(void)
{
    struct net_config config = {
        {{10U, 0U, 2U, 15U}},
        {{255U, 255U, 255U, 0U}},
        {{10U, 0U, 2U, 2U}},
        NET_ARP_CACHE_TTL_DEFAULT_MS,
        NET_ARP_RETRY_INTERVAL_MIN_MS,
        NET_ARP_RETRY_COUNT_DEFAULT
    };

    return config;
}

static int initialize(void)
{
    struct net_config config = valid_config();

    phase_d_backend_reset();
    memset(&net_global_context, 0, sizeof(net_global_context));
    return require(net_init(&config) == 0, "initialize stack");
}

static void make_arp_frame(unsigned char *frame,
                           const struct net_mac_addr *ethernet_destination,
                           const struct net_mac_addr *ethernet_source,
                           unsigned int operation,
                           const struct net_mac_addr *arp_source,
                           const struct net_ipv4_addr *source_ip,
                           const struct net_mac_addr *arp_target,
                           const struct net_ipv4_addr *target_ip)
{
    unsigned char *arp = frame + NET_ETHERNET_HEADER_SIZE;

    memset(frame, 0xA5, NET_FRAME_MIN);
    memcpy(frame, ethernet_destination->octets, 6U);
    memcpy(frame + 6U, ethernet_source->octets, 6U);
    net_write_be16(frame + 12U, NET_ETHERTYPE_ARP);
    net_write_be16(arp, 1U);
    net_write_be16(arp + 2U, NET_ETHERTYPE_IPV4);
    arp[4] = 6U;
    arp[5] = 4U;
    net_write_be16(arp + 6U, (net_u16)operation);
    memcpy(arp + 8U, arp_source->octets, 6U);
    memcpy(arp + 14U, source_ip->octets, 4U);
    memcpy(arp + 18U, arp_target->octets, 6U);
    memcpy(arp + 24U, target_ip->octets, 4U);
}

static int queue_frame(const unsigned char *frame)
{
    return require(phase_d_backend_queue_receive(frame, NET_FRAME_MIN) == 0,
                   "queue frame");
}

static int mac_equal(const struct net_mac_addr *left,
                     const struct net_mac_addr *right)
{
    return memcmp(left->octets, right->octets, 6U) == 0;
}

static int test_parse_boundaries(void)
{
    unsigned char frame[NET_FRAME_MIN];
    struct net_ethernet_view view;
    struct net_arp_packet packet;
    struct net_arp_packet sentinel;
    unsigned int length;

    if (!initialize()) {
        return 0;
    }
    make_arp_frame(frame, &local_mac, &peer_mac, NET_ARP_REQUEST,
                   &peer_mac, &peer_ip, &broadcast_mac, &local_ip);
    memset(&view, 0, sizeof(view));
    view.source = peer_mac;
    view.payload = frame + NET_ETHERNET_HEADER_SIZE;
    for (length = 0U; length < NET_ARP_PACKET_SIZE; ++length) {
        memset(&packet, 0xA5, sizeof(packet));
        sentinel = packet;
        view.payload_length = length;
        if (!require(net_arp_parse(&view, &packet) == NET_ARP_MALFORMED,
                     "reject each truncated ARP length") ||
            !require(memcmp(&packet, &sentinel, sizeof(packet)) == 0,
                     "truncated parser output unchanged")) {
            return 0;
        }
    }
    view.payload_length = NET_FRAME_MIN - NET_ETHERNET_HEADER_SIZE;
    if (!require(net_arp_parse(&view, &packet) == NET_ARP_REQUEST,
                 "parse request despite nonzero padding") ||
        !require(mac_equal(&packet.sender_mac, &peer_mac) &&
                     memcmp(packet.sender_ip.octets, peer_ip.octets, 4U) == 0,
                 "parsed sender fields") ||
        !require(net_arp_parse(0, &packet) == NET_ERR_INVALID &&
                     net_arp_parse(&view, 0) == NET_ERR_INVALID,
                 "parser rejects null arguments")) {
        return 0;
    }
    frame[15] = 2U;
    if (!require(net_arp_parse(&view, &packet) == NET_ARP_MALFORMED,
                 "reject hardware type")) {
        return 0;
    }
    frame[15] = 1U;
    frame[16] = 0U;
    if (!require(net_arp_parse(&view, &packet) == NET_ARP_MALFORMED,
                 "reject protocol type")) {
        return 0;
    }
    frame[16] = 8U;
    frame[18] = 5U;
    if (!require(net_arp_parse(&view, &packet) == NET_ARP_MALFORMED,
                 "reject hardware length")) {
        return 0;
    }
    frame[18] = 6U;
    frame[19] = 5U;
    if (!require(net_arp_parse(&view, &packet) == NET_ARP_MALFORMED,
                 "reject protocol length")) {
        return 0;
    }
    frame[19] = 4U;
    frame[21] = 3U;
    if (!require(net_arp_parse(&view, &packet) == NET_ARP_UNSUPPORTED,
                 "reject unsupported operation")) {
        return 0;
    }
    frame[21] = 1U;
    frame[22] ^= 1U;
    return require(net_arp_parse(&view, &packet) == NET_ARP_MALFORMED,
                   "reject Ethernet/ARP sender mismatch");
}

static int test_request_and_probe(void)
{
    unsigned char frame[NET_FRAME_MIN];
    unsigned char expected[NET_FRAME_MIN];
    const unsigned char *sent;
    struct net_mac_addr found;
    unsigned int index;

    if (!initialize()) {
        return 0;
    }
    make_arp_frame(frame, &broadcast_mac, &peer_mac, NET_ARP_REQUEST,
                   &peer_mac, &peer_ip, &broadcast_mac, &local_ip);
    if (!queue_frame(frame) ||
        !require(net_poll(0U) == 1, "reply to request for local address") ||
        !require(phase_d_backend_transmit_count() == 1U,
                 "one ARP reply") ||
        !require(net_arp_cache_lookup(&net_global_context, &peer_ip,
                                      &found) == 1 &&
                     mac_equal(&found, &peer_mac),
                 "learn request sender") ||
        !require(net_global_context.counters.arp_cache_updates == 1U,
                 "one cache update")) {
        return 0;
    }
    sent = phase_d_backend_transmit_frame(0U);
    make_arp_frame(expected, &peer_mac, &local_mac, NET_ARP_REPLY,
                   &local_mac, &local_ip, &peer_mac, &peer_ip);
    memset(expected + 42U, 0, NET_FRAME_MIN - 42U);
    if (!require(phase_d_backend_transmit_length(0U) == NET_FRAME_MIN,
                 "reply minimum frame length") ||
        !require(memcmp(sent, expected, NET_FRAME_MIN) == 0,
                 "complete ARP reply bytes and padding") ||
        !require(memcmp(sent, peer_mac.octets, 6U) == 0 &&
                     memcmp(sent + 6U, local_mac.octets, 6U) == 0,
                 "reply Ethernet addresses") ||
        !require(net_read_be16(sent + 12U) == NET_ETHERTYPE_ARP &&
                     net_read_be16(sent + 20U) == NET_ARP_REPLY,
                 "reply types") ||
        !require(memcmp(sent + 22U, local_mac.octets, 6U) == 0 &&
                     memcmp(sent + 28U, local_ip.octets, 4U) == 0 &&
                     memcmp(sent + 32U, peer_mac.octets, 6U) == 0 &&
                     memcmp(sent + 38U, peer_ip.octets, 4U) == 0,
                 "exact reply payload")) {
        return 0;
    }
    for (index = 42U; index < NET_FRAME_MIN; ++index) {
        if (!require(sent[index] == 0U, "zero reply padding")) {
            return 0;
        }
    }

    if (!initialize()) {
        return 0;
    }
    make_arp_frame(frame, &broadcast_mac, &peer_mac, NET_ARP_REQUEST,
                   &peer_mac, &zero_ip, &other_mac, &local_ip);
    if (!queue_frame(frame) ||
        !require(net_poll(0U) == 1, "reply to ARP probe") ||
        !require(net_global_context.counters.arp_cache_updates == 0U,
                 "probe does not create zero-IP entry") ||
        !require(memcmp(phase_d_backend_transmit_frame(0U) + 38U,
                        zero_ip.octets, 4U) == 0,
                 "probe reply targets zero IP")) {
        return 0;
    }
    make_arp_frame(frame, &local_mac, &peer_mac, NET_ARP_REQUEST,
                   &peer_mac, &peer_ip, &other_mac, &other_ip);
    return queue_frame(frame) &&
           require(net_poll(0U) == 0,
                   "ignore request for unrelated address") &&
           require(phase_d_backend_transmit_count() == 1U,
                   "unrelated request causes no reply");
}

static int test_cache_and_routes(void)
{
    struct net_config config = valid_config();
    struct net_ipv4_addr peers[5];
    struct net_ipv4_addr next_hop;
    struct net_ipv4_addr offlink = {{10U, 0U, 3U, 7U}};
    struct net_ipv4_addr local_broadcast = {{10U, 0U, 2U, 255U}};
    struct net_ipv4_addr multicast = {{224U, 0U, 0U, 1U}};
    struct net_mac_addr found;
    unsigned int index;

    if (!initialize()) {
        return 0;
    }
    for (index = 0U; index < 5U; ++index) {
        peers[index] = peer_ip;
        peers[index].octets[3] = (unsigned char)(20U + index);
    }
    phase_d_backend_clock_set(UINT_MAX - 200U);
    for (index = 0U; index < 4U; ++index) {
        if (!require(net_arp_cache_store(&net_global_context,
                                         &peers[index],
                                         &peer_mac) == 0,
                     "fill cache") ||
            !require(net_global_context.arp_cache[index].valid,
                     "fill successive slot")) {
            return 0;
        }
        phase_d_backend_clock_advance(10U);
    }
    if (!require(net_arp_cache_lookup(&net_global_context, &peers[0],
                                      &found) == 1,
                 "cache hit before expiry") ||
        !require(net_global_context.arp_cache[0].updated_ms ==
                     UINT_MAX - 200U,
                 "lookup does not extend entry lifetime") ||
        !require(net_arp_cache_store(&net_global_context, &peers[4],
                                     &other_mac) == 0,
                 "replace oldest full-cache entry") ||
        !require(net_arp_cache_lookup(&net_global_context, &peers[0],
                                      &found) == 0 &&
                     net_arp_cache_lookup(&net_global_context,
                                           &peers[4], &found) == 1 &&
                     mac_equal(&found, &other_mac),
                 "oldest evicted, new entry retained") ||
        !require(net_arp_cache_store(&net_global_context, &peers[4],
                                     &peer_mac) == 0 &&
                     net_arp_cache_lookup(&net_global_context,
                                           &peers[4], &found) == 1 &&
                     mac_equal(&found, &peer_mac),
                 "existing key updated")) {
        return 0;
    }
    phase_d_backend_clock_advance(NET_ARP_CACHE_TTL_DEFAULT_MS);
    found = other_mac;
    if (!require(net_arp_cache_lookup(&net_global_context, &peers[4],
                                      &found) == 0 &&
                     mac_equal(&found, &other_mac),
                 "wrap-safe expiry preserves output") ||
        !require(net_arp_cache_store(&net_global_context, &peers[0],
                                     &peer_mac) == 0 &&
                     net_global_context.arp_cache[0].valid &&
                     memcmp(net_global_context.arp_cache[0].address.octets,
                            peers[0].octets, 4U) == 0,
                 "expired slot reused first") ||
        !require(net_arp_cache_store(&net_global_context, &local_ip,
                                     &peer_mac) == NET_ERR_INVALID,
                 "do not cache claim to local IP") ||
        !require(net_arp_cache_store(&net_global_context, &offlink,
                                     &peer_mac) == NET_ERR_INVALID,
                 "do not cache offlink sender") ||
        !require(net_ipv4_select_next_hop(&config, &peer_ip,
                                          &next_hop) == 0 &&
                     memcmp(next_hop.octets, peer_ip.octets, 4U) == 0,
                 "onlink direct next hop") ||
        !require(net_ipv4_select_next_hop(&config, &offlink,
                                          &next_hop) == 0 &&
                     memcmp(next_hop.octets,
                            config.gateway.octets, 4U) == 0,
                 "offlink gateway next hop") ||
        !require(net_ipv4_select_next_hop(&config, &local_broadcast,
                                          &next_hop) == NET_ERR_NO_ROUTE &&
                     net_ipv4_select_next_hop(&config, &multicast,
                                               &next_hop) == NET_ERR_NO_ROUTE &&
                     net_ipv4_select_next_hop(&config, &local_ip,
                                               &next_hop) == NET_ERR_NO_ROUTE,
                 "reject non-unicast and local destinations")) {
        return 0;
    }
    return require(net_ipv4_select_next_hop(0, &peer_ip,
                                             &next_hop) == NET_ERR_INVALID,
                   "route helper validates input");
}

static int test_resolution(void)
{
    unsigned char reply[NET_FRAME_MIN];
    unsigned char expected[NET_FRAME_MIN];
    const unsigned char *sent;
    struct net_mac_addr found;
    struct net_mac_addr sentinel = other_mac;

    if (!initialize()) {
        return 0;
    }
    make_arp_frame(reply, &broadcast_mac, &peer_mac, NET_ARP_REPLY,
                   &peer_mac, &peer_ip, &local_mac, &local_ip);
    if (!queue_frame(reply) ||
        !require(net_resolve_arp(&peer_ip, &found, 0U) == 0,
                 "matching broadcast reply resolves in zero-time pass") ||
        !require(mac_equal(&found, &peer_mac), "resolved MAC") ||
        !require(phase_d_backend_transmit_count() == 1U,
                 "resolution sent one request")) {
        return 0;
    }
    sent = phase_d_backend_transmit_frame(0U);
    make_arp_frame(expected, &broadcast_mac, &local_mac,
                   NET_ARP_REQUEST, &local_mac, &local_ip,
                   &zero_mac, &peer_ip);
    memset(expected + 42U, 0, NET_FRAME_MIN - 42U);
    if (!require(phase_d_backend_transmit_length(0U) == NET_FRAME_MIN &&
                     memcmp(sent, expected, NET_FRAME_MIN) == 0,
                 "complete ARP request bytes and padding") ||
        !require(phase_d_backend_transmit_length(0U) == NET_FRAME_MIN &&
                     memcmp(sent, broadcast_mac.octets, 6U) == 0 &&
                     net_read_be16(sent + 20U) == NET_ARP_REQUEST,
                 "exact request header") ||
        !require(memcmp(sent + 22U, local_mac.octets, 6U) == 0 &&
                     memcmp(sent + 28U, local_ip.octets, 4U) == 0 &&
                     memcmp(sent + 32U, "\0\0\0\0\0\0", 6U) == 0 &&
                     memcmp(sent + 38U, peer_ip.octets, 4U) == 0,
                 "exact request addresses") ||
        !require(net_resolve_arp(&peer_ip, &found, 0U) == 0 &&
                     phase_d_backend_transmit_count() == 1U,
                 "cache hit avoids request")) {
        return 0;
    }
    phase_d_backend_clock_advance(NET_ARP_CACHE_TTL_DEFAULT_MS);
    found = other_mac;
    if (!require(net_resolve_arp(&peer_ip, &found, 0U) ==
                     NET_ERR_TIMEOUT &&
                     mac_equal(&found, &other_mac) &&
                     phase_d_backend_transmit_count() == 2U,
                 "expired cache entry requires another request")) {
        return 0;
    }

    if (!initialize()) {
        return 0;
    }
    make_arp_frame(reply, &local_mac, &peer_mac, NET_ARP_REPLY,
                   &peer_mac, &other_ip, &local_mac, &local_ip);
    if (!queue_frame(reply) ||
        !require(net_resolve_arp(&peer_ip, &sentinel, 0U) ==
                     NET_ERR_TIMEOUT && mac_equal(&sentinel, &other_mac),
                 "reply for another sender does not resolve") ||
        !require(net_global_context.counters.arp_cache_updates == 0U,
                 "unmatched reply does not cache")) {
        return 0;
    }
    return require(net_global_context.pending_arp.active == 0,
                   "pending operation cleared after timeout");
}

static int test_cache_expired_key_priority(void)
{
    struct net_ipv4_addr first = {{10U, 0U, 2U, 20U}};
    struct net_ipv4_addr existing = {{10U, 0U, 2U, 21U}};
    struct net_ipv4_addr peer;
    unsigned int index;

    if (!initialize()) {
        return 0;
    }
    if (!require(net_arp_cache_store(&net_global_context, &first,
                                     &peer_mac) == 0 &&
                     net_arp_cache_store(&net_global_context, &existing,
                                          &peer_mac) == 0,
                 "seed cache key replacement")) {
        return 0;
    }
    net_global_context.arp_cache[0].valid = 0;
    phase_d_backend_clock_advance(NET_ARP_CACHE_TTL_DEFAULT_MS);
    if (!require(net_arp_cache_store(&net_global_context, &existing,
                                     &other_mac) == 0 &&
                     !net_global_context.arp_cache[0].valid &&
                     net_global_context.arp_cache[1].valid &&
                     mac_equal(&net_global_context.arp_cache[1].mac,
                               &other_mac),
                 "expired same key precedes an earlier vacant slot")) {
        return 0;
    }
    if (!initialize()) {
        return 0;
    }
    for (index = 0U; index < NET_ARP_CACHE_CAPACITY; ++index) {
        peer = first;
        peer.octets[3] += (unsigned char)index;
        if (!require(net_arp_cache_store(&net_global_context,
                                         &peer, &peer_mac) == 0,
                     "fill cache with equal timestamps")) {
            return 0;
        }
    }
    peer.octets[3] = 30U;
    return require(net_arp_cache_store(&net_global_context,
                                        &peer, &other_mac) == 0 &&
                       memcmp(net_global_context.arp_cache[0].address.octets,
                              peer.octets, 4U) == 0,
                   "equal-age replacement chooses first slot");
}

static int test_resolution_rejections(void)
{
    unsigned char frame[NET_FRAME_MIN];
    struct net_mac_addr output;
    unsigned int variant;

    for (variant = 0U; variant < 6U; ++variant) {
        if (!initialize()) {
            return 0;
        }
        make_arp_frame(frame, &local_mac, &peer_mac, NET_ARP_REPLY,
                       &peer_mac, &peer_ip, &local_mac, &local_ip);
        if (variant == 0U) {
            memcpy(frame + 38U, other_ip.octets, 4U);
        } else if (variant == 1U) {
            memcpy(frame + 32U, other_mac.octets, 6U);
        } else if (variant == 2U) {
            memcpy(frame + 22U, other_mac.octets, 6U);
        } else if (variant == 3U) {
            memcpy(frame + 28U, local_ip.octets, 4U);
        } else if (variant == 4U) {
            frame[22U] = 1U;
        } else {
            frame[18U] = 5U;
        }
        output = other_mac;
        if (!queue_frame(frame) ||
            !require(net_resolve_arp(&peer_ip, &output, 0U) ==
                         NET_ERR_TIMEOUT &&
                         mac_equal(&output, &other_mac),
                     "mismatched or malformed reply cannot resolve") ||
            !require(net_global_context.counters.arp_cache_updates == 0U,
                     "rejected reply cannot update cache")) {
            return 0;
        }
    }

    if (!initialize()) {
        return 0;
    }
    make_arp_frame(frame, &broadcast_mac, &peer_mac, NET_ARP_REQUEST,
                   &peer_mac, &peer_ip, &other_mac, &local_ip);
    output = other_mac;
    return queue_frame(frame) &&
           require(net_resolve_arp(&peer_ip, &output, 0U) ==
                       NET_ERR_TIMEOUT && mac_equal(&output, &other_mac),
                   "request learning cannot complete active resolution") &&
           require(net_global_context.counters.arp_cache_updates == 1U &&
                       phase_d_backend_transmit_count() == 2U,
                   "request learned and answered without resolving");
}

static int test_timing_and_errors(void)
{
    struct net_mac_addr output = other_mac;
    struct net_ipv4_addr offlink = {{10U, 0U, 3U, 7U}};
    struct cancel_counter counter;

    if (!initialize()) {
        return 0;
    }
    phase_d_backend_set_receive_clock_step(250U);
    if (!require(net_resolve_arp(&peer_ip, &output, 3500U) ==
                     NET_ERR_TIMEOUT,
                 "three attempts then timeout") ||
        !require(mac_equal(&output, &other_mac),
                 "timeout preserves output") ||
        !require(phase_d_backend_transmit_count() == 3U &&
                     phase_d_backend_transmit_time(0U) == 0U &&
                     phase_d_backend_transmit_time(1U) == 1000U &&
                     phase_d_backend_transmit_time(2U) == 2000U,
                 "exact retry spacing and count") ||
        !require(net_global_context.pending_arp.active == 0,
                 "pending cleared after retries")) {
        return 0;
    }

    if (!initialize()) {
        return 0;
    }
    if (!require(net_resolve_arp(&peer_ip, &output, 0U) ==
                     NET_ERR_TIMEOUT &&
                     phase_d_backend_transmit_count() == 1U,
                 "zero-time call transmits once") ||
        !require(net_resolve_arp(&other_ip, &output, 0U) ==
                     NET_ERR_TIMEOUT &&
                     phase_d_backend_transmit_count() == 1U,
                 "cross-destination request throttle")) {
        return 0;
    }
    phase_d_backend_clock_advance(999U);
    if (!require(net_resolve_arp(&other_ip, &output, 0U) ==
                     NET_ERR_TIMEOUT &&
                     phase_d_backend_transmit_count() == 1U,
                 "throttle before one second")) {
        return 0;
    }
    phase_d_backend_clock_advance(1U);
    if (!require(net_resolve_arp(&other_ip, &output, 0U) ==
                     NET_ERR_TIMEOUT &&
                     phase_d_backend_transmit_count() == 2U &&
                     phase_d_backend_transmit_time(1U) == 1000U,
                 "throttle opens at one second")) {
        return 0;
    }
    if (!require(net_resolve_arp(&offlink, &output, 0U) ==
                     NET_ERR_NO_ROUTE && mac_equal(&output, &other_mac),
                 "offlink address does not enter ARP") ||
        !require(net_resolve_arp(0, &output, 0U) == NET_ERR_INVALID &&
                     net_resolve_arp(&peer_ip, 0, 0U) == NET_ERR_INVALID &&
                     net_resolve_arp(&peer_ip, &output,
                                     (unsigned int)NET_TIMEOUT_MAX_MS + 1U) ==
                         NET_ERR_INVALID,
                 "resolver arguments and duration") ||
        !require(net_poll((unsigned int)NET_TIMEOUT_MAX_MS + 1U) ==
                     NET_ERR_INVALID,
                 "poll duration")) {
        return 0;
    }

    if (!initialize()) {
        return 0;
    }
    phase_d_backend_set_cancelled(1);
    if (!require(net_resolve_arp(&peer_ip, &output, 1000U) ==
                     NET_ERR_CANCELLED &&
                     phase_d_backend_transmit_count() == 0U &&
                     mac_equal(&output, &other_mac),
                 "cancellation before transmit") ||
        !require(net_poll(0U) == NET_ERR_CANCELLED,
                 "poll cancellation")) {
        return 0;
    }
    phase_d_backend_set_cancelled(0);
    counter.calls = 0U;
    counter.limit = 3U;
    if (!require(net_set_cancel_callback(cancel_after_calls,
                                          &counter) == 0 &&
                     net_resolve_arp(&peer_ip, &output, 1000U) ==
                         NET_ERR_CANCELLED &&
                     phase_d_backend_transmit_count() == 1U &&
                     mac_equal(&output, &other_mac),
                 "callback cancellation after one request")) {
        return 0;
    }
    if (!require(net_set_cancel_callback(0, 0) == 0,
                 "clear cancellation callback")) {
        return 0;
    }
    net_global_context.pending_arp.active = 1;
    if (!require(net_resolve_arp(&peer_ip, &output, 0U) == NET_ERR_BUSY &&
                     net_poll(0U) == NET_ERR_BUSY,
                 "active synchronous operation rejects competing calls")) {
        return 0;
    }
    net_global_context.pending_arp.active = 0;
    if (!require(phase_d_backend_queue_receive_error(SYS_ERR_UNAVAILABLE) == 0 &&
                     net_poll(0U) == NET_ERR_UNAVAILABLE &&
                     net_global_context.counters.received_frames == 0U,
                 "unavailable receive does not count frame") ||
        !require(phase_d_backend_queue_receive_error(SYS_ERR_DEVICE) == 0 &&
                     net_poll(0U) == NET_ERR_DEVICE &&
                     net_global_context.counters.received_frames == 0U,
                 "device receive error does not count frame")) {
        return 0;
    }
    return 1;
}

static int test_device_and_wrap(void)
{
    struct net_mac_addr output = other_mac;

    if (!initialize()) {
        return 0;
    }
    if (!require(phase_d_backend_set_next_send_error(SYS_ERR_UNAVAILABLE) == 0 &&
                     net_resolve_arp(&peer_ip, &output, 0U) ==
                         NET_ERR_UNAVAILABLE &&
                     phase_d_backend_transmit_count() == 0U &&
                     net_global_context.pending_arp.active == 0 &&
                     mac_equal(&output, &other_mac),
                 "unavailable transmit preserves output and clears pending")) {
        return 0;
    }
    if (!initialize()) {
        return 0;
    }
    if (!require(phase_d_backend_set_next_send_result(59) == 0 &&
                     net_resolve_arp(&peer_ip, &output, 0U) ==
                         NET_ERR_DEVICE &&
                     net_global_context.pending_arp.active == 0 &&
                     mac_equal(&output, &other_mac),
                 "short transmit is device failure")) {
        return 0;
    }
    if (!initialize()) {
        return 0;
    }
    if (!require(phase_d_backend_queue_receive_error(SYS_ERR_DEVICE) == 0 &&
                     net_resolve_arp(&peer_ip, &output, 1000U) ==
                         NET_ERR_DEVICE &&
                     net_global_context.counters.received_frames == 0U &&
                     net_global_context.pending_arp.active == 0,
                 "resolver receive error clears pending without counting")) {
        return 0;
    }
    if (!initialize()) {
        return 0;
    }
    phase_d_backend_clock_set(UINT_MAX - 499U);
    phase_d_backend_set_receive_clock_step(250U);
    return require(net_resolve_arp(&peer_ip, &output, 3500U) ==
                       NET_ERR_TIMEOUT &&
                       phase_d_backend_transmit_count() == 3U,
                   "wrap-safe resolution deadline and retries") &&
           require(phase_d_backend_transmit_time(0U) == UINT_MAX - 499U &&
                       phase_d_backend_transmit_time(1U) == 500U &&
                       phase_d_backend_transmit_time(2U) == 1500U,
                   "wrap-safe retry timestamps");
}

static int test_rejections_and_deadline(void)
{
    unsigned char frame[NET_FRAME_MIN];
    struct net_mac_addr output = other_mac;

    if (!initialize()) {
        return 0;
    }
    make_arp_frame(frame, &local_mac, &peer_mac, NET_ARP_REPLY,
                   &peer_mac, &peer_ip, &local_mac, &local_ip);
    if (!queue_frame(frame) ||
        !require(net_poll(0U) == 0 &&
                     net_global_context.counters.arp_cache_updates == 0U,
                 "unsolicited reply ignored")) {
        return 0;
    }
    make_arp_frame(frame, &local_mac, &peer_mac, NET_ARP_REQUEST,
                   &peer_mac, &local_ip, &local_mac, &local_ip);
    if (!queue_frame(frame) ||
        !require(net_poll(0U) == 0 &&
                     phase_d_backend_transmit_count() == 0U,
                 "foreign claim for local IP ignored")) {
        return 0;
    }
    make_arp_frame(frame, &local_mac, &peer_mac, NET_ARP_REQUEST,
                   &peer_mac, &peer_ip, &broadcast_mac, &local_ip);
    frame[18] = 5U;
    if (!queue_frame(frame) ||
        !require(net_poll(0U) == 0 &&
                     net_global_context.counters.malformed_frames == 1U &&
                     phase_d_backend_transmit_count() == 0U,
                 "malformed ARP causes no reply or cache write")) {
        return 0;
    }
    make_arp_frame(frame, &local_mac, &peer_mac, NET_ARP_REQUEST,
                   &peer_mac, &peer_ip, &broadcast_mac, &local_ip);
    frame[21] = 3U;
    if (!queue_frame(frame) ||
        !require(net_poll(0U) == 0 &&
                     net_global_context.counters.unsupported_frames == 1U,
                 "unsupported ARP operation counted")) {
        return 0;
    }

    if (!initialize()) {
        return 0;
    }
    make_arp_frame(frame, &local_mac, &peer_mac, NET_ARP_REPLY,
                   &peer_mac, &peer_ip, &local_mac, &local_ip);
    phase_d_backend_set_receive_clock_step(5U);
    return queue_frame(frame) &&
           require(net_resolve_arp(&peer_ip, &output, 4U) ==
                       NET_ERR_TIMEOUT && mac_equal(&output, &other_mac),
                   "late reply cannot complete expired deadline") &&
           require(net_global_context.counters.received_frames == 1U,
                   "late positive raw receive counted once") &&
           require(net_global_context.counters.arp_cache_updates == 0U,
                   "late reply cannot update cache");
}

static int test_poll_idle_and_ipv4(void)
{
    unsigned char frame[NET_FRAME_MIN];

    if (!initialize()) {
        return 0;
    }
    memset(frame, 0, sizeof(frame));
    memcpy(frame, local_mac.octets, 6U);
    memcpy(frame + 6U, peer_mac.octets, 6U);
    net_write_be16(frame + 12U, NET_ETHERTYPE_IPV4);
    if (!queue_frame(frame) ||
        !require(net_poll(0U) == 0 &&
                     net_global_context.counters.unsupported_frames == 1U,
                 "D4 consumes IPv4 without claiming handler work") ||
        !require(net_poll(0U) == 0, "zero-time idle poll")) {
        return 0;
    }
    phase_d_backend_set_receive_clock_step(100U);
    if (!require(net_poll(500U) == 0 && net_clock_now_ms() == 500U,
                 "positive-duration idle poll ends at deadline")) {
        return 0;
    }
    phase_d_backend_clock_set(UINT_MAX - 199U);
    return require(net_poll(500U) == 0 && net_clock_now_ms() == 300U,
                   "wrap-safe poll deadline");
}

static int test_reentrant_guard(void)
{
    struct reentrant_probe probe;
    struct net_mac_addr output = other_mac;

    if (!initialize()) {
        return 0;
    }
    memset(&probe, 0, sizeof(probe));
    if (!require(net_set_cancel_callback(try_reentrant_calls,
                                          &probe) == 0 &&
                     net_poll(0U) == 0 &&
                     probe.poll_result == NET_ERR_BUSY &&
                     probe.resolve_result == NET_ERR_BUSY,
                 "poll callback cannot reenter network service")) {
        return 0;
    }
    if (!initialize()) {
        return 0;
    }
    memset(&probe, 0, sizeof(probe));
    return require(net_set_cancel_callback(try_reentrant_calls,
                                            &probe) == 0 &&
                       net_resolve_arp(&peer_ip, &output, 0U) ==
                           NET_ERR_TIMEOUT &&
                       probe.poll_result == NET_ERR_BUSY &&
                       probe.resolve_result == NET_ERR_BUSY,
                   "resolver callback cannot reenter network service");
}

int main(void)
{
    if (!test_parse_boundaries() || !test_request_and_probe() ||
        !test_cache_and_routes() || !test_cache_expired_key_priority() ||
        !test_resolution() ||
        !test_resolution_rejections() || !test_timing_and_errors() ||
        !test_device_and_wrap() || !test_rejections_and_deadline() ||
        !test_poll_idle_and_ipv4() || !test_reentrant_guard()) {
        return 1;
    }
    puts("Phase D ARP and polling tests passed");
    return 0;
}
