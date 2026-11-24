#include "../network_common/deterministic_backend.h"

#include "../../transport/lib/net/arp.h"
#include "../../transport/lib/net/net_platform.h"
#include "../../transport/lib/net/tcp/internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "TCP stream test failure at line %d: %s\n", \
            __LINE__, #condition); return 0; } } while (0)

enum { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16 };

static const struct net_config network = {
    {{10U, 0U, 2U, 15U}}, {{255U, 255U, 255U, 0U}}, {{10U, 0U, 2U, 2U}},
    60000U, 1000U, 2U
};
static const struct tcp_endpoint remote = {{{192U, 0U, 2U, 9U}}, 22U};
static const unsigned char local_mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
static const struct net_mac_addr gateway = {{2U, 0U, 0U, 0U, 0U, 1U}};
static const struct net_mac_addr refreshed = {{2U, 0U, 0U, 0U, 0U, 2U}};
static const struct net_mac_addr incoming = {{2U, 0U, 0U, 0U, 0U, 3U}};

struct peer {
    tcp_handle handle;
    unsigned int port;
    net_u32 first_send;
    net_u32 first_receive;
};

struct wire_segment {
    net_u32 seq, ack;
    unsigned int window, flags, mss;
    const unsigned char *data;
    unsigned int length;
};

/* An independent wire oracle, with no production encoder or decoder. */
static void word(unsigned char *data, unsigned int value)
{
    data[0] = (unsigned char)(value >> 8U);
    data[1] = (unsigned char)value;
}

static unsigned int read_word(const unsigned char *data)
{
    return ((unsigned int)data[0] << 8U) | data[1];
}

static net_u32 read_longword(const unsigned char *data)
{
    return (read_word(data) << 16U) | read_word(data + 2U);
}

static unsigned int sum_bytes(const unsigned char *data, unsigned int length,
                              unsigned int sum)
{
    for (unsigned int index = 0U; index < length; ++index) {
        sum += (unsigned int)data[index] << (index % 2U == 0U ? 8U : 0U);
    }
    while (sum > 0xffffU) {
        sum = (sum & 0xffffU) + (sum >> 16U);
    }
    return sum;
}

static unsigned int build_frame(unsigned char *frame, const struct peer *peer,
                                const struct net_mac_addr *mac, int to_guest,
                                struct wire_segment segment)
{
    unsigned int header = segment.mss != 0U ? 24U : 20U;
    unsigned int size = header + segment.length;
    unsigned int wire_size = 34U + size;
    unsigned char *ip = frame + 14U;
    unsigned char *tcp = ip + 20U;

    if (wire_size < NET_FRAME_MIN) {
        wire_size = NET_FRAME_MIN;
    }
    memset(frame, 0, wire_size);
    memcpy(frame, to_guest ? local_mac : mac->octets, 6U);
    memcpy(frame + 6U, to_guest ? mac->octets : local_mac, 6U);
    word(frame + 12U, 0x0800U);
    ip[0] = 0x45U;
    word(ip + 2U, 20U + size);
    word(ip + 6U, 0x4000U);
    ip[8] = 64U;
    ip[9] = 6U;
    memcpy(ip + 12U, to_guest ? remote.address.octets : network.address.octets, 4U);
    memcpy(ip + 16U, to_guest ? network.address.octets : remote.address.octets, 4U);
    word(ip + 10U, sum_bytes(ip, 20U, 0U) ^ 0xffffU);

    word(tcp, to_guest ? remote.port : peer->port);
    word(tcp + 2U, to_guest ? peer->port : remote.port);
    word(tcp + 4U, segment.seq >> 16U);
    word(tcp + 6U, segment.seq);
    word(tcp + 8U, segment.ack >> 16U);
    word(tcp + 10U, segment.ack);
    tcp[12] = (unsigned char)(header << 2U);
    tcp[13] = (unsigned char)segment.flags;
    word(tcp + 14U, segment.window);
    if (segment.mss != 0U) {
        tcp[20] = 2U;
        tcp[21] = 4U;
        word(tcp + 22U, segment.mss);
    }
    if (segment.length != 0U) {
        memcpy(tcp + header, segment.data, segment.length);
    }
    word(tcp + 16U, sum_bytes(tcp, size, sum_bytes(ip + 12U, 8U, 6U + size)) ^ 0xffffU);
    return wire_size;
}

static int receive_later(const struct peer *peer, struct wire_segment segment,
                          unsigned int delay)
{
    unsigned char frame[NET_FRAME_MAX];
    unsigned int length = build_frame(frame, peer, &incoming, 1, segment);

    return network_test_backend_schedule_receive(frame, length, delay) == 0;
}

static int expect(unsigned int index, const struct peer *peer,
                  const struct net_mac_addr *mac, struct wire_segment segment)
{
    unsigned char frame[NET_FRAME_MAX];
    unsigned int length = build_frame(frame, peer, mac, 0, segment);

    CHECK(network_test_backend_transmit_length(index) == length);
    CHECK(memcmp(frame, network_test_backend_transmit_frame(index), length) == 0);
    return 1;
}

static int take(const struct peer *peer, const struct net_mac_addr *mac,
                struct wire_segment segment)
{
    CHECK(expect(0U, peer, mac, segment));
    return network_test_backend_drop_transmits(1U) == 0;
}

static int state(const struct peer *peer, enum tcp_state expected, int reason,
                 unsigned int readable, unsigned int writable)
{
    struct tcp_status status;

    CHECK(tcp_status(peer->handle, &status) == 0 && status.state == expected);
    CHECK(status.terminal_result == reason);
    CHECK(status.readable_bytes == readable && status.writable_bytes == writable);
    CHECK(((status.flags & TCP_STATUS_READABLE) != 0U) == (readable != 0U));
    CHECK((status.flags & TCP_STATUS_PEER_EOF) == 0U);
    return 1;
}

static int reset(unsigned int clock, unsigned int ttl)
{
    struct net_config config = network;

    config.arp_cache_ttl_ms = ttl;
    network_test_backend_reset();
    network_test_backend_random_seed(0x12345678U);
    network_test_backend_clock_set(clock);
    memset(&net_global_context, 0, sizeof(net_global_context));
    memset(&tcp_global_context, 0, sizeof(tcp_global_context));
    return net_init(&config) == 0;
}

static int establish_config(struct peer *peer, net_u32 irs, unsigned int window,
                            unsigned int mss, const struct tcp_config *config)
{
    const unsigned char *syn;

    peer->handle = TCP_INVALID_HANDLE;
    peer->first_receive = irs + 1U;
    CHECK(net_arp_cache_store(&net_global_context, &network.gateway, &gateway) == 0);
    CHECK(tcp_connect(&peer->handle, &remote, config, 0U) == TCP_ERR_WOULD_BLOCK);
    CHECK(network_test_backend_transmit_length(0U) == NET_FRAME_MIN);
    syn = network_test_backend_transmit_frame(0U);
    peer->port = read_word(syn + 34U);
    peer->first_send = read_longword(syn + 38U) + 1U;
    CHECK(take(peer, &gateway, (struct wire_segment){
        .seq = peer->first_send - 1U, .flags = SYN, .mss = 536U
    }));
    CHECK(receive_later(peer, (struct wire_segment){
        .seq = irs, .ack = peer->first_send, .window = window,
        .flags = SYN | ACK, .mss = mss
    }, 0U));
    CHECK(tcp_connect(&peer->handle, &remote, config, 0U) == 0);
    CHECK(take(peer, &gateway, (struct wire_segment){
        .seq = peer->first_send, .ack = peer->first_receive,
        .flags = ACK, .window = TCP_QUEUE_CAPACITY
    }));
    return state(peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY);
}

static int establish(struct peer *peer, net_u32 irs, unsigned int window, unsigned int mss)
{
    return establish_config(peer, irs, window, mss, 0);
}

static int dispose(struct peer *peer, int final)
{
    CHECK(tcp_abort(peer->handle) == 0 && tcp_release(peer->handle) == 0);
    CHECK(!net_global_context.service_active && !net_global_context.pending_arp.active);
    if (final) {
        CHECK(tcp_deinit() == 0);
    }
    return 1;
}

