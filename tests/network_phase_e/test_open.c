#include "../network_common/deterministic_backend.h"

#include "../../transport/lib/crypto/hmac_sha256.h"
#include "../../transport/lib/net/arp.h"
#include "../../transport/lib/net/net_platform.h"
#include "../../transport/lib/net/tcp/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "E3 active open test failure at line %d: %s\n", \
            __LINE__, #condition); return 0; } } while (0)

static const struct net_config net_config = {
    {{10U, 0U, 2U, 15U}}, {{255U, 255U, 255U, 0U}}, {{10U, 0U, 2U, 2U}},
    60000U, 1000U, 2U
};
static const struct tcp_endpoint local_peer = {{{10U, 0U, 2U, 2U}}, 22U};
static const struct tcp_endpoint remote_peer = {{{192U, 0U, 2U, 9U}}, 22U};
static const struct net_mac_addr peer_mac = {{2U, 0U, 0U, 0U, 0U, 1U}};
static const struct net_mac_addr refreshed_mac = {{2U, 0U, 0U, 0U, 0U, 9U}};

static struct allocation {
    void *memory;
    size_t bytes;
} allocations[2];
static unsigned int allocation_calls;
static unsigned int fail_allocation;
static size_t live_bytes;
static size_t peak_bytes;
static int cleared_allocations;

static int is_zero(const void *memory, size_t length)
{
    const unsigned char *bytes = memory;
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] != 0U) {
            return 0;
        }
    }
    return 1;
}

void *test_hmac_calloc(size_t count, size_t size)
{
    if (++allocation_calls == fail_allocation) {
        return 0;
    }
    for (unsigned int i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
        if (allocations[i].memory == 0) {
            void *memory = calloc(count, size);
            if (memory != 0) {
                allocations[i] = (struct allocation){memory, count * size};
                live_bytes += count * size;
                if (live_bytes > peak_bytes) {
                    peak_bytes = live_bytes;
                }
            }
            return memory;
        }
    }
    abort();
}

void test_hmac_free(void *memory)
{
    if (memory == 0) {
        return;
    }
    for (unsigned int i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
        if (allocations[i].memory == memory) {
            cleared_allocations &= is_zero(memory, allocations[i].bytes);
            live_bytes -= allocations[i].bytes;
            allocations[i] = (struct allocation){0};
            free(memory);
            return;
        }
    }
    abort();
}

static int reset_allocations(unsigned int fail_at)
{
    CHECK(live_bytes == 0U);
    allocation_calls = 0U;
    fail_allocation = fail_at;
    peak_bytes = 0;
    cleared_allocations = 1;
    return 1;
}

static int initialize(const struct net_config *config)
{
    CHECK(reset_allocations(0U));
    network_test_backend_reset();
    network_test_backend_random_seed(0x12345678U);
    memset(&net_global_context, 0, sizeof(net_global_context));
    memset(&tcp_global_context, 0, sizeof(tcp_global_context));
    CHECK(net_init(config) == 0);
    CHECK(network_test_backend_random_call_count() == 0U);
    return 1;
}

static int test_hmac(void)
{
    static const unsigned char input[] = "Hi There";
    /* RFC 4231 test case 1. */
    static const unsigned char expected[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };
    unsigned char key[20];
    unsigned char output[32];

    CHECK(reset_allocations(0U));
    memset(key, 0x0b, sizeof(key));
    CHECK(crypto_hmac_sha256(key, sizeof(key), input, sizeof(input) - 1U, output) == 0);
    CHECK(memcmp(output, expected, sizeof(output)) == 0);
    CHECK(allocation_calls == 2U && peak_bytes <= 2048U && live_bytes == 0U &&
          cleared_allocations);
    printf("HMAC temporary heap requests peak at %lu bytes on host.\n",
           (unsigned long)peak_bytes);
    CHECK(crypto_hmac_sha256(0, sizeof(key), input, sizeof(input) - 1U, output) ==
          CRYPTO_HMAC_FAILED);
    CHECK(is_zero(output, sizeof(output)));
    for (unsigned int step = 1U; step <= 2U; ++step) {
        CHECK(reset_allocations(step));
        memset(output, 0xa5, sizeof(output));
        CHECK(crypto_hmac_sha256(key, sizeof(key), input, sizeof(input) - 1U, output) ==
              CRYPTO_HMAC_NO_MEMORY);
        CHECK(allocation_calls == step && live_bytes == 0U &&
              cleared_allocations && is_zero(output, sizeof(output)));
    }
    return 1;
}

static int unpublished(tcp_handle handle)
{
    CHECK(handle == TCP_INVALID_HANDLE && tcp_global_context.last_generation == 0U);
    CHECK(net_global_context.transport.owner == 0 && !net_global_context.pending_arp.active);
    CHECK(!net_global_context.service_active && network_test_backend_transmit_count() == 0U);
    return 1;
}

