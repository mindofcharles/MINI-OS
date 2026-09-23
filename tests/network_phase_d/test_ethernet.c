#include "deterministic_backend.h"

#include "../../transport/lib/net/ethernet.h"
#include "../../transport/lib/net/net_platform.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const unsigned char local_mac[6] = {
    0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
};
static const unsigned char peer_mac[6] = {
    0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
};
static const unsigned char broadcast_mac[6] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
};

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Phase D Ethernet test failed: %s\n", message);
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

static void make_receive_frame(const unsigned char *destination,
                               const unsigned char *source, net_u16 type)
{
    memset(net_global_context.rx_frame, 0xA5,
           sizeof(net_global_context.rx_frame));
    memcpy(net_global_context.rx_frame, destination, 6U);
    memcpy(net_global_context.rx_frame + 6U, source, 6U);
    net_global_context.rx_frame[12] = (unsigned char)(type >> 8);
    net_global_context.rx_frame[13] = (unsigned char)type;
}

static int rejected_unchanged(unsigned int length, int expected,
                              unsigned int malformed,
                              unsigned int unsupported,
                              const char *message)
{
    struct net_ethernet_view view;
    struct net_ethernet_view sentinel;
    struct net_context expected_context;

    memset(&view, 0xA5, sizeof(view));
    memcpy(&sentinel, &view, sizeof(sentinel));
    memcpy(&expected_context, &net_global_context,
           sizeof(expected_context));
    ++expected_context.counters.received_frames;
    expected_context.counters.malformed_frames += malformed;
    expected_context.counters.unsupported_frames += unsupported;

    return require(net_ethernet_decode(&net_global_context, length, &view) ==
                       expected, message) &&
           require(memcmp(&view, &sentinel, sizeof(view)) == 0,
                   "rejected view unchanged") &&
           require(memcmp(&net_global_context, &expected_context,
                          sizeof(expected_context)) == 0,
                   "rejection changes only expected receive counters");
}

static int test_lifecycle(void)
{
    struct net_ethernet_view view;
    struct net_ethernet_view sentinel;
    struct net_mac_addr peer;

    phase_d_backend_reset();
    memset(&net_global_context, 0, sizeof(net_global_context));
    memset(&view, 0xA5, sizeof(view));
    memcpy(&sentinel, &view, sizeof(sentinel));
    memcpy(peer.octets, peer_mac, sizeof(peer.octets));
    if (!require(net_ethernet_decode(0, 60U, &view) == NET_ERR_INVALID,
                 "null receive context") ||
        !require(net_ethernet_decode(&net_global_context, 60U, 0) ==
                     NET_ERR_INVALID, "null receive view") ||
        !require(net_ethernet_decode(&net_global_context, 60U, &view) ==
                     NET_ERR_STATE, "uninitialized receive") ||
        !require(memcmp(&view, &sentinel, sizeof(view)) == 0,
                 "uninitialized receive preserves output") ||
        !require(net_global_context.counters.received_frames == 0U,
                 "uninitialized receive does not count") ||
        !require(net_ethernet_begin_tx(0) == 0 &&
                     net_ethernet_begin_tx(&net_global_context) == 0,
                 "uninitialized transmit preparation") ||
        !require(net_ethernet_send(0, &peer, NET_ETHERTYPE_IPV4, 0U) ==
                     NET_ERR_INVALID, "null transmit context") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_IPV4, 0U) == NET_ERR_STATE,
                 "uninitialized transmit")) {
        return 0;
    }
    if (!initialize()) {
        return 0;
    }
    return require(net_ethernet_decode(&net_global_context, 0U, &view) ==
                       NET_ERR_INVALID,
                   "zero raw result is not a frame") &&
           require(net_global_context.counters.received_frames == 0U &&
                       net_global_context.counters.malformed_frames == 0U,
                   "zero raw result does not change counters") &&
           require(memcmp(&view, &sentinel, sizeof(view)) == 0,
                   "zero raw result preserves output");
}