static void pattern(unsigned char *data, unsigned int offset, unsigned int length,
                    unsigned int salt)
{
    for (unsigned int index = 0U; index < length; ++index) {
        unsigned int position = offset + index;

        data[index] = (unsigned char)(position * 29U + position / 251U + salt);
    }
}

static int ring_boundary(void)
{
    unsigned char guarded[TCP_QUEUE_CAPACITY + 2U];
    unsigned char out[7];
    static const unsigned char bytes[7] = {0U, 2U, 3U, 4U, 5U, 0U, 7U};
    unsigned int tail = tcp_ring_offset(TCP_QUEUE_CAPACITY - 4U, 1U);

    memset(guarded, 0xa5, sizeof(guarded));
    tcp_ring_write(guarded + 1U, tail, bytes, sizeof(bytes));
    tcp_ring_read(guarded + 1U, tail, out, sizeof(out));
    CHECK(memcmp(out, bytes, sizeof(out)) == 0);
    CHECK(guarded[0] == 0xa5U && guarded[TCP_QUEUE_CAPACITY + 1U] == 0xa5U);
    CHECK(guarded[5] == 0xa5U && guarded[TCP_QUEUE_CAPACITY - 3U] == 0xa5U);
    return 1;
}

static int transmit_boundaries(void)
{
    struct peer peer;
    struct wire_segment reply, expected;
    unsigned char data[TCP_QUEUE_CAPACITY + 13U], oracle[128];

    /* This clock with the test seed gives a wire ISN of 0xffffffd0. */
    CHECK(reset(1605633777U, network.arp_cache_ttl_ms));
    CHECK(establish(&peer, 71U, 128U, 128U) && peer.first_send == 0xffffffd1U);
    pattern(data, 0U, sizeof(data), 0U);
    CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == TCP_QUEUE_CAPACITY);
    memset(data, 0x5a, sizeof(data)); /* Accepted data must no longer alias the caller. */
    pattern(oracle, 0U, sizeof(oracle), 0U);
    expected = (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK, .data = oracle, .length = 128U
    };
    CHECK(take(&peer, &gateway, expected));
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 0U));
    network_test_backend_set_receive_clock_step(1U);
    CHECK(tcp_send(peer.handle, data, 1U, 0U) == TCP_ERR_WOULD_BLOCK);
    CHECK(tcp_send(peer.handle, data, 1U, 3U) == NET_ERR_TIMEOUT);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 0U));
    network_test_backend_set_receive_clock_step(0U);

    reply = (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send + 32U, .window = 128U, .flags = ACK
    };
    CHECK(receive_later(&peer, reply, 0U) && net_poll(0U) == 1);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 32U));
    CHECK(net_poll(0U) == 1);
    pattern(oracle, 128U, 32U, 0U);
    expected.seq += 128U;
    expected.length = 32U;
    CHECK(take(&peer, &gateway, expected));

    reply.ack = peer.first_send + 161U; /* Still queued, never sent. */
    CHECK(receive_later(&peer, reply, 0U) && net_poll(0U) == 1);
    expected.seq += 32U;
    expected.length = 0U;
    CHECK(take(&peer, &gateway, expected));
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 32U));

    reply.seq += 3U;
    reply.ack = peer.first_send + 32U;
    reply.window = 32U; /* Smaller than the outstanding flight. */
    CHECK(receive_later(&peer, reply, 0U) && net_poll(0U) == 1);
    --reply.seq;
    reply.window = 512U; /* Older sequence cannot replace the newer window. */
    CHECK(receive_later(&peer, reply, 0U) && net_poll(0U) == 0);
    CHECK(net_poll(0U) == 0);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 32U));

    reply.seq += 2U;
    reply.ack = peer.first_send + 160U;
    reply.window = 0U;
    CHECK(receive_later(&peer, reply, 0U) && net_poll(0U) == 1);
    CHECK(tcp_send(peer.handle, data, 13U, 0U) == 13);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 147U));
    CHECK(net_poll(0U) == 0 && network_test_backend_transmit_count() == 0U);

    reply.window = 7U;
    CHECK(receive_later(&peer, reply, 0U) && net_poll(0U) == 1);
    CHECK(net_poll(0U) == 1);
    pattern(oracle, 160U, 7U, 0U);
    expected.length = 7U;
    CHECK(take(&peer, &gateway, expected));
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 147U));
    return dispose(&peer, 1);
}

static int arp_reply(const struct net_mac_addr *mac)
{
    static const unsigned char prefix[] = {8, 6, 0, 1, 8, 0, 6, 4, 0, 2};
    unsigned char frame[NET_FRAME_MIN] = {0};

    memcpy(frame, local_mac, 6U);
    memcpy(frame + 6U, mac->octets, 6U);
    memcpy(frame + 12U, prefix, sizeof(prefix));
    memcpy(frame + 22U, mac->octets, 6U);
    memcpy(frame + 28U, network.gateway.octets, 4U);
    memcpy(frame + 32U, local_mac, 6U);
    memcpy(frame + 38U, network.address.octets, 4U);
    return network_test_backend_queue_receive(frame, sizeof(frame)) == 0;
}

static int take_arp_request(void)
{
    static const unsigned char header[] = {
        255U, 255U, 255U, 255U, 255U, 255U, 0x52U, 0x54U, 0U, 0x12U, 0x34U, 0x56U,
        8U, 6U, 0U, 1U, 8U, 0U, 6U, 4U, 0U, 1U
    };
    const unsigned char *frame;

    CHECK(network_test_backend_transmit_length(0U) == NET_FRAME_MIN);
    frame = network_test_backend_transmit_frame(0U);
    CHECK(memcmp(frame, header, sizeof(header)) == 0);
    CHECK(memcmp(frame + 38U, network.gateway.octets, 4U) == 0);
    return network_test_backend_drop_transmits(1U) == 0;
}