static int test_preflight_failures(void)
{
    const int entropy_errors[] = {SYS_ERR_UNAVAILABLE, 0, 31};
    tcp_handle handle;

    for (unsigned int i = 0; i < sizeof(entropy_errors) / sizeof(entropy_errors[0]); ++i) {
        CHECK(initialize(&net_config));
        handle = TCP_INVALID_HANDLE;
        CHECK(network_test_backend_set_next_random_result(entropy_errors[i]) == 0);
        CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_ENTROPY);
        CHECK(unpublished(handle));
        CHECK(!tcp_global_context.secret_ready && allocation_calls == 0U);
        CHECK(is_zero(tcp_global_context.instance_secret, TCP_INSTANCE_SECRET_BYTES));
    }

    for (unsigned int step = 1U; step <= 2U; ++step) {
        unsigned char secret[TCP_INSTANCE_SECRET_BYTES];
        CHECK(initialize(&net_config) && reset_allocations(step));
        handle = TCP_INVALID_HANDLE;
        CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_NO_MEMORY);
        CHECK(unpublished(handle));
        CHECK(allocation_calls == step && live_bytes == 0U && cleared_allocations);
        CHECK(network_test_backend_random_call_count() == 2U && tcp_global_context.secret_ready);
        memcpy(secret, tcp_global_context.instance_secret, sizeof(secret));
        CHECK(network_test_backend_set_next_random_result(1) == 0);
        CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_ENTROPY);
        CHECK(unpublished(handle) && allocation_calls == step);
        CHECK(memcmp(secret, tcp_global_context.instance_secret, sizeof(secret)) == 0);
        CHECK(tcp_deinit() == 0);
        CHECK(is_zero(tcp_global_context.instance_secret, sizeof(secret)));
    }

    CHECK(initialize(&net_config));
    handle = TCP_INVALID_HANDLE;
    network_test_backend_cancel_once();
    CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == NET_ERR_CANCELLED);
    CHECK(unpublished(handle) && network_test_backend_random_call_count() == 0U);
    tcp_global_context.last_generation = UINT_MAX;
    CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_GENERATION_EXHAUSTED);
    CHECK(handle == TCP_INVALID_HANDLE && network_test_backend_random_call_count() == 0U);
    return 1;
}

/* Independent wire builder and checksum oracle. */
static void word(unsigned char *bytes, unsigned int value)
{
    bytes[0] = (unsigned char)(value >> 8);
    bytes[1] = (unsigned char)value;
}

static void longword(unsigned char *bytes, unsigned int value)
{
    word(bytes, value >> 16);
    word(bytes + 2, value);
}

static unsigned int checksum(const unsigned char *bytes, size_t length,
                             unsigned int sum)
{
    for (size_t i = 0; i < length; ++i) {
        sum += (unsigned int)bytes[i] << (i % 2U == 0U ? 8U : 0U);
    }
    while (sum > 0xffffU) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return sum ^ 0xffffU;
}

struct control {
    unsigned int source_port, destination_port, sequence, ack, flags, window, mss;
};

static unsigned char *ipv4_frame(unsigned char frame[NET_FRAME_MIN], int incoming,
                                 const struct net_ipv4_addr *peer,
                                 const struct net_mac_addr *mac, unsigned char protocol,
                                 unsigned int length)
{
    const unsigned char *local = net_global_context.device_mac.octets;
    unsigned char *ip = frame + 14;

    memset(frame, 0, NET_FRAME_MIN);
    memcpy(frame, incoming ? local : mac->octets, 6);
    memcpy(frame + 6, incoming ? mac->octets : local, 6);
    word(frame + 12, 0x0800U);
    ip[0] = 0x45U;
    word(ip + 2, 20U + length);
    word(ip + 6, 0x4000U);
    ip[8] = 64U;
    ip[9] = protocol;
    memcpy(ip + 12, incoming ? peer->octets : net_config.address.octets, 4);
    memcpy(ip + 16, incoming ? net_config.address.octets : peer->octets, 4);
    word(ip + 10, checksum(ip, 20U, 0U));
    return ip + 20;
}

static void control_frame(unsigned char frame[NET_FRAME_MIN], int incoming,
                          const struct tcp_endpoint *endpoint,
                          const struct net_mac_addr *mac, struct control control,
                          const unsigned char *data, unsigned int data_length)
{
    unsigned int header_length = control.mss != 0U ? 24U : 20U;
    unsigned int length = header_length + data_length;
    unsigned char *tcp = ipv4_frame(frame, incoming, &endpoint->address, mac, 6U, length);
    unsigned int addresses;

    word(tcp, control.source_port);
    word(tcp + 2, control.destination_port);
    longword(tcp + 4, control.sequence);
    longword(tcp + 8, control.ack);
    tcp[12] = (unsigned char)(header_length << 2U);
    tcp[13] = (unsigned char)control.flags;
    word(tcp + 14, control.window);
    if (control.mss != 0U) {
        tcp[20] = 2;
        tcp[21] = 4;
        word(tcp + 22, control.mss);
    }
    if (data_length != 0U) {
        memcpy(tcp + header_length, data, data_length);
    }
    addresses = checksum(tcp - 8, 8U, 0U) ^ 0xffffU;
    word(tcp + 16, checksum(tcp, length, addresses + 6U + length));
}