static int test_receive_lengths_and_views(void)
{
    static const unsigned int lengths[] = {60U, 61U, 1514U};
    struct net_ethernet_view view;
    unsigned int index;

    make_receive_frame(local_mac, peer_mac, NET_ETHERTYPE_ARP);
    if (!rejected_unchanged(59U, NET_ETHERNET_DROP, 1U, 0U,
                            "reject truncated frame") ||
        !rejected_unchanged(1515U, NET_ETHERNET_DROP, 1U, 0U,
                            "reject oversized frame") ||
        !rejected_unchanged(UINT_MAX, NET_ETHERNET_DROP, 1U, 0U,
                            "reject wrapped length")) {
        return 0;
    }

    for (index = 0U; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
        net_u32 received_before = net_global_context.counters.received_frames;

        make_receive_frame(local_mac, peer_mac, NET_ETHERTYPE_ARP);
        if (!require(net_ethernet_decode(&net_global_context, lengths[index],
                                         &view) == NET_ETHERNET_ARP,
                     "ARP classification at frame boundary") ||
            !require(view.payload ==
                         net_global_context.rx_frame +
                             NET_ETHERNET_HEADER_SIZE &&
                         view.payload_length ==
                             lengths[index] - NET_ETHERNET_HEADER_SIZE,
                     "borrow full data field including padding") ||
            !require(view.payload[view.payload_length - 1U] == 0xA5U,
                     "preserve last data byte") ||
            !require(view.ethertype == NET_ETHERTYPE_ARP &&
                         memcmp(view.destination.octets, local_mac, 6U) == 0 &&
                         memcmp(view.source.octets, peer_mac, 6U) == 0,
                     "decoded header values") ||
            !require(net_global_context.counters.received_frames ==
                         received_before + 1U,
                     "accepted frame counted once")) {
            return 0;
        }
    }

    make_receive_frame(broadcast_mac, peer_mac, NET_ETHERTYPE_IPV4);
    if (!require(net_ethernet_decode(&net_global_context, 60U, &view) ==
                     NET_ETHERNET_IPV4,
                 "classify broadcast IPv4 destination") ||
        !require(memcmp(view.destination.octets, broadcast_mac, 6U) == 0 &&
                     view.ethertype == NET_ETHERTYPE_IPV4,
                 "broadcast view values")) {
        return 0;
    }
    return 1;
}

static int test_receive_rejections(void)
{
    static const unsigned char unrelated_mac[6] = {
        0x02U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU
    };
    static const unsigned char multicast_mac[6] = {
        0x01U, 0x00U, 0x5EU, 0x00U, 0x00U, 0x01U
    };
    static const unsigned char zero_mac[6] = {0};
    static const net_u16 unsupported_types[] = {
        0x05DCU, 0x05DDU, 0x0600U, 0x8100U, 0x86DDU, 0xFFFFU
    };
    unsigned int index;

    make_receive_frame(unrelated_mac, zero_mac, NET_ETHERTYPE_ARP);
    if (!rejected_unchanged(59U, NET_ETHERNET_DROP, 1U, 0U,
                            "length precedes destination and source") ||
        !rejected_unchanged(60U, NET_ETHERNET_DROP, 0U, 1U,
                            "destination precedes source validation")) {
        return 0;
    }
    make_receive_frame(multicast_mac, peer_mac, NET_ETHERTYPE_ARP);
    if (!rejected_unchanged(60U, NET_ETHERNET_DROP, 0U, 1U,
                            "reject multicast destination")) {
        return 0;
    }
    make_receive_frame(local_mac, zero_mac, NET_ETHERTYPE_ARP);
    if (!rejected_unchanged(60U, NET_ETHERNET_DROP, 1U, 0U,
                            "reject zero source")) {
        return 0;
    }
    make_receive_frame(local_mac, multicast_mac, NET_ETHERTYPE_ARP);
    if (!rejected_unchanged(60U, NET_ETHERNET_DROP, 1U, 0U,
                            "reject multicast source")) {
        return 0;
    }
    make_receive_frame(local_mac, broadcast_mac, NET_ETHERTYPE_ARP);
    if (!rejected_unchanged(60U, NET_ETHERNET_DROP, 1U, 0U,
                            "reject broadcast source")) {
        return 0;
    }
    for (index = 0U;
         index < sizeof(unsupported_types) / sizeof(unsupported_types[0]);
         ++index) {
        make_receive_frame(local_mac, peer_mac, unsupported_types[index]);
        if (!rejected_unchanged(60U, NET_ETHERNET_DROP, 0U, 1U,
                                "reject unsupported EtherType or length")) {
            return 0;
        }
    }
    return 1;
}