static int receive_boundaries(void)
{
    struct peer peer;
    struct wire_segment packet, ack;
    unsigned char data[536], output[257], oracle[257];
    unsigned int stored = 0U, delivered = 0U;

    CHECK(reset(0U, 1000U) && establish(&peer, UINT_MAX - 7U, 512U, 0U));
    pattern(data, 0U, 8U, 27U);
    packet = (struct wire_segment){
        .seq = peer.first_receive + 4U, .ack = peer.first_send, .window = 512U,
        .flags = ACK, .data = data + 4U, .length = 4U
    };
    ack = (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK
    };
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1 && take(&peer, &gateway, ack));
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY));
    packet.seq = peer.first_receive;
    packet.data = data;
    ack.ack += 4U;
    ack.window -= 4U;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1 && take(&peer, &gateway, ack));
    packet.seq += 2U;
    packet.data = data + 2U;
    packet.length = 6U;
    ack.ack += 4U;
    ack.window -= 4U;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1 && take(&peer, &gateway, ack));
    CHECK(ack.ack == 1U); /* Crossed the TCP sequence boundary. */
    packet.seq = peer.first_receive;
    packet.data = data;
    packet.length = 8U;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1 && take(&peer, &gateway, ack));

    stored = 8U;
    while (stored < TCP_QUEUE_CAPACITY) {
        unsigned int available = TCP_QUEUE_CAPACITY - stored;
        unsigned int copied = available < 500U ? available : 500U;

        packet.seq = peer.first_receive + stored;
        packet.data = data;
        /* The last segment spans the end of the advertised window. */
        packet.length = copied + (copied == available ? 13U : 0U);
        pattern(data, stored, packet.length, 27U);
        stored += copied;
        ack.ack += copied;
        ack.window -= copied;
        CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1);
        CHECK(take(&peer, &gateway, ack));
    }
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, TCP_QUEUE_CAPACITY, TCP_QUEUE_CAPACITY));
    packet.seq = ack.ack + 1U;
    packet.length = 0U;
    packet.flags = RST;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 0);
    CHECK(network_test_backend_transmit_count() == 0U);
    packet.seq = ack.ack;
    packet.length = 1U;
    packet.flags = ACK;
    pattern(data, stored, 1U, 27U);
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1 && take(&peer, &gateway, ack));
    packet.length = 0U;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 0);
    --packet.seq;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1 && take(&peer, &gateway, ack));

    network_test_backend_clock_advance(1000U);
    CHECK(tcp_recv(peer.handle, output, 1U, 0U) == 1 && take_arp_request());
    pattern(oracle, 0U, 1U, 27U);
    CHECK(output[0] == oracle[0]);
    packet.seq = ack.ack;
    packet.length = 1U;
    /* There is one free byte, but the zero window update has not left yet. */
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 0);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, TCP_QUEUE_CAPACITY - 1U, TCP_QUEUE_CAPACITY));
    CHECK(tcp_recv(peer.handle, output, 16U, 0U) == 16);
    pattern(oracle, 1U, 16U, 27U);
    CHECK(memcmp(output, oracle, 16U) == 0);
    CHECK(network_test_backend_transmit_count() == 0U);
    CHECK(net_global_context.pending_arp.started_ms == 1000U &&
          net_global_context.pending_arp.attempts == 1U);
    CHECK(arp_reply(&refreshed) && net_poll(0U) == 1 && net_poll(0U) == 1);
    ack.window = 17U;
    CHECK(take(&peer, &refreshed, ack));
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1);
    ++stored;
    ++ack.ack;
    --ack.window;
    CHECK(take(&peer, &refreshed, ack));
    delivered = 17U;
    while (delivered < stored) {
        unsigned int need = stored - delivered;
        unsigned int count = need < sizeof(output) ? need : sizeof(output);

        CHECK(tcp_recv(peer.handle, output, sizeof(output), 0U) == (int)count);
        pattern(oracle, delivered, count, 27U);
        CHECK(memcmp(output, oracle, count) == 0);
        delivered += count;
        ack.window += count;
        CHECK(take(&peer, &refreshed, ack));
    }

    /* A replay uses live sequence/credit; an attached FIN does not fake EOF. */
    packet.seq = peer.first_receive - 1U;
    packet.flags = SYN | ACK;
    packet.length = 0U;
    packet.window = 1U;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1 && take(&peer, &refreshed, ack));
    CHECK(tcp_global_context.connection.stream.peer_window == 512U);
    packet.seq = ack.ack;
    packet.window = 512U;
    packet.flags = ACK | FIN;
    packet.length = 2U;
    pattern(data, delivered, 2U, 27U);
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1);
    ack.ack += 2U;
    ack.window -= 2U;
    CHECK(take(&peer, &refreshed, ack));
    CHECK(tcp_recv(peer.handle, output, sizeof(output), 0U) == 2);
    CHECK(memcmp(output, data, 2U) == 0);
    ack.window += 2U;
    CHECK(take(&peer, &refreshed, ack));
    CHECK(tcp_recv(peer.handle, output, sizeof(output), 0U) == TCP_ERR_WOULD_BLOCK);
    network_test_backend_set_receive_clock_step(1U);
    CHECK(tcp_recv(peer.handle, output, sizeof(output), 3U) == NET_ERR_TIMEOUT);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY));
    packet.flags = RST;
    packet.length = 0U;
    packet.seq = peer.first_receive;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 0);
    packet.seq = ack.ack;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1);
    CHECK(state(&peer, TCP_STATE_FAILED, TCP_ERR_RESET, 0U, 0U));
    return dispose(&peer, 1);
}

static int wait_services_both_directions(void)
{
    struct peer peer;
    unsigned char queued[TCP_QUEUE_CAPACITY] = {0};
    static const unsigned char input[] = {'a', 0U, 'b', 0U};
    unsigned char output[4];

    CHECK(reset(0U, network.arp_cache_ttl_ms) && establish(&peer, 77U, 0U, 128U));
    CHECK(tcp_send(peer.handle, queued, sizeof(queued), 0U) == TCP_QUEUE_CAPACITY);
    CHECK(network_test_backend_transmit_count() == 0U);
    CHECK(receive_later(&peer, (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send, .window = 128U, .flags = ACK
    }, 1U));
    CHECK(receive_later(&peer, (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send, .window = 128U,
        .flags = ACK, .data = input, .length = 2U
    }, 3U));
    network_test_backend_set_receive_clock_step(1U);
    CHECK(tcp_recv(peer.handle, output, sizeof(output), 10U) == 2);
    CHECK(memcmp(output, input, 2U) == 0);
    CHECK(take(&peer, &gateway, (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK, .data = queued, .length = 128U
    }));
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 0U));

    CHECK(receive_later(&peer, (struct wire_segment){
        .seq = peer.first_receive + 2U, .ack = peer.first_send + 32U, .window = 128U,
        .flags = ACK, .data = input + 2U, .length = 2U
    }, 2U));
    CHECK(tcp_send(peer.handle, input, sizeof(input), 8U) == (int)sizeof(input));
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 2U, 32U - sizeof(input)));
    CHECK(tcp_recv(peer.handle, output + 2U, 2U, 0U) == 2);
    CHECK(memcmp(output, input, sizeof(input)) == 0);
    return dispose(&peer, 1);
}

static int wait_expiry_retains_progress(void)
{
    static const unsigned char data[] = {1U, 0U, 2U};
    static const unsigned char queued[TCP_QUEUE_CAPACITY] = {0};
    struct peer peer;

    for (unsigned int elapsed = 2U; elapsed <= 3U; ++elapsed) {
        unsigned char output[sizeof(data)] = {0xffU, 0xffU, 0xffU};
        net_u32 start;

        CHECK(reset(UINT_MAX - 1U, network.arp_cache_ttl_ms) &&
              establish(&peer, 31U, 64U, 64U));
        start = net_clock_now_ms();
        CHECK(receive_later(&peer, (struct wire_segment){
            .seq = peer.first_receive, .ack = peer.first_send, .window = 64U,
            .flags = ACK, .data = data, .length = sizeof(data)
        }, elapsed));
        network_test_backend_set_receive_clock_step(elapsed);
        network_test_backend_set_send_clock_step(5U);
        CHECK(tcp_recv(peer.handle, output, sizeof(output), 2U) == NET_ERR_TIMEOUT);
        CHECK(net_elapsed_ms(start, net_clock_now_ms()) == elapsed);
        CHECK(output[0] == 0xffU && output[1] == 0xffU && output[2] == 0xffU);
        CHECK(network_test_backend_transmit_count() == 0U);
        CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, sizeof(data), TCP_QUEUE_CAPACITY));
        CHECK(tcp_recv(peer.handle, output, sizeof(output), 0U) == (int)sizeof(data));
        CHECK(memcmp(data, output, sizeof(data)) == 0);
        CHECK(take(&peer, &gateway, (struct wire_segment){
            .seq = peer.first_send, .ack = peer.first_receive + sizeof(data),
            .window = TCP_QUEUE_CAPACITY, .flags = ACK
        }));
        CHECK(dispose(&peer, 1));

        CHECK(reset(UINT_MAX - 1U, network.arp_cache_ttl_ms) &&
              establish(&peer, 31U, 64U, 64U));
        CHECK(tcp_send(peer.handle, queued, sizeof(queued), 0U) == (int)sizeof(queued));
        CHECK(take(&peer, &gateway, (struct wire_segment){
            .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
            .flags = ACK, .data = queued, .length = 64U
        }));
        start = net_clock_now_ms();
        CHECK(receive_later(&peer, (struct wire_segment){
            .seq = peer.first_receive, .ack = peer.first_send + 64U, .window = 64U, .flags = ACK
        }, elapsed));
        network_test_backend_set_receive_clock_step(elapsed);
        CHECK(tcp_send(peer.handle, data, 1U, 2U) == NET_ERR_TIMEOUT);
        CHECK(net_elapsed_ms(start, net_clock_now_ms()) == elapsed);
        CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 64U));
        CHECK(network_test_backend_transmit_count() == 0U);
        CHECK(tcp_send(peer.handle, data, 1U, 0U) == 1);
        CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, 63U));
        CHECK(take(&peer, &gateway, (struct wire_segment){
            .seq = peer.first_send + 64U, .ack = peer.first_receive,
            .window = TCP_QUEUE_CAPACITY, .flags = ACK, .data = queued + 64U, .length = 64U
        }));
        CHECK(dispose(&peer, 1));
    }
    return 1;
}