static int expect_control(unsigned int index, const struct tcp_endpoint *endpoint,
                          const struct net_mac_addr *mac, struct control control)
{
    unsigned char frame[NET_FRAME_MIN];

    control_frame(frame, 0, endpoint, mac, control, 0, 0U);
    CHECK(network_test_backend_transmit_length(index) == sizeof(frame));
    CHECK(memcmp(network_test_backend_transmit_frame(index), frame, sizeof(frame)) == 0);
    return 1;
}

static int queue_control(const struct tcp_endpoint *endpoint,
                         const struct net_mac_addr *mac, struct control control)
{
    unsigned char frame[NET_FRAME_MIN];

    control_frame(frame, 1, endpoint, mac, control, 0, 0U);
    return network_test_backend_queue_receive(frame, sizeof(frame)) == 0;
}

static int queue_window_probe(struct control control)
{
    unsigned char frame[NET_FRAME_MIN];
    const unsigned char byte = 0x5AU;

    control_frame(frame, 1, &local_peer, &peer_mac, control, &byte, 1U);
    return network_test_backend_queue_receive(frame, sizeof(frame)) == 0;
}

static void arp_reply(unsigned char frame[NET_FRAME_MIN], const struct net_mac_addr *mac)
{
    static const unsigned char arp_prefix[] = {0x08, 0x06, 0, 1, 8, 0, 6, 4, 0, 2};
    const unsigned char *local = net_global_context.device_mac.octets;

    memset(frame, 0, NET_FRAME_MIN);
    memcpy(frame, local, 6);
    memcpy(frame + 6, mac->octets, 6);
    memcpy(frame + 12, arp_prefix, sizeof(arp_prefix));
    memcpy(frame + 22, mac->octets, 6);
    memcpy(frame + 28, net_config.gateway.octets, 4);
    memcpy(frame + 32, local, 6);
    memcpy(frame + 38, net_config.address.octets, 4);
}

static int status_is(tcp_handle handle, enum tcp_state state, int terminal)
{
    struct tcp_status status;

    CHECK(tcp_status(handle, &status) == 0 && status.state == state);
    CHECK(status.terminal_result == terminal);
    CHECK(status.readable_bytes == 0U && status.writable_bytes == 0U);
    return 1;
}

static int dispose(tcp_handle handle)
{
    CHECK(tcp_abort(handle) == 0 && tcp_release(handle) == 0);
    CHECK(!net_global_context.pending_arp.active && !net_global_context.service_active);
    CHECK(tcp_deinit() == 0);
    return 1;
}