static int test_receive_backend(void)
{
    unsigned char frame[NET_FRAME_MIN];
    struct net_ethernet_view view;
    net_u32 before = net_global_context.counters.received_frames;
    int result;

    memset(frame, 0, sizeof(frame));
    memcpy(frame, local_mac, 6U);
    memcpy(frame + 6U, peer_mac, 6U);
    frame[12] = 0x08U;
    frame[13] = 0x06U;
    frame[14] = 0x42U;
    if (!require(phase_d_backend_queue_receive(frame, sizeof(frame)) == 0,
                 "queue raw frame")) {
        return 0;
    }
    result = net_recv_frame(net_global_context.rx_frame,
                            sizeof(net_global_context.rx_frame));
    return require(result == (int)sizeof(frame), "raw receive") &&
           require(net_ethernet_decode(&net_global_context,
                                       (unsigned int)result, &view) ==
                       NET_ETHERNET_ARP,
                   "classify raw received frame") &&
           require(view.payload[0] == 0x42U &&
                       view.payload_length == NET_FRAME_MIN - 14U,
                   "raw receive payload") &&
           require(net_global_context.counters.received_frames == before + 1U,
                   "raw receive counted once") &&
           require(phase_d_backend_queue_receive_error(SYS_ERR_DEVICE) == 0,
                   "queue raw receive error") &&
           require(net_recv_frame(net_global_context.rx_frame,
                                  sizeof(net_global_context.rx_frame)) ==
                       SYS_ERR_DEVICE,
                   "raw receive error") &&
           require(net_global_context.counters.received_frames == before + 1U,
                   "raw receive error is not a frame");
}

static int test_transmit_lengths(void)
{
    static const struct {
        unsigned int payload_length;
        unsigned int wire_length;
        net_u16 type;
        int broadcast;
    } cases[] = {
        {0U, 60U, NET_ETHERTYPE_ARP, 1},
        {1U, 60U, NET_ETHERTYPE_IPV4, 0},
        {45U, 60U, NET_ETHERTYPE_IPV4, 0},
        {46U, 60U, NET_ETHERTYPE_IPV4, 0},
        {1500U, 1514U, NET_ETHERTYPE_IPV4, 0}
    };
    struct net_mac_addr peer;
    struct net_mac_addr broadcast;
    const unsigned char *captured;
    const struct net_mac_addr *destination;
    unsigned char *payload;
    unsigned int index;
    unsigned int byte;

    memcpy(peer.octets, peer_mac, 6U);
    memcpy(broadcast.octets, broadcast_mac, 6U);
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        phase_d_backend_clear_transmits();
        memset(net_global_context.tx_frame, 0xA5,
               sizeof(net_global_context.tx_frame));
        payload = net_ethernet_begin_tx(&net_global_context);
        destination = cases[index].broadcast ? &broadcast : &peer;
        if (!require(payload == net_global_context.tx_frame + 14U,
                     "in-place payload pointer") ||
            !require(net_global_context.tx_frame[NET_FRAME_MAX - 1U] == 0U,
                     "begin clears reused frame")) {
            return 0;
        }
        for (byte = 0U; byte < cases[index].payload_length; ++byte) {
            payload[byte] = (unsigned char)(byte ^ 0x5AU);
        }
        if (!require(net_ethernet_send(&net_global_context, destination,
                                       cases[index].type,
                                       cases[index].payload_length) == 0,
                     "send logical payload") ||
            !require(phase_d_backend_transmit_count() == 1U &&
                         phase_d_backend_transmit_length(0U) ==
                             cases[index].wire_length,
                     "one exact-length raw send")) {
            return 0;
        }
        captured = phase_d_backend_transmit_frame(0U);
        if (!require(memcmp(captured, destination->octets, 6U) == 0 &&
                         memcmp(captured + 6U, local_mac, 6U) == 0,
                     "destination and initialized source MAC") ||
            !require(captured[12] ==
                         (unsigned char)(cases[index].type >> 8) &&
                         captured[13] == (unsigned char)cases[index].type,
                     "EtherType network byte order")) {
            return 0;
        }
        for (byte = 0U; byte < cases[index].payload_length; ++byte) {
            if (!require(captured[14U + byte] ==
                             (unsigned char)(byte ^ 0x5AU),
                         "payload survives header finalization")) {
                return 0;
            }
        }
        for (byte = 14U + cases[index].payload_length;
             byte < cases[index].wire_length; ++byte) {
            if (!require(captured[byte] == 0U, "zero Ethernet padding")) {
                return 0;
            }
        }
        if (!require(net_ethernet_send(&net_global_context, destination,
                                       cases[index].type,
                                       cases[index].payload_length) ==
                         NET_ERR_STATE &&
                         phase_d_backend_transmit_count() == 1U,
                     "prepared frame consumed once")) {
            return 0;
        }
    }
    return 1;
}