static int started_io_can_finish(void)
{
    static const unsigned char bytes[] = {1U, 0U};
    struct peer peer;
    unsigned char readback = 0xffU;
    net_u32 start;

    CHECK(reset(0U, network.arp_cache_ttl_ms) && establish(&peer, 31U, 64U, 64U));
    CHECK(receive_later(&peer, (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send, .window = 64U,
        .flags = ACK, .data = bytes, .length = 1U
    }, 0U));
    start = net_clock_now_ms();
    network_test_backend_set_send_clock_step(3U);
    network_test_backend_set_receive_clock_step(1U);
    CHECK(tcp_send(peer.handle, bytes, sizeof(bytes), 2U) == (int)sizeof(bytes));
    CHECK(net_elapsed_ms(start, net_clock_now_ms()) == 3U);
    CHECK(network_test_backend_transmit_time(0U) == start);
    CHECK(take(&peer, &gateway, (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK | PSH, .data = bytes, .length = sizeof(bytes)
    }));
    CHECK(network_test_backend_pending_receive_count() == 1U);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - sizeof(bytes)));

    /* A timely raw receive may start its ACK, but the read has not copied yet. */
    start = net_clock_now_ms();
    CHECK(tcp_recv(peer.handle, &readback, 1U, 2U) == NET_ERR_TIMEOUT);
    CHECK(readback == 0xffU && net_elapsed_ms(start, net_clock_now_ms()) == 4U);
    CHECK(net_elapsed_ms(start, network_test_backend_transmit_time(0U)) == 1U);
    CHECK(take(&peer, &gateway, (struct wire_segment){
        .seq = peer.first_send + sizeof(bytes), .ack = peer.first_receive + 1U,
        .window = TCP_QUEUE_CAPACITY - 1U, .flags = ACK
    }));
    /* Now the copy starts before the deadline; a slow window update cannot undo it. */
    start = net_clock_now_ms();
    CHECK(tcp_recv(peer.handle, &readback, 1U, 2U) == 1 && readback == bytes[0]);
    CHECK(net_elapsed_ms(start, net_clock_now_ms()) == 3U);
    CHECK(take(&peer, &gateway, (struct wire_segment){
        .seq = peer.first_send + sizeof(bytes), .ack = peer.first_receive + 1U,
        .window = TCP_QUEUE_CAPACITY, .flags = ACK
    }));
    return dispose(&peer, 1);
}

static int deferred_raw_send_preserves_timers(void)
{
    static const unsigned char data[] = {1U, 0U};
    const struct tcp_rto *rto = &tcp_global_context.connection.stream.rto;
    struct peer peer;
    struct wire_segment expected;
    unsigned char output = 0xffU;

    /* Keep the cache valid past the maximum neighbor resolution budget. */
    CHECK(reset(300U, 300000U) && establish(&peer, 31U, 64U, 64U));
    /* The eight clock reads after entry consume the wait at the raw TX gate. */
    network_test_backend_set_clock_read_step(1U);
    CHECK(tcp_send(peer.handle, data, sizeof(data), 8U) == (int)sizeof(data));
    network_test_backend_set_clock_read_step(0U);
    CHECK(network_test_backend_transmit_count() == 0U);
    CHECK(!net_global_context.pending_arp.active &&
          !tcp_global_context.connection.stream.neighbor_pending);
    network_test_backend_clock_advance(120001U);
    expected = (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK | PSH, .data = data, .length = sizeof(data)
    };
    CHECK(net_poll(0U) == 1 && take(&peer, &gateway, expected));
    CHECK(rto->timer_started_ms == net_clock_now_ms() && rto->retries == 0U);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - sizeof(data)));

    network_test_backend_clock_advance(1000U);
    network_test_backend_set_clock_read_step(1U);
    CHECK(tcp_recv(peer.handle, &output, 1U, 8U) == NET_ERR_TIMEOUT && output == 0xffU);
    network_test_backend_set_clock_read_step(0U);
    CHECK(network_test_backend_transmit_count() == 0U && rto->retries == 0U &&
          rto->wait_ms == 1000U);
    CHECK(net_poll(0U) == 1 && take(&peer, &gateway, expected));
    CHECK(rto->retries == 1U && rto->wait_ms == 2000U);
    return dispose(&peer, 1);
}

static int wait_expiry_defers_control_replies(void)
{
    static const unsigned char arp_header[] = {8, 6, 0, 1, 8, 0, 6, 4, 0, 1};
    struct peer peer;
    unsigned char frame[NET_FRAME_MIN];
    unsigned char byte = 0xffU;

    CHECK(reset(0U, network.arp_cache_ttl_ms) && establish(&peer, 31U, 64U, 64U));
    for (int echo = 0; echo <= 1; ++echo) {
        net_u32 received = net_global_context.counters.received_frames;
        net_u32 learned = net_global_context.counters.arp_cache_updates;
        net_u32 requests = net_global_context.counters.echo_requests;
        const unsigned char *sent;

        memset(frame, 0, sizeof(frame));
        memcpy(frame, local_mac, sizeof(local_mac));
        memcpy(frame + 6U, refreshed.octets, 6U);
        if (echo) {
            unsigned char *ip = frame + 14U, *icmp = ip + 20U;

            word(frame + 12U, 0x0800U);
            ip[0] = 0x45U;
            word(ip + 2U, 29U);
            word(ip + 6U, 0x4000U);
            ip[8] = 64U;
            ip[9] = 1U;
            memcpy(ip + 12U, network.gateway.octets, 4U);
            memcpy(ip + 16U, network.address.octets, 4U);
            word(ip + 10U, sum_bytes(ip, 20U, 0U) ^ 0xffffU);
            icmp[0] = 8U;
            word(icmp + 4U, 7U);
            word(icmp + 6U, 9U);
            icmp[8] = 0x34U;
            word(icmp + 2U, sum_bytes(icmp, 9U, 0U) ^ 0xffffU);
        } else {
            memcpy(frame + 12U, arp_header, sizeof(arp_header));
            memcpy(frame + 22U, refreshed.octets, 6U);
            memcpy(frame + 28U, network.gateway.octets, 4U);
            memcpy(frame + 38U, network.address.octets, 4U);
        }
        CHECK(network_test_backend_schedule_receive(frame, sizeof(frame), 3U) == 0);
        network_test_backend_set_receive_clock_step(3U);
        CHECK(tcp_recv(peer.handle, &byte, 1U, 2U) == NET_ERR_TIMEOUT && byte == 0xffU);
        CHECK(network_test_backend_transmit_count() == 0U);
        CHECK(network_test_backend_pending_receive_count() == 0U);
        CHECK(net_global_context.counters.received_frames == received &&
              net_global_context.counters.arp_cache_updates == learned &&
              net_global_context.counters.echo_requests == requests);

        CHECK(net_poll(0U) == 1 && network_test_backend_transmit_length(0U) == NET_FRAME_MIN);
        sent = network_test_backend_transmit_frame(0U);
        CHECK(memcmp(sent, refreshed.octets, 6U) == 0);
        if (echo) {
            CHECK(read_word(sent + 12U) == 0x0800U && sent[34] == 0U && sent[42] == 0x34U);
            CHECK(read_word(sent + 38U) == 7U && read_word(sent + 40U) == 9U);
        } else {
            CHECK(read_word(sent + 12U) == 0x0806U && read_word(sent + 20U) == 2U);
        }
        CHECK(net_global_context.counters.received_frames == received + 1U &&
              net_global_context.counters.arp_cache_updates == learned + (echo == 0) &&
              net_global_context.counters.echo_requests == requests + (echo != 0));
        CHECK(network_test_backend_drop_transmits(1U) == 0 && net_poll(0U) == 0);
    }
    return dispose(&peer, 1);
}