static int test_handshake(void)
{
    const unsigned int port = 55097U, iss = 0x8a313876U, irs = UINT_MAX;
    struct control reply = {22U, port, irs, iss + 1U, 0x12U, 2048U, 600U};
    struct control ack = {port, 22U, iss + 1U, 0U, 0x10U, 0U, 0U};
    tcp_handle handle = TCP_INVALID_HANDLE;
    struct tcp_active_open *open = &tcp_global_context.connection.open;
    unsigned char frame[NET_FRAME_MIN];
    unsigned char byte = 0xa5;

    CHECK(initialize(&net_config));
    CHECK(tcp_connect(&handle, &remote_peer, 0, 0U) == TCP_ERR_WOULD_BLOCK);
    CHECK(status_is(handle, TCP_STATE_CONNECTING, 0));
    CHECK(net_global_context.pending_arp.owner == handle);
    CHECK(network_test_backend_transmit_count() == 1U);
    CHECK(memcmp(network_test_backend_transmit_frame(0) + 38,
                 net_config.gateway.octets, 4) == 0);
    CHECK(network_test_backend_transmit_frame(0)[13] == 0x06U);
    CHECK(tcp_close(handle, 0U) == NET_ERR_STATE && tcp_shutdown_write(handle) == NET_ERR_STATE);

    network_test_backend_set_receive_clock_step(1U);
    CHECK(tcp_connect(&handle, &remote_peer, 0, 4U) == NET_ERR_TIMEOUT);
    CHECK(status_is(handle, TCP_STATE_CONNECTING, 0) && open->syn_transmissions == 0U);
    CHECK(net_global_context.pending_arp.started_ms == 0U);
    CHECK(tcp_global_context.connection.started_ms == 0U);
    CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == NET_ERR_INVALID);
    arp_reply(frame, &peer_mac);
    /* The receive completes exactly as this short call budget expires. */
    CHECK(network_test_backend_schedule_receive(frame, sizeof(frame), 1U) == 0);
    CHECK(net_poll(1U) == 1 && net_global_context.pending_arp.resolved);
    network_test_backend_set_receive_clock_step(0U);
    CHECK(tcp_connect(&handle, &remote_peer, 0, 0U) == TCP_ERR_WOULD_BLOCK);
    CHECK(expect_control(1, &remote_peer, &peer_mac,
                        (struct control){port, 22U, iss, 0U, 2U, 0U, 536U}));
    CHECK(network_test_backend_random_call_count() == 2U && allocation_calls == 2U);
    CHECK(!net_global_context.pending_arp.active && live_bytes == 0U && cleared_allocations);

    ++reply.source_port;
    CHECK(queue_control(&remote_peer, &refreshed_mac, reply) && net_poll(0U) == 0);
    --reply.source_port;
    --reply.ack;
    CHECK(queue_control(&remote_peer, &refreshed_mac, reply) && net_poll(0U) == 1);
    CHECK(expect_control(2, &remote_peer, &peer_mac,
                        (struct control){port, 22U, iss, 0U, 4U, 0U, 0U}));
    ++reply.ack;
    CHECK(status_is(handle, TCP_STATE_CONNECTING, 0));
    CHECK(queue_control(&remote_peer, &refreshed_mac, reply));
    CHECK(tcp_connect(&handle, &remote_peer, 0, 5U) == 0);
    CHECK(expect_control(3, &remote_peer, &peer_mac, ack));
    CHECK(status_is(handle, TCP_STATE_ESTABLISHED, 0));
    CHECK(open->peer_mss == 600U && open->peer_window == 2048U && open->irs == irs);
    CHECK(tcp_connect(&handle, &remote_peer, 0, 0U) == 0);
    CHECK(tcp_send(handle, &byte, 1U, 0U) == TCP_ERR_UNSUPPORTED);
    CHECK(tcp_recv(handle, &byte, 1U, 0U) == TCP_ERR_UNSUPPORTED && byte == 0xa5U);
    CHECK(tcp_close(handle, 0U) == TCP_ERR_UNSUPPORTED);
    CHECK(tcp_shutdown_write(handle) == TCP_ERR_UNSUPPORTED);
    CHECK(network_test_backend_transmit_count() == 4U);
    /* The peer retries its SYN-ACK after losing the final ACK. */
    CHECK(queue_control(&remote_peer, &refreshed_mac, reply) && net_poll(0U) == 1);
    CHECK(expect_control(4, &remote_peer, &peer_mac, ack));
    CHECK(open->syn_transmissions == 1U && status_is(handle, TCP_STATE_ESTABLISHED, 0));
    return dispose(handle);
}

static int start_cached(tcp_handle *handle, const struct tcp_config *config)
{
    CHECK(net_arp_cache_store(&net_global_context, &local_peer.address, &peer_mac) == 0);
    CHECK(tcp_connect(handle, &local_peer, config, 0U) == TCP_ERR_WOULD_BLOCK);
    CHECK(network_test_backend_transmit_count() == 1U &&
          tcp_global_context.connection.open.syn_transmissions == 1U);
    return 1;
}

static int test_clock_and_generation(void)
{
    const unsigned int port = 55097U, initial_iss = 0xb26b786aU;
    tcp_handle handle = TCP_INVALID_HANDLE;
    tcp_handle next = TCP_INVALID_HANDLE;
    unsigned char secret[TCP_INSTANCE_SECRET_BYTES];

    CHECK(initialize(&net_config) && start_cached(&handle, 0));
    CHECK(expect_control(0, &local_peer, &peer_mac,
                        (struct control){port, 22U, initial_iss, 0U, 2U, 0U, 536U}));
    memcpy(secret, tcp_global_context.instance_secret, sizeof(secret));
    CHECK(tcp_abort(handle) == 0);
    CHECK(tcp_connect(&next, &local_peer, 0, 0U) == NET_ERR_BUSY && next == TCP_INVALID_HANDLE);
    CHECK(tcp_release(handle) == 0);
    network_test_backend_random_seed(0xdb768f18U); /* Reproduce the same port at the same tick. */
    CHECK(tcp_connect(&next, &local_peer, 0, 0U) == TCP_ERR_WOULD_BLOCK && next != handle);
    CHECK(expect_control(1, &local_peer, &peer_mac,
                        (struct control){port, 22U, initial_iss + 1U, 0U, 2U, 0U, 536U}));
    CHECK(network_test_backend_random_call_count() == 3U);
    CHECK(memcmp(secret, tcp_global_context.instance_secret, sizeof(secret)) == 0);
    CHECK(tcp_abort(handle) == NET_ERR_STATE);
    CHECK(dispose(next));
    CHECK(is_zero(tcp_global_context.instance_secret, sizeof(secret)));
    return 1;
}