static int test_transmit_validation_and_errors(void)
{
    static const unsigned char multicast_mac[6] = {
        0x01U, 0x00U, 0x5EU, 0x00U, 0x00U, 0x01U
    };
    struct net_mac_addr peer;
    struct net_mac_addr broadcast;
    struct net_mac_addr multicast;
    struct net_mac_addr zero;

    memcpy(peer.octets, peer_mac, 6U);
    memcpy(broadcast.octets, broadcast_mac, 6U);
    memcpy(multicast.octets, multicast_mac, 6U);
    memset(&zero, 0, sizeof(zero));
    phase_d_backend_clear_transmits();
    net_ethernet_begin_tx(&net_global_context);
    if (!require(net_ethernet_send(&net_global_context, 0,
                                   NET_ETHERTYPE_ARP, 0U) == NET_ERR_INVALID,
                 "reject null destination") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, 1501U) == NET_ERR_INVALID,
                 "reject payload beyond MTU") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, UINT_MAX) ==
                     NET_ERR_INVALID,
                 "reject overflowing payload before length addition") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   0x8100U, 0U) == NET_ERR_INVALID,
                 "reject unsupported type") ||
        !require(net_ethernet_send(&net_global_context, &zero,
                                   NET_ETHERTYPE_ARP, 0U) == NET_ERR_INVALID,
                 "reject zero destination") ||
        !require(net_ethernet_send(&net_global_context, &multicast,
                                   NET_ETHERTYPE_ARP, 0U) == NET_ERR_INVALID,
                 "reject multicast destination") ||
        !require(net_ethernet_send(&net_global_context, &broadcast,
                                   NET_ETHERTYPE_IPV4, 0U) == NET_ERR_INVALID,
                 "reject IPv4 broadcast send") ||
        !require(phase_d_backend_transmit_count() == 0U,
                 "invalid sends do not reach raw backend") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, 0U) == 0,
                 "invalid argument does not consume preparation") ||
        !require(memcmp(phase_d_backend_transmit_frame(0U), peer.octets,
                        sizeof(peer.octets)) == 0,
                 "ARP supports unicast destination")) {
        return 0;
    }

    phase_d_backend_clear_transmits();
    net_ethernet_begin_tx(&net_global_context);
    if (!require(phase_d_backend_set_next_send_error(SYS_ERR_UNAVAILABLE) == 0,
                 "inject unavailable") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, 0U) ==
                     NET_ERR_UNAVAILABLE,
                 "map raw unavailable") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, 0U) == NET_ERR_STATE,
                 "failed send consumes preparation")) {
        return 0;
    }
    net_ethernet_begin_tx(&net_global_context);
    if (!require(phase_d_backend_set_next_send_error(SYS_ERR_TIMEOUT) == 0,
                 "inject raw timeout") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, 0U) == NET_ERR_DEVICE,
                 "raw timeout is device error")) {
        return 0;
    }
    net_ethernet_begin_tx(&net_global_context);
    if (!require(phase_d_backend_set_next_send_error(SYS_ERR_DEVICE) == 0,
                 "inject raw reset or failure") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, 0U) == NET_ERR_DEVICE,
                 "raw failure is device error")) {
        return 0;
    }
    net_ethernet_begin_tx(&net_global_context);
    if (!require(phase_d_backend_set_next_send_result(SYS_ERR_DEVICE) ==
                     SYS_ERR_INVALID,
                 "positive-result injection rejects negative errors") ||
        !require(phase_d_backend_set_next_send_result(0) == 0,
                 "inject zero send result") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, 0U) == NET_ERR_DEVICE,
                 "zero send result is device error")) {
        return 0;
    }
    net_ethernet_begin_tx(&net_global_context);
    if (!require(phase_d_backend_set_next_send_result(59) == 0,
                 "inject short send") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, 0U) == NET_ERR_DEVICE,
                 "short send is device error")) {
        return 0;
    }
    net_ethernet_begin_tx(&net_global_context);
    if (!require(phase_d_backend_set_next_send_result(61) == 0,
                 "inject oversized result") ||
        !require(net_ethernet_send(&net_global_context, &peer,
                                   NET_ETHERTYPE_ARP, 0U) == NET_ERR_DEVICE,
                 "overlong send is device error")) {
        return 0;
    }
    net_ethernet_begin_tx(&net_global_context);
    return require(net_ethernet_send(&net_global_context, &peer,
                                     NET_ETHERTYPE_ARP, 0U) == 0,
                   "injected send result is one-shot");
}

int main(void)
{
    if (!test_lifecycle() || !test_receive_lengths_and_views() ||
        !test_receive_rejections() || !test_receive_backend() ||
        !test_transmit_lengths() || !test_transmit_validation_and_errors()) {
        return 1;
    }
    puts("Phase D Ethernet tests passed.");
    return 0;
}