static int duplex_bytes(void)
{
    enum { TX_COUNT = 7351U, RX_COUNT = 7129U };
    unsigned char outbound[TX_COUNT], inbound[RX_COUNT], readback[173];
    unsigned int accepted = 0U, emitted = 0U, supplied = 0U, read = 0U;
    unsigned int last_ack = 0U, peer_acked = 0U, right = TCP_QUEUE_CAPACITY;
    struct peer peer;

    CHECK(reset(0U, network.arp_cache_ttl_ms) && establish(&peer, UINT_MAX - 2U, 400U, 311U));
    pattern(outbound, 0U, sizeof(outbound), 0U);
    pattern(inbound, 0U, sizeof(inbound), 77U);
    for (unsigned int pass = 0U; pass < 256U; ++pass) {
        unsigned int limit, count;
        int result;

        if (accepted != TX_COUNT) {
            count = TX_COUNT - accepted;
            if (count > 1807U) {
                count = 1807U;
            }
            result = tcp_send(peer.handle, outbound + accepted, count, 0U);
            CHECK(result == TCP_ERR_WOULD_BLOCK || (result > 0 && (unsigned int)result <= count));
            if (result > 0) {
                accepted += (unsigned int)result;
            }
        }
        if (read != RX_COUNT) {
            count = read == 0U ? 1U : sizeof(readback);
            result = tcp_recv(peer.handle, readback, count, 0U);
            CHECK(result == TCP_ERR_WOULD_BLOCK || (result > 0 && (unsigned int)result <= count));
            if (result > 0) {
                CHECK((unsigned int)result <= RX_COUNT - read);
                CHECK(memcmp(readback, inbound + read, (unsigned int)result) == 0);
                read += (unsigned int)result;
            }
        }
        CHECK(net_poll(0U) >= 0);
        while (network_test_backend_transmit_count() != 0U) {
            const unsigned char *frame = network_test_backend_transmit_frame(0U);
            unsigned int length, acknowledged, window, flags;

            CHECK(network_test_backend_transmit_length(0U) >= NET_FRAME_MIN);
            CHECK(read_word(frame + 16U) >= 40U);
            length = read_word(frame + 16U) - 40U;
            acknowledged = read_longword(frame + 42U) - peer.first_receive;
            window = read_word(frame + 48U);
            flags = frame[47];
            CHECK(acknowledged >= peer_acked && acknowledged <= supplied);
            CHECK(acknowledged + window >= right && acknowledged + window <= read + TCP_QUEUE_CAPACITY);
            CHECK(length <= 311U && length <= accepted - emitted);
            CHECK(flags == ACK || (length != 0U && flags == (ACK | PSH)));
            CHECK(take(&peer, &gateway, (struct wire_segment){
                .seq = peer.first_send + emitted, .ack = peer.first_receive + acknowledged,
                .window = window, .flags = flags, .data = outbound + emitted, .length = length
            }));
            emitted += length;
            peer_acked = acknowledged;
            right = acknowledged + window;
        }
        if (last_ack == TX_COUNT && peer_acked == RX_COUNT && read == RX_COUNT) {
            CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY));
            CHECK(accepted == TX_COUNT && emitted == TX_COUNT && supplied == RX_COUNT);
            return dispose(&peer, 1);
        }

        limit = pass % 2U == 0U ? 239U : 317U;
        count = RX_COUNT - supplied;
        if (count > limit) {
            count = limit;
        }
        CHECK(right >= supplied);
        if (count > right - supplied) {
            count = right - supplied;
        }
        if (count != 0U || last_ack != emitted) {
            CHECK(receive_later(&peer, (struct wire_segment){
                .seq = peer.first_receive + supplied, .ack = peer.first_send + emitted,
                .window = 400U, .flags = ACK, .data = inbound + supplied, .length = count
            }, 0U));
            supplied += count;
            last_ack = emitted;
        }
    }
    CHECK(0 && "full duplex did not complete within the bounded trace");
    return 0;
}

static int neighbor_during_stream(void)
{
    struct peer peer;
    unsigned char outbound[100] = {0}, output[2];
    static const unsigned char input[] = {0U, 1U};
    struct wire_segment packet;

    CHECK(reset(0U, 1000U) && establish(&peer, 29U, 64U, 64U));
    CHECK(tcp_send(peer.handle, outbound, sizeof(outbound), 0U) == (int)sizeof(outbound));
    CHECK(take(&peer, &gateway, (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK, .data = outbound, .length = 64U
    }));
    network_test_backend_clock_advance(1000U);
    packet = (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send + 32U, .window = 64U,
        .flags = ACK, .data = input, .length = 1U
    };
    /* The RTO starts ARP first; the incoming partial ACK then changes the range. */
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 2 && take_arp_request());
    CHECK(net_global_context.pending_arp.active &&
          tcp_global_context.connection.stream.rto.retries == 0U);
    CHECK(tcp_recv(peer.handle, output, 1U, 0U) == 1 && output[0] == 0U);
    packet.seq += 1U;
    packet.ack += 32U;
    packet.data = input + 1U;
    CHECK(receive_later(&peer, packet, 0U) && net_poll(0U) == 1);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 1U, TCP_QUEUE_CAPACITY - 36U));
    CHECK(network_test_backend_transmit_count() == 0U);
    CHECK(net_global_context.pending_arp.started_ms == 1000U &&
          net_global_context.pending_arp.attempts == 1U);
    CHECK(arp_reply(&refreshed) && net_poll(0U) == 1 && net_poll(0U) == 1);
    CHECK(take(&peer, &refreshed, (struct wire_segment){
        .seq = peer.first_send + 64U, .ack = peer.first_receive + 2U,
        .window = TCP_QUEUE_CAPACITY - 1U, .flags = ACK | PSH,
        .data = outbound + 64U, .length = 36U
    }));
    CHECK(tcp_recv(peer.handle, output + 1U, 1U, 0U) == 1 && output[1] == 1U);
    CHECK(take(&peer, &refreshed, (struct wire_segment){
        .seq = peer.first_send + 100U, .ack = peer.first_receive + 2U,
        .window = TCP_QUEUE_CAPACITY, .flags = ACK
    }));
    return dispose(&peer, 1);
}

static int retry_of_timed_prefix(void)
{
    unsigned char data[56];
    struct peer peer;
    struct wire_segment expected, ack;

    CHECK(reset(1605633777U, network.arp_cache_ttl_ms) &&
          establish(&peer, 31U, 64U, 64U) && peer.first_send == 0xffffffd1U);
    pattern(data, 0U, sizeof(data), 9U);
    CHECK(tcp_send(peer.handle, data, 40U, 0U) == 40);
    expected = (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK | PSH, .data = data, .length = 40U
    };
    CHECK(take(&peer, &gateway, expected));
    network_test_backend_clock_advance(800U);
    CHECK(tcp_send(peer.handle, data + 40U, 16U, 0U) == 16);
    expected.seq += 40U;
    expected.data += 40U;
    expected.length = 16U;
    CHECK(take(&peer, &gateway, expected));

    network_test_backend_clock_advance(199U);
    ack = (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send, .window = 48U, .flags = ACK
    };
    CHECK(receive_later(&peer, ack, 0U) && net_poll(0U) == 1);
    CHECK(receive_later(&peer, ack, 0U) && net_poll(0U) == 0);
    CHECK(network_test_backend_transmit_count() == 0U);
    network_test_backend_clock_advance(1U);
    CHECK(net_poll(0U) == 1);
    /* The newer 16-byte tail has only been in flight for 200 ms. */
    expected.seq = peer.first_send;
    expected.data = data;
    expected.length = 40U;
    expected.flags = ACK;
    CHECK(take(&peer, &gateway, expected));
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - sizeof(data)));

    network_test_backend_clock_advance(20U);
    ack.ack += 8U;
    CHECK(receive_later(&peer, ack, 0U) && net_poll(0U) == 1);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - 48U));
    network_test_backend_clock_advance(1999U);
    CHECK(net_poll(0U) == 0 && network_test_backend_transmit_count() == 0U);
    network_test_backend_clock_advance(1U);
    CHECK(net_poll(0U) == 1);
    expected.seq += 8U;
    expected.data += 8U;
    expected.length = 48U;
    expected.flags = ACK | PSH;
    CHECK(take(&peer, &gateway, expected));
    ack.ack = peer.first_send + sizeof(data);
    CHECK(ack.ack < peer.first_send);
    CHECK(receive_later(&peer, ack, 0U) && net_poll(0U) == 1);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY));
    network_test_backend_clock_advance(4000U);
    CHECK(net_poll(0U) == 0 && network_test_backend_transmit_count() == 0U);
    return dispose(&peer, 1);
}