static int test_loss_and_neighbor_refresh(void)
{
    struct net_config config = net_config;
    struct tcp_active_open *open = &tcp_global_context.connection.open;
    tcp_handle handle = TCP_INVALID_HANDLE;
    struct control syn, reply;
    unsigned char frame[NET_FRAME_MIN];

    config.arp_cache_ttl_ms = 1000U;
    CHECK(initialize(&config));
    network_test_backend_clock_set(UINT_MAX - 500U);
    CHECK(start_cached(&handle, 0));
    syn = (struct control){open->local_port, 22U, open->iss, 0U, 2U, 0U, 536U};
    network_test_backend_clock_advance(999U);
    CHECK(net_poll(0U) == 0 && network_test_backend_transmit_count() == 1U);
    network_test_backend_clock_advance(1U);
    CHECK(net_poll(0U) == 1 && network_test_backend_transmit_count() == 2U);
    CHECK(network_test_backend_transmit_frame(1)[13] == 0x06U);
    CHECK(open->syn_transmissions == 1U && net_global_context.pending_arp.active);
    arp_reply(frame, &refreshed_mac);
    CHECK(network_test_backend_schedule_receive(frame, sizeof(frame), 3U) == 0);
    CHECK(net_poll(0U) == 0 && open->syn_transmissions == 1U);
    network_test_backend_clock_advance(3U);
    CHECK(net_poll(0U) == 1 && net_poll(0U) == 1);
    CHECK(expect_control(2, &local_peer, &refreshed_mac, syn));
    CHECK(open->syn_transmissions == 2U && open->syn_rto_ms == 2000U);
    CHECK(network_test_backend_random_call_count() == 2U);

    reply = (struct control){22U, syn.source_port, 17U, syn.sequence + 1U, 0x12U, 0U, 0U};
    CHECK(queue_control(&local_peer, &refreshed_mac, reply) && net_poll(0U) == 1);
    CHECK(status_is(handle, TCP_STATE_ESTABLISHED, 0) && open->peer_mss == 536U);
    reply.flags = 4U;
    CHECK(queue_control(&local_peer, &peer_mac, reply) && net_poll(0U) == 0);
    ++reply.sequence;
    CHECK(queue_control(&local_peer, &peer_mac, reply) && net_poll(0U) == 1);
    CHECK(status_is(handle, TCP_STATE_FAILED, TCP_ERR_RESET));
    return dispose(handle);
}

static int test_handshake_during_refresh(void)
{
    struct net_config config = net_config;
    struct tcp_active_open *open = &tcp_global_context.connection.open;
    unsigned char frame[NET_FRAME_MIN];
    tcp_handle handle;

    config.arp_cache_ttl_ms = 1000U;
    for (int reply = 0; reply <= 1; ++reply) {
        struct control syn_ack;

        handle = TCP_INVALID_HANDLE;
        CHECK(initialize(&config) && start_cached(&handle, 0));
        network_test_backend_clock_advance(1000U);
        CHECK(net_poll(0U) == 1 && network_test_backend_transmit_count() == 2U);
        CHECK(net_global_context.pending_arp.owner == handle &&
              net_global_context.pending_arp.attempts == 1U &&
              open->syn_transmissions == 1U);
        if (reply) {
            syn_ack = (struct control){
                22U, open->local_port, 19U, open->iss + 1U, 0x12U, 128U, 0U
            };
            CHECK(queue_control(&local_peer, &peer_mac, syn_ack));
            CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_WOULD_BLOCK);
            CHECK(status_is(handle, TCP_STATE_CONNECTING, 0));
            CHECK(open->ack_pending && network_test_backend_transmit_count() == 2U);
            CHECK(queue_control(&local_peer, &peer_mac, syn_ack) && net_poll(0U) == 0);
            CHECK(open->syn_transmissions == 1U && net_global_context.pending_arp.attempts == 1U);
            arp_reply(frame, &refreshed_mac);
            CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
            CHECK(net_poll(0U) == 1 && status_is(handle, TCP_STATE_CONNECTING, 0));
            CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == 0);
            CHECK(status_is(handle, TCP_STATE_ESTABLISHED, 0));
            CHECK(expect_control(2, &local_peer, &refreshed_mac,
                                (struct control){open->local_port, 22U,
                                    open->iss + 1U, 20U, 0x10U, 0U, 0U}));
            network_test_backend_clock_advance(3000U);
            CHECK(net_poll(0U) == 0 && network_test_backend_transmit_count() == 3U);
        } else {
            network_test_backend_clock_advance(1000U);
            CHECK(net_poll(0U) == 1 && open->syn_transmissions == 1U);
            network_test_backend_clock_advance(1000U);
            CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_NEIGHBOR_TIMEOUT);
            CHECK(status_is(handle, TCP_STATE_FAILED, TCP_ERR_NEIGHBOR_TIMEOUT));
            CHECK(network_test_backend_transmit_count() == 3U);
        }
        CHECK(!net_global_context.pending_arp.active && dispose(handle));
    }
    return 1;
}