static int retry_after_neighbor_refresh(void)
{
    unsigned char data[64];
    struct peer peer;
    struct wire_segment expected;
    const struct tcp_rto *rto = &tcp_global_context.connection.stream.rto;

    CHECK(reset(0U, 1000U) && establish(&peer, 31U, 64U, 64U));
    pattern(data, 0U, sizeof(data), 12U);
    CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == (int)sizeof(data));
    expected = (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK | PSH, .data = data, .length = sizeof(data)
    };
    CHECK(take(&peer, &gateway, expected));
    network_test_backend_clock_advance(1000U);
    CHECK(net_poll(0U) == 1 && take_arp_request());
    CHECK(rto->retries == 0U && rto->wait_ms == 1000U);
    network_test_backend_clock_advance(50U);
    CHECK(receive_later(&peer, (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send, .window = 16U, .flags = ACK
    }, 0U) && net_poll(0U) == 1);
    CHECK(network_test_backend_transmit_count() == 0U && rto->retries == 0U);
    CHECK(arp_reply(&refreshed) && net_poll(0U) == 1 && rto->retries == 0U);
    CHECK(net_poll(0U) == 1 && network_test_backend_transmit_time(0U) == 1050U);
    expected.flags = ACK;
    expected.length = 16U;
    CHECK(take(&peer, &refreshed, expected));
    CHECK(rto->retries == 1U && rto->wait_ms == 2000U && rto->timer_started_ms == 1050U);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - sizeof(data)));
    CHECK(receive_later(&peer, (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send, .flags = ACK
    }, 0U) && net_poll(0U) == 1);
    network_test_backend_clock_advance(2000U);
    CHECK(net_poll(0U) == 0 && network_test_backend_transmit_count() == 0U);
    CHECK(!net_global_context.pending_arp.active && rto->retries == 1U);
    return dispose(&peer, 1);
}

static int ack_releases_idle_neighbor(void)
{
    static const unsigned char data[] = {9U, 0U, 8U};
    static const unsigned int prefixes[] = {1U, sizeof(data)};
    const struct net_ipv4_addr other = {{10U, 0U, 2U, 3U}};
    struct peer peer;
    struct net_mac_addr resolved;

    for (unsigned int index = 0U; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
        unsigned int acked = prefixes[index];

        CHECK(reset(0U, 1000U) && establish(&peer, 31U, 64U, 64U));
        CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == (int)sizeof(data));
        CHECK(take(&peer, &gateway, (struct wire_segment){
            .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
            .flags = ACK | PSH, .data = data, .length = sizeof(data)
        }));
        network_test_backend_clock_advance(1000U);
        CHECK(net_poll(0U) == 1 && take_arp_request());
        CHECK(net_global_context.pending_arp.active &&
              net_global_context.pending_arp.owner == peer.handle);
        CHECK(net_arp_cache_store(&net_global_context, &other, &refreshed) == 0);
        CHECK(net_resolve_arp(&other, &resolved, 0U) == NET_ERR_BUSY);

        network_test_backend_clock_advance(1U);
        CHECK(receive_later(&peer, (struct wire_segment){
            .seq = peer.first_receive, .ack = peer.first_send + acked,
            .window = 64U, .flags = ACK
        }, 0U) && net_poll(0U) == 1);
        CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U,
                    TCP_QUEUE_CAPACITY - (sizeof(data) - acked)));
        /* No additional service call is needed before another ARP user. */
        CHECK(net_resolve_arp(&other, &resolved, 0U) == 0);
        CHECK(memcmp(&resolved, &refreshed, sizeof(resolved)) == 0);
        CHECK(!net_global_context.pending_arp.active &&
              !tcp_global_context.connection.stream.neighbor_pending);
        CHECK(network_test_backend_transmit_count() == 0U);
        if (acked < sizeof(data)) {
            network_test_backend_clock_advance(1000U);
            CHECK(net_poll(0U) == 1 && take_arp_request());
            CHECK(net_global_context.pending_arp.started_ms == 2001U);
        }
        CHECK(dispose(&peer, 1));
    }
    return 1;
}

enum retry_cutoff {
    RETRY_NO_ACK,
    RETRY_DUPLICATE_ACK,
    RETRY_ADVANCING_ACK,
    RETRY_CLOSED_WINDOW,
    RETRY_HARD_DEADLINE
};

static int retry_exhaustion(enum retry_cutoff cutoff)
{
    static const unsigned int retry_ms[] = {1000U, 3000U, 7000U, 15000U, 31000U};
    static const unsigned char data[] = {0x85U, 0U, 0xfeU};
    const net_u32 start = UINT_MAX - 700U;
    struct tcp_config config;
    struct peer peer;
    struct wire_segment expected;
    net_u32 received;
    int reason;
    unsigned char readback = 0xffU;

    CHECK(tcp_config_defaults(&config) == 0);
    if (cutoff == RETRY_HARD_DEADLINE) {
        config.transmit_timeout_ms = 63000U;
    }
    CHECK(reset(start, 300000U) && establish_config(&peer, 31U, 64U, 64U, &config));
    CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == (int)sizeof(data));
    expected = (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK | PSH, .data = data, .length = sizeof(data)
    };
    CHECK(take(&peer, &gateway, expected));
    network_test_backend_set_receive_clock_step(1U);
    CHECK(tcp_recv(peer.handle, &readback, 1U, 25U) == NET_ERR_TIMEOUT && readback == 0xffU);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - sizeof(data)));
    network_test_backend_set_receive_clock_step(0U);
    for (unsigned int index = 0U; index < sizeof(retry_ms) / sizeof(retry_ms[0]); ++index) {
        network_test_backend_clock_set(start + retry_ms[index] - 1U);
        CHECK(net_poll(0U) == 0 && network_test_backend_transmit_count() == 0U);
        network_test_backend_clock_advance(1U);
        CHECK(net_poll(0U) == 1);
        CHECK(network_test_backend_transmit_time(0U) == start + retry_ms[index]);
        CHECK(take(&peer, &gateway, expected));
    }
    network_test_backend_clock_set(start + 62999U);
    CHECK(net_poll(0U) == 0);
    received = net_global_context.counters.received_frames;
    if (cutoff != RETRY_NO_ACK) {
        struct wire_segment ack = {
            .seq = peer.first_receive, .ack = peer.first_send, .flags = ACK,
            .window = cutoff == RETRY_CLOSED_WINDOW ? 0U : 64U
        };

        if (cutoff == RETRY_ADVANCING_ACK || cutoff == RETRY_HARD_DEADLINE) {
            ack.ack += sizeof(data);
        }
        CHECK(receive_later(&peer, ack, 0U));
        if (cutoff == RETRY_DUPLICATE_ACK) {
            /* The ready advancing ACK behind it must not buy a second input slot. */
            ack.ack += sizeof(data);
            CHECK(receive_later(&peer, ack, 0U));
        }
    }
    network_test_backend_clock_advance(1U);
    CHECK(net_poll(0U) == 1 && network_test_backend_transmit_count() == 0U);
    CHECK(net_global_context.counters.received_frames - received == (cutoff != RETRY_NO_ACK));
    if (cutoff == RETRY_ADVANCING_ACK || cutoff == RETRY_CLOSED_WINDOW) {
        CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY -
                    (cutoff == RETRY_CLOSED_WINDOW ? sizeof(data) : 0U)));
        CHECK(net_poll(0U) == 0);
    } else {
        reason = cutoff == RETRY_HARD_DEADLINE ? TCP_ERR_TRANSMIT_TIMEOUT : TCP_ERR_RETRIES;
        CHECK(state(&peer, TCP_STATE_FAILED, reason, 0U, 0U));
        CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == reason);
        CHECK(tcp_recv(peer.handle, &readback, 1U, 0U) == reason);
    }
    return dispose(&peer, 1);
}

static int raw_send_obeys_transmit_deadline(void)
{
    static const unsigned char data = 0x81U;
    static const struct { unsigned int wait_ms, elapsed_ms; } cases[] = {
        {0U, 6U}, {20U, 7U}
    };
    struct tcp_config config;
    struct peer peer;
    unsigned char readback = 0xffU;

    CHECK(tcp_config_defaults(&config) == 0);
    for (unsigned int index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        /* Clock reads move time across the deadline during frame preparation. */
        unsigned int elapsed = cases[index].elapsed_ms;

        for (unsigned int slack = 0U; slack <= 1U; ++slack) {
            config.transmit_timeout_ms = elapsed + slack;
            CHECK(reset(100U, network.arp_cache_ttl_ms) &&
                  establish_config(&peer, 31U, 64U, 64U, &config));
            network_test_backend_set_clock_read_step(1U);
            /* The accepted byte is reported even if the same call notices expiry. */
            CHECK(tcp_send(peer.handle, &data, 1U, cases[index].wait_ms) == 1);
            network_test_backend_set_clock_read_step(0U);
            if (slack == 0U) {
                CHECK(network_test_backend_transmit_count() == 0U);
                CHECK(state(&peer, TCP_STATE_FAILED, TCP_ERR_TRANSMIT_TIMEOUT, 0U, 0U));
                CHECK(tcp_recv(peer.handle, &readback, 1U, 0U) == TCP_ERR_TRANSMIT_TIMEOUT);
            } else {
                CHECK(net_elapsed_ms(tcp_global_context.connection.transmit_started_ms,
                                     network_test_backend_transmit_time(0U)) == elapsed);
                CHECK(take(&peer, &gateway, (struct wire_segment){
                    .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
                    .flags = ACK | PSH, .data = &data, .length = 1U
                }));
                CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - 1U));
            }
            CHECK(dispose(&peer, 1));
        }
    }
    return 1;
}

static int transmit_progress_deadline(void)
{
    static const unsigned char data[] = {9U, 0U, 8U};
    struct tcp_config config;
    struct peer peer;
    struct wire_segment ack;
    unsigned char readback = 0xffU;

    CHECK(tcp_config_defaults(&config) == 0);
    config.transmit_timeout_ms = 20U;
    CHECK(reset(0U, network.arp_cache_ttl_ms) &&
          establish_config(&peer, 31U, 0U, 64U, &config));
    network_test_backend_clock_advance(100U); /* No pending bytes: no deadline. */
    CHECK(net_poll(0U) == 0);
    CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == (int)sizeof(data));
    network_test_backend_clock_advance(10U);
    CHECK(tcp_send(peer.handle, data, 1U, 0U) == 1);
    ack = (struct wire_segment){.seq = peer.first_receive, .ack = peer.first_send, .flags = ACK};
    network_test_backend_clock_advance(9U);
    CHECK(receive_later(&peer, ack, 0U) && net_poll(0U) == 0);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - 4U));
    network_test_backend_clock_advance(1U);
    CHECK(net_poll(0U) == 1 && network_test_backend_transmit_count() == 0U);
    CHECK(state(&peer, TCP_STATE_FAILED, TCP_ERR_TRANSMIT_TIMEOUT, 0U, 0U));
    CHECK(tcp_recv(peer.handle, &readback, 1U, 0U) == TCP_ERR_TRANSMIT_TIMEOUT);
    CHECK(dispose(&peer, 1));

    CHECK(reset(0U, network.arp_cache_ttl_ms) &&
          establish_config(&peer, 31U, 64U, 64U, &config));
    CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == (int)sizeof(data));
    CHECK(take(&peer, &gateway, (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK | PSH, .data = data, .length = sizeof(data)
    }));
    network_test_backend_clock_advance(15U);
    ack = (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send + 1U, .window = 64U, .flags = ACK
    };
    CHECK(receive_later(&peer, ack, 0U) && net_poll(0U) == 1);
    network_test_backend_clock_advance(19U);
    CHECK(net_poll(0U) == 0);
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - 2U));
    /* The receive itself crosses the new no-progress deadline. */
    ack.ack = peer.first_send + sizeof(data);
    CHECK(receive_later(&peer, ack, 2U));
    network_test_backend_set_receive_clock_step(2U);
    CHECK(net_poll(0U) == 1 && net_clock_now_ms() == 36U);
    CHECK(state(&peer, TCP_STATE_FAILED, TCP_ERR_TRANSMIT_TIMEOUT, 0U, 0U));
    CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == TCP_ERR_TRANSMIT_TIMEOUT);
    CHECK(dispose(&peer, 1));

    CHECK(reset(0U, 1000U) && establish_config(&peer, 31U, 64U, 64U, &config));
    network_test_backend_clock_advance(1000U);
    CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == (int)sizeof(data));
    CHECK(take_arp_request()); /* No first data transmission yet. */
    CHECK(net_global_context.pending_arp.owner == peer.handle);
    network_test_backend_clock_advance(20U);
    CHECK(net_poll(0U) == 1 && network_test_backend_transmit_count() == 0U);
    CHECK(state(&peer, TCP_STATE_FAILED, TCP_ERR_TRANSMIT_TIMEOUT, 0U, 0U));
    return dispose(&peer, 1);
}

static int reset_challenge_rules(void)
{
    static const unsigned char queued[70] = {0};
    static const unsigned char incoming_bytes[] = {0xffU, 0U};
    struct peer peer;
    struct wire_segment reply, reset_packet;

    CHECK(reset(UINT_MAX - 249U, network.arp_cache_ttl_ms) &&
          establish(&peer, UINT_MAX - 4U, 64U, 64U));
    CHECK(tcp_send(peer.handle, queued, sizeof(queued), 0U) == (int)sizeof(queued));
    CHECK(take(&peer, &gateway, (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK, .data = queued, .length = 64U
    }));
    CHECK(receive_later(&peer, (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send, .window = 64U,
        .flags = ACK, .data = incoming_bytes, .length = sizeof(incoming_bytes)
    }, 0U) && net_poll(0U) == 1);
    reply = (struct wire_segment){
        .seq = peer.first_send + 64U,
        .ack = peer.first_receive + sizeof(incoming_bytes),
        .window = TCP_QUEUE_CAPACITY - sizeof(incoming_bytes), .flags = ACK
    };
    CHECK(take(&peer, &gateway, reply));

    reset_packet = (struct wire_segment){
        .seq = peer.first_receive + TCP_QUEUE_CAPACITY, .flags = RST
    };
    CHECK(receive_later(&peer, reset_packet, 0U) && net_poll(0U) == 0);
    reset_packet.seq = reply.ack - 1U;
    CHECK(receive_later(&peer, reset_packet, 0U) && net_poll(0U) == 0);
    CHECK(network_test_backend_transmit_count() == 0U);

    /* An abort from the peer can arrive ahead of its last in-flight bytes. */
    reset_packet.seq = reply.ack + 3U;
    CHECK(reset_packet.seq == 1U);
    CHECK(receive_later(&peer, reset_packet, 0U) && net_poll(0U) == 1);
    CHECK(take(&peer, &gateway, reply));
    CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, sizeof(incoming_bytes),
                TCP_QUEUE_CAPACITY - sizeof(queued)));
    network_test_backend_clock_advance(499U);
    CHECK(receive_later(&peer, reset_packet, 0U) && net_poll(0U) == 0);
    CHECK(network_test_backend_transmit_count() == 0U);
    network_test_backend_clock_advance(1U);
    CHECK(receive_later(&peer, reset_packet, 0U) && net_poll(0U) == 1);
    CHECK(take(&peer, &gateway, reply));

    reset_packet.seq = reply.ack; /* Exact reset takes effect even during the limit. */
    CHECK(receive_later(&peer, reset_packet, 0U) && net_poll(0U) == 1);
    CHECK(network_test_backend_transmit_count() == 0U);
    CHECK(state(&peer, TCP_STATE_FAILED, TCP_ERR_RESET, 0U, 0U));
    return dispose(&peer, 1);
}