static int test_ack_commit_budget(void)
{
    struct tcp_active_open *open = &tcp_global_context.connection.open;
    struct tcp_config config;

    CHECK(tcp_config_defaults(&config) == 0);
    config.connect_timeout_ms = 5U;
    for (int failed = 0; failed <= 1; ++failed) {
        struct control reply, ack;
        tcp_handle handle = TCP_INVALID_HANDLE;

        CHECK(initialize(&net_config) && start_cached(&handle, &config));
        reply = (struct control){22U, open->local_port, 17U, open->iss + 1U,
                                 0x12U, 16U, 0U};
        ack = (struct control){open->local_port, 22U, open->iss + 1U, 18U, 0x10U, 0U, 0U};
        network_test_backend_clock_advance(4U);
        network_test_backend_set_send_clock_step(2U);
        if (failed) {
            CHECK(network_test_backend_set_next_send_error(SYS_ERR_DEVICE) == 0);
        }
        CHECK(queue_control(&local_peer, &peer_mac, reply));
        CHECK(tcp_connect(&handle, &local_peer, &config, 1U) == (failed ? NET_ERR_DEVICE : 0));
        CHECK(net_clock_now_ms() == 6U);
        if (failed) {
            CHECK(status_is(handle, TCP_STATE_FAILED, NET_ERR_DEVICE));
            CHECK(network_test_backend_transmit_count() == 1U);
        } else {
            CHECK(status_is(handle, TCP_STATE_ESTABLISHED, 0));
            CHECK(network_test_backend_transmit_time(1) == 4U);
            CHECK(expect_control(1, &local_peer, &peer_mac, ack));
            CHECK(net_poll(0U) == 0 && status_is(handle, TCP_STATE_ESTABLISHED, 0));
        }
        CHECK(dispose(handle));
    }
    return 1;
}

static int test_local_failure_during_other_protocol(void)
{
    const struct net_ipv4_addr other_peer = {{10U, 0U, 2U, 3U}};
    const unsigned char payload = 0x58U;
    struct tcp_active_open *open = &tcp_global_context.connection.open;
    struct net_mac_addr resolved;
    struct net_ping_result ping;
    struct tcp_config config;
    tcp_handle handle = TCP_INVALID_HANDLE;
    unsigned char frame[NET_FRAME_MIN];
    unsigned char *echo;

    CHECK(initialize(&net_config) && start_cached(&handle, 0));
    CHECK(net_arp_cache_store(&net_global_context, &other_peer, &refreshed_mac) == 0);
    CHECK(queue_control(&local_peer, &peer_mac,
                        (struct control){22U, open->local_port, 0U, open->iss + 1U,
                                         0x14U, 0U, 0U}));
    echo = ipv4_frame(frame, 1, &other_peer, &refreshed_mac, 1U, 9U);
    word(echo + 4, net_global_context.next_echo_identifier + 1U);
    word(echo + 6, 3U);
    echo[8] = payload;
    word(echo + 2, checksum(echo, 9U, 0U));
    CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
    network_test_backend_set_receive_clock_step(1U);
    CHECK(net_ping(&other_peer, 3U, &payload, 1U, 20U, &ping) == 0);
    CHECK(memcmp(ping.source.octets, other_peer.octets, 4U) == 0 &&
          ping.sequence == 3U && ping.payload_length == 1U);
    CHECK(status_is(handle, TCP_STATE_FAILED, TCP_ERR_REFUSED) && dispose(handle));

    handle = TCP_INVALID_HANDLE;
    CHECK(initialize(&net_config) && tcp_config_defaults(&config) == 0);
    config.connect_timeout_ms = 5U;
    CHECK(start_cached(&handle, &config));
    arp_reply(frame, &refreshed_mac);
    memcpy(frame + 28, other_peer.octets, 4U);
    CHECK(network_test_backend_schedule_receive(frame, sizeof(frame), 6U) == 0);
    network_test_backend_set_receive_clock_step(5U);
    CHECK(net_resolve_arp(&other_peer, &resolved, 20U) == 0);
    CHECK(memcmp(resolved.octets, refreshed_mac.octets, 6U) == 0);
    CHECK(status_is(handle, TCP_STATE_FAILED, TCP_ERR_CONNECT_TIMEOUT));
    CHECK(dispose(handle));
    return 1;
}