static int reset_challenge_neighbor_refresh(void)
{
    static const unsigned char bytes[] = {1U, 0U, 2U};
    struct peer peer;

    for (int resolved = 0; resolved <= 1; ++resolved) {
        CHECK(reset(0U, 1000U) && establish(&peer, 31U, 0U, 64U));
        CHECK(tcp_send(peer.handle, bytes, sizeof(bytes), 0U) == (int)sizeof(bytes));
        network_test_backend_clock_advance(1000U);
        CHECK(receive_later(&peer, (struct wire_segment){
            .seq = peer.first_receive + 1U, .flags = RST
        }, 0U) && net_poll(0U) == 1);
        CHECK(net_poll(0U) == 1 && take_arp_request());
        if (resolved) {
            /* Normal data becomes sendable while the empty challenge is pending. */
            CHECK(receive_later(&peer, (struct wire_segment){
                .seq = peer.first_receive, .ack = peer.first_send,
                .flags = ACK, .window = 64U
            }, 0U) && net_poll(0U) == 1);
            CHECK(arp_reply(&refreshed) && net_poll(0U) == 1);
            CHECK(net_poll(0U) == 1 && take(&peer, &refreshed, (struct wire_segment){
                .seq = peer.first_send, .ack = peer.first_receive,
                .window = TCP_QUEUE_CAPACITY, .flags = ACK
            }));
            CHECK(net_poll(0U) == 1 && take(&peer, &refreshed, (struct wire_segment){
                .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
                .flags = ACK | PSH, .data = bytes, .length = sizeof(bytes)
            }));
        } else {
            network_test_backend_clock_advance(1000U);
            CHECK(net_poll(0U) == 1 && take_arp_request());
            network_test_backend_clock_advance(1000U);
            CHECK(net_poll(0U) == 1);
            CHECK(!net_global_context.pending_arp.active && net_poll(0U) == 0);
        }
        CHECK(network_test_backend_transmit_count() == 0U);
        CHECK(state(&peer, TCP_STATE_ESTABLISHED, 0, 0U, TCP_QUEUE_CAPACITY - sizeof(bytes)));
        CHECK(dispose(&peer, 1));
    }
    return 1;
}

static int failure_and_reuse(void)
{
    struct peer peer;
    tcp_handle last = TCP_INVALID_HANDLE;
    unsigned char data[] = {1U, 0U, 2U};

    CHECK(reset(0U, 1000U));
    for (unsigned int mode = 0U; mode < 3U; ++mode) {
        int reason = mode == 0U ? NET_ERR_CANCELLED :
                     mode == 1U ? TCP_ERR_RESET : TCP_ERR_NEIGHBOR_TIMEOUT;

        CHECK(establish(&peer, 10U, 512U, 0U) && peer.handle != last);
        if (last != TCP_INVALID_HANDLE) {
            CHECK(tcp_send(last, data, 1U, 0U) == NET_ERR_STATE);
        }
        if (mode == 0U) {
            network_test_backend_cancel_once();
        } else if (mode == 1U) {
            CHECK(receive_later(&peer, (struct wire_segment){
                .seq = peer.first_receive, .flags = RST
            }, 0U));
        } else {
            network_test_backend_clock_advance(1000U);
        }
        CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == (int)sizeof(data));
        if (mode == 2U) {
            CHECK(net_global_context.pending_arp.active && take_arp_request());
            network_test_backend_clock_advance(1000U);
            CHECK(net_poll(0U) == 1 && take_arp_request());
            network_test_backend_clock_advance(1000U);
            CHECK(net_poll(0U) == 1);
        }
        CHECK(state(&peer, TCP_STATE_FAILED, reason, 0U, 0U));
        CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == reason);
        CHECK(tcp_recv(peer.handle, data, sizeof(data), 0U) == reason);
        CHECK(!net_global_context.pending_arp.active);
        for (unsigned int index = 0U; index < TCP_QUEUE_CAPACITY; ++index) {
            CHECK(tcp_global_context.connection.tx_queue[index] == 0U &&
                  tcp_global_context.connection.rx_queue[index] == 0U);
        }
        last = peer.handle;
        CHECK(dispose(&peer, 0));
        network_test_backend_clear_transmits();
    }
    CHECK(tcp_deinit() == 0);
    return 1;
}

static int read_progress_then_device_failure(void)
{
    static const unsigned char data[] = {1U, 0U, 2U, 3U};
    unsigned char output[] = {0xffU, 0xffU, 0xffU, 0xffU};
    struct peer peer;
    tcp_handle old;

    CHECK(reset(0U, network.arp_cache_ttl_ms) && establish(&peer, 29U, 64U, 64U));
    CHECK(receive_later(&peer, (struct wire_segment){
        .seq = peer.first_receive, .ack = peer.first_send, .window = 64U,
        .flags = ACK, .data = data, .length = sizeof(data)
    }, 0U) && net_poll(0U) == 1);
    CHECK(take(&peer, &gateway, (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive + sizeof(data),
        .window = TCP_QUEUE_CAPACITY - sizeof(data), .flags = ACK
    }));

    /* The read has already copied bytes when the window update fails to send. */
    CHECK(network_test_backend_set_next_send_error(SYS_ERR_DEVICE) == 0);
    CHECK(tcp_recv(peer.handle, output, 2U, 0U) == 2);
    CHECK(memcmp(output, data, 2U) == 0 && output[2] == 0xffU && output[3] == 0xffU);
    CHECK(network_test_backend_transmit_count() == 0U);
    CHECK(state(&peer, TCP_STATE_FAILED, NET_ERR_DEVICE, 0U, 0U));
    CHECK(tcp_recv(peer.handle, output + 2U, 2U, 0U) == NET_ERR_DEVICE);
    CHECK(output[2] == 0xffU && output[3] == 0xffU);
    old = peer.handle;
    CHECK(dispose(&peer, 0));

    CHECK(establish(&peer, 31U, 64U, 64U) && peer.handle != old);
    CHECK(tcp_recv(old, output, sizeof(output), 0U) == NET_ERR_STATE);
    CHECK(tcp_recv(peer.handle, output, sizeof(output), 0U) == TCP_ERR_WOULD_BLOCK);
    CHECK(tcp_send(peer.handle, data, sizeof(data), 0U) == (int)sizeof(data));
    CHECK(take(&peer, &gateway, (struct wire_segment){
        .seq = peer.first_send, .ack = peer.first_receive, .window = TCP_QUEUE_CAPACITY,
        .flags = ACK | PSH, .data = data, .length = sizeof(data)
    }));
    return dispose(&peer, 1);
}

int test_tcp_stream(void)
{
    if (!ring_boundary() || !transmit_boundaries() || !receive_boundaries() ||
        !wait_services_both_directions() || !wait_expiry_retains_progress() ||
        !started_io_can_finish() || !deferred_raw_send_preserves_timers() ||
        !wait_expiry_defers_control_replies() ||
        !duplex_bytes() || !neighbor_during_stream() || !reset_challenge_rules() ||
        !reset_challenge_neighbor_refresh() || !failure_and_reuse() ||
        !read_progress_then_device_failure()) {
        return 0;
    }
    puts("E4 bounded TCP queues, duplex byte streams and flow control: PASS");
    if (!retry_of_timed_prefix() || !retry_after_neighbor_refresh() ||
        !ack_releases_idle_neighbor() || !retry_exhaustion(RETRY_NO_ACK) ||
        !retry_exhaustion(RETRY_DUPLICATE_ACK) || !retry_exhaustion(RETRY_ADVANCING_ACK) ||
        !retry_exhaustion(RETRY_CLOSED_WINDOW) || !retry_exhaustion(RETRY_HARD_DEADLINE) ||
        !raw_send_obeys_transmit_deadline() || !transmit_progress_deadline()) {
        return 0;
    }
    puts("E5a TCP retransmission, ARP resumption and transmit deadlines: PASS");
    return 1;
}