static int test_receive_recovery(void)
{
    struct tcp_active_open *open = &tcp_global_context.connection.open;
    tcp_handle handle = TCP_INVALID_HANDLE;
    struct control syn, reply;

    CHECK(initialize(&net_config) && start_cached(&handle, 0));
    syn = (struct control){open->local_port, 22U, open->iss, 0U, 2U, 0U, 536U};
    CHECK(network_test_backend_queue_receive_error(SYS_ERR_OVERRUN) == 0);
    CHECK(net_poll(0U) == 0 && status_is(handle, TCP_STATE_CONNECTING, 0));
    CHECK(network_test_backend_queue_receive_error(SYS_ERR_TIMEOUT) == 0);
    CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_WOULD_BLOCK);
    CHECK(network_test_backend_transmit_count() == 1U);
    network_test_backend_clock_advance(1000U);
    CHECK(net_poll(0U) == 1 && expect_control(1, &local_peer, &peer_mac, syn));
    reply = (struct control){22U, open->local_port, 17U, open->iss + 1U, 0x12U, 16U, 0U};
    CHECK(queue_control(&local_peer, &peer_mac, reply) && net_poll(0U) == 1);
    CHECK(status_is(handle, TCP_STATE_ESTABLISHED, 0));
    CHECK(network_test_backend_queue_receive_error(SYS_ERR_RESET) == 0);
    CHECK(net_poll(0U) == NET_ERR_DEVICE && status_is(handle, TCP_STATE_FAILED, NET_ERR_DEVICE));
    return dispose(handle);
}

static int test_zero_window_ack(void)
{
    struct net_config network = net_config;
    struct tcp_config config;
    struct tcp_active_open *open = &tcp_global_context.connection.open;
    tcp_handle handle = TCP_INVALID_HANDLE;
    struct control peer_ack, local_ack;
    unsigned char frame[NET_FRAME_MIN];

    network.arp_cache_ttl_ms = 1000U;
    CHECK(initialize(&network) && tcp_config_defaults(&config) == 0);
    config.connect_timeout_ms = 5U;
    CHECK(start_cached(&handle, &config));
    peer_ack = (struct control){22U, open->local_port, UINT_MAX, open->iss + 1U, 0x12U, 16U, 0U};
    local_ack = (struct control){open->local_port, 22U, open->iss + 1U, 0U, 0x10U, 0U, 0U};
    CHECK(queue_control(&local_peer, &peer_mac, peer_ack) && net_poll(0U) == 1);
    CHECK(status_is(handle, TCP_STATE_ESTABLISHED, 0));
    peer_ack.flags = 0x10U;
    peer_ack.sequence = 0U;
    CHECK(queue_control(&local_peer, &peer_mac, peer_ack) && net_poll(0U) == 0);
    CHECK(network_test_backend_transmit_count() == 2U);
    CHECK(queue_window_probe(peer_ack) && net_poll(0U) == 1);
    CHECK(expect_control(2, &local_peer, &peer_mac, local_ack));
    peer_ack.sequence = UINT_MAX; /* Zero-length keepalive just before RCV.NXT. */
    CHECK(queue_control(&local_peer, &peer_mac, peer_ack) && net_poll(0U) == 1);
    CHECK(expect_control(3, &local_peer, &peer_mac, local_ack));

    network_test_backend_clock_advance(1000U);
    peer_ack.sequence = 0U;
    CHECK(queue_window_probe(peer_ack) && net_poll(0U) == 1);
    CHECK(open->ack_pending && net_global_context.pending_arp.owner == handle);
    CHECK(network_test_backend_transmit_count() == 4U);
    CHECK(net_poll(0U) == 1 && network_test_backend_transmit_frame(4)[13] == 0x06U);
    network_test_backend_clock_advance(1U);
    CHECK(queue_window_probe(peer_ack) && net_poll(0U) == 0);
    CHECK(open->ack_started_ms == 1000U && net_global_context.pending_arp.attempts == 1U);
    arp_reply(frame, &refreshed_mac);
    CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
    CHECK(net_poll(0U) == 1 && net_poll(0U) == 1);
    CHECK(expect_control(5, &local_peer, &refreshed_mac, local_ack));
    CHECK(!open->ack_pending && !net_global_context.pending_arp.active);
    CHECK(status_is(handle, TCP_STATE_ESTABLISHED, 0));

    network_test_backend_clock_advance(1000U);
    CHECK(queue_window_probe(peer_ack) && net_poll(0U) == 1);
    CHECK(net_poll(0U) == 1);
    network_test_backend_clock_advance(1000U);
    CHECK(net_poll(0U) == 1);
    network_test_backend_clock_advance(1000U);
    CHECK(net_poll(0U) == 1 && status_is(handle, TCP_STATE_FAILED, TCP_ERR_NEIGHBOR_TIMEOUT));
    return dispose(handle);
}

static int test_terminal_paths(void)
{
    tcp_handle handle = TCP_INVALID_HANDLE;
    struct tcp_active_open *open = &tcp_global_context.connection.open;
    struct tcp_config config;
    struct control reply, syn;
    const unsigned int delays[] = {1000U, 2000U, 4000U, 8000U, 16000U};

    CHECK(initialize(&net_config) && start_cached(&handle, 0));
    reply = (struct control){22U, open->local_port, 0U, open->iss, 0x14U, 0U, 0U};
    CHECK(queue_control(&local_peer, &peer_mac, reply) && net_poll(0U) == 0);
    ++reply.ack;
    CHECK(queue_control(&local_peer, &peer_mac, reply) && net_poll(0U) == 1);
    CHECK(status_is(handle, TCP_STATE_FAILED, TCP_ERR_REFUSED));
    CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_REFUSED && dispose(handle));

    CHECK(initialize(&net_config));
    handle = TCP_INVALID_HANDLE;
    CHECK(start_cached(&handle, 0));
    syn = (struct control){open->local_port, 22U, open->iss, 0U, 2U, 0U, 536U};
    for (unsigned int i = 0; i < sizeof(delays) / sizeof(delays[0]); ++i) {
        network_test_backend_clock_advance(delays[i] - 1U);
        CHECK(net_poll(0U) == 0);
        network_test_backend_clock_advance(1U);
        CHECK(net_poll(0U) == 1);
        CHECK(expect_control(i + 1U, &local_peer, &peer_mac, syn));
    }
    network_test_backend_clock_advance(32000U);
    CHECK(net_poll(0U) == 1 && network_test_backend_transmit_count() == 6U);
    CHECK(status_is(handle, TCP_STATE_FAILED, TCP_ERR_RETRIES) && dispose(handle));

    CHECK(initialize(&net_config));
    handle = TCP_INVALID_HANDLE;
    CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_WOULD_BLOCK);
    network_test_backend_clock_advance(1000U);
    CHECK(net_poll(0U) == 1);
    network_test_backend_clock_advance(1000U);
    CHECK(net_poll(0U) == 1);
    CHECK(status_is(handle, TCP_STATE_FAILED, TCP_ERR_NEIGHBOR_TIMEOUT));
    CHECK(network_test_backend_transmit_count() == 2U && dispose(handle));

    CHECK(initialize(&net_config));
    handle = TCP_INVALID_HANDLE;
    CHECK(tcp_config_defaults(&config) == 0);
    config.connect_timeout_ms = 5U;
    CHECK(start_cached(&handle, &config));
    reply = (struct control){22U, open->local_port, 17U, open->iss + 1U, 0x12U, 16U, 0U};
    network_test_backend_set_receive_clock_step(5U);
    CHECK(queue_control(&local_peer, &peer_mac, reply));
    CHECK(tcp_connect(&handle, &local_peer, &config, 5U) == TCP_ERR_CONNECT_TIMEOUT);
    CHECK(status_is(handle, TCP_STATE_FAILED, TCP_ERR_CONNECT_TIMEOUT));
    CHECK(network_test_backend_transmit_count() == 1U && dispose(handle));

    CHECK(initialize(&net_config));
    handle = TCP_INVALID_HANDLE;
    CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == TCP_ERR_WOULD_BLOCK);
    network_test_backend_cancel_once();
    CHECK(tcp_connect(&handle, &local_peer, 0, 0U) == NET_ERR_CANCELLED);
    CHECK(status_is(handle, TCP_STATE_FAILED, NET_ERR_CANCELLED) && dispose(handle));

    CHECK(initialize(&net_config));
    handle = TCP_INVALID_HANDLE;
    CHECK(start_cached(&handle, 0));
    reply = (struct control){22U, open->local_port, 17U, open->iss + 1U, 0x12U, 16U, 0U};
    CHECK(queue_control(&local_peer, &peer_mac, reply));
    CHECK(network_test_backend_set_next_send_error(SYS_ERR_DEVICE) == 0);
    CHECK(net_poll(0U) == NET_ERR_DEVICE);
    CHECK(status_is(handle, TCP_STATE_FAILED, NET_ERR_DEVICE) && dispose(handle));
    return 1;
}

int test_tcp_active_open(void)
{
    if (!test_hmac() || !test_preflight_failures() || !test_handshake() ||
        !test_clock_and_generation() || !test_loss_and_neighbor_refresh() ||
        !test_handshake_during_refresh() || !test_ack_commit_budget() ||
        !test_local_failure_during_other_protocol() || !test_receive_recovery() ||
        !test_zero_window_ack() || !test_terminal_paths()) {
        return 0;
    }
    puts("E3 HMAC, entropy, active-open, shared service and zero-window tests: PASS");
    return 1;
}
