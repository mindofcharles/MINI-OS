#include "../network_common/deterministic_backend.h"

#include "../../transport/lib/net/tcp/internal.h"
#include "../../transport/lib/net/arp.h"
#include "../../transport/lib/net/ipv4.h"
#include "../../transport/lib/net/byteorder.h"
#include "../../transport/lib/net/net_platform.h"
#include "../../transport/lib/crypto/clear.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "E1 foundation failure at line %d: %s\n", \
            __LINE__, #condition); return 0; } } while (0)

static const struct tcp_endpoint endpoint = {{{10U, 0U, 2U, 2U}}, 22U};
static const struct net_mac_addr peer_mac = {{2U, 0U, 0U, 0U, 0U, 1U}};
static const struct net_mac_addr new_mac = {{2U, 0U, 0U, 0U, 0U, 9U}};
static const struct net_config net_config = {
    {{10U, 0U, 2U, 15U}}, {{255U, 255U, 255U, 0U}}, {{10U, 0U, 2U, 2U}},
    1000U, 1000U, 2U
};

static int initialize(void)
{
    network_test_backend_reset();
    /* Test isolation only; production never resets generation or disable state. */
    memset(&net_global_context, 0, sizeof(net_global_context));
    memset(&tcp_global_context, 0, sizeof(tcp_global_context));
    CHECK(net_init(&net_config) == 0);
    return 1;
}

static int allocate_model(tcp_handle *handle, enum tcp_state state)
{
    struct tcp_config config;

    *handle = TCP_INVALID_HANDLE;
    CHECK(tcp_config_defaults(&config) == 0);
    /* Private lifecycle fixture, not a SYN handshake or a public connect. */
    CHECK(tcp_connection_allocate(&endpoint, &config,
                                  net_clock_now_ms(), handle) == 0);
    tcp_global_context.connection.state = state;
    return 1;
}

static int all_zero(const void *data, unsigned int length)
{
    const unsigned char *bytes = data;
    unsigned int index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) return 0;
    }
    return 1;
}

static int test_public_gate(void)
{
    struct tcp_config config;
    struct tcp_endpoint changed = endpoint;
    struct tcp_status status;
    struct tcp_status saved;
    tcp_handle handle = TCP_INVALID_HANDLE;
    unsigned char byte = 0xA5U;

    CHECK(initialize());
    CHECK(sizeof(struct tcp_context) <= TCP_CONTEXT_MAX_BYTES);
    CHECK(sizeof(struct net_context) <= 4096U);
    CHECK(tcp_config_defaults(0) == NET_ERR_INVALID);
    CHECK(tcp_config_defaults(&config) == 0);
    CHECK(config.close_grace_ms > 240000U);
    CHECK(tcp_connect(&handle, &endpoint, 0, 0U) == TCP_ERR_UNSUPPORTED);
    CHECK(handle == TCP_INVALID_HANDLE && !net_global_context.pending_arp.active);
    CHECK(net_global_context.transport.owner == 0);
    CHECK(network_test_backend_transmit_count() == 0U);
    CHECK(network_test_backend_random_call_count() == 0U);
    CHECK(tcp_connect(0, &endpoint, 0, 0U) == NET_ERR_INVALID);
    CHECK(tcp_connect(&handle, 0, 0, 0U) == NET_ERR_INVALID);
    CHECK(tcp_connect(&handle, &endpoint, 0, 0x80000000U) == NET_ERR_INVALID);
    changed.port = 0U;
    CHECK(tcp_connect(&handle, &changed, 0, 0U) == NET_ERR_INVALID);
    changed.port = 65536U;
    CHECK(tcp_connect(&handle, &changed, 0, 0U) == NET_ERR_INVALID);
    changed = endpoint;
    changed.address.octets[0] = 127U;
    CHECK(tcp_connect(&handle, &changed, 0, 0U) == NET_ERR_NO_ROUTE);
    config.connect_timeout_ms = 0U;
    CHECK(tcp_connect(&handle, &endpoint, &config, 0U) == NET_ERR_INVALID);
    CHECK(tcp_config_defaults(&config) == 0);
    config.close_grace_ms = 0x80000000U;
    CHECK(tcp_connect(&handle, &endpoint, &config, 0U) == NET_ERR_INVALID);
    CHECK(tcp_config_defaults(&config) == 0);
    config.transmit_timeout_ms = 0U;
    CHECK(tcp_connect(&handle, &endpoint, &config, 0U) == NET_ERR_INVALID);
    CHECK(tcp_recv(handle, &byte, 0U, 0U) == NET_ERR_INVALID);
    CHECK(tcp_send(handle, 0, 1U, 0U) == NET_ERR_INVALID);
    CHECK(tcp_send(handle, &byte, 1U, 0U) == NET_ERR_STATE);
    CHECK(tcp_recv(handle, &byte, 1U, 0U) == NET_ERR_STATE);
    memset(&status, 0xA5, sizeof(status));
    saved = status;
    CHECK(tcp_status(handle, &status) == NET_ERR_STATE);
    CHECK(memcmp(&status, &saved, sizeof(status)) == 0 && byte == 0xA5U);
    CHECK(tcp_abort(handle) == NET_ERR_STATE);
    CHECK(tcp_release(handle) == NET_ERR_STATE);
    CHECK(tcp_close(handle, 0U) == NET_ERR_STATE);
    CHECK(tcp_deinit() == 0 && tcp_deinit() == 0);
    CHECK(tcp_connect(&handle, &endpoint, 0, 0U) == NET_ERR_STATE);
    return 1;
}

static int test_terminal_records(void)
{
    static const int reasons[] = {
        TCP_ERR_REFUSED, TCP_ERR_RESET, TCP_ERR_RETRIES, TCP_ERR_NO_MEMORY,
        TCP_ERR_CLOSE_TIMEOUT, TCP_ERR_CONNECT_TIMEOUT,
        TCP_ERR_TRANSMIT_TIMEOUT, NET_ERR_DEVICE, NET_ERR_UNAVAILABLE,
        NET_ERR_CANCELLED
    };
    unsigned int index;

    for (index = 0U; index < sizeof(reasons) / sizeof(reasons[0]); ++index) {
        struct tcp_status status;
        tcp_handle handle;
        tcp_handle next = TCP_INVALID_HANDLE;
        unsigned char byte = 0xA5U;
        unsigned char secret[TCP_INSTANCE_SECRET_BYTES];

        CHECK(initialize() && allocate_model(&handle, TCP_STATE_ESTABLISHED));
        memset(tcp_global_context.instance_secret, 0x39, sizeof(secret));
        memcpy(secret, tcp_global_context.instance_secret, sizeof(secret));
        tcp_global_context.secret_ready = 1;
        memset(tcp_global_context.connection.tx_queue, 0xA5, TCP_QUEUE_CAPACITY);
        memset(tcp_global_context.connection.rx_queue, 0xA5, TCP_QUEUE_CAPACITY);
        memset(tcp_global_context.connection.secret_temporary, 0xA5,
               TCP_INSTANCE_SECRET_BYTES);
        tcp_global_context.connection.tx_length = 7U;
        CHECK(tcp_connection_progress_result(7U, reasons[index]) == 7);
        CHECK(tcp_status(handle, &status) == 0);
        CHECK(status.state == TCP_STATE_FAILED &&
              status.terminal_result == reasons[index]);
        CHECK((status.flags & TCP_STATUS_TERMINAL) != 0U);
        CHECK(all_zero(tcp_global_context.connection.tx_queue, TCP_QUEUE_CAPACITY));
        CHECK(all_zero(tcp_global_context.connection.rx_queue, TCP_QUEUE_CAPACITY));
        CHECK(all_zero(tcp_global_context.connection.secret_temporary,
                       TCP_INSTANCE_SECRET_BYTES));
        CHECK(memcmp(secret, tcp_global_context.instance_secret, sizeof(secret)) == 0);
        CHECK(tcp_recv(handle, &byte, 1U, 0U) == reasons[index]);
        CHECK(tcp_send(handle, &byte, 1U, 0U) == reasons[index]);
        CHECK(tcp_shutdown_write(handle) == reasons[index]);
        CHECK(tcp_close(handle, 0U) == reasons[index]);
        CHECK(tcp_connect(&next, &endpoint, 0, 0U) == NET_ERR_BUSY);
        CHECK(tcp_deinit() == NET_ERR_BUSY && byte == 0xA5U);
        CHECK(tcp_abort(handle) == 0);
        CHECK(tcp_status(handle, &status) == 0 &&
              status.terminal_result == reasons[index]);
        network_test_backend_cancel_once();
        CHECK(tcp_status(handle, &status) == 0);
        CHECK(net_cancel_requested() == 1 && net_cancel_requested() == 0);
        CHECK(tcp_release(handle) == 0);
        CHECK(all_zero(&tcp_global_context.connection,
                       sizeof(tcp_global_context.connection)));
        CHECK(tcp_status(handle, &status) == NET_ERR_STATE);
        CHECK(allocate_model(&next, TCP_STATE_ESTABLISHED) && next != handle);
        CHECK(tcp_abort(handle) == NET_ERR_STATE);
        CHECK(tcp_abort(next) == 0 && tcp_release(next) == 0);
        CHECK(tcp_deinit() == 0);
        CHECK(all_zero(tcp_global_context.instance_secret, sizeof(secret)));
        CHECK(tcp_global_context.last_generation == next);
    }
    CHECK(initialize());
    tcp_global_context.last_generation = UINT_MAX;
    {
        struct tcp_config config;
        tcp_handle handle = TCP_INVALID_HANDLE;
        CHECK(tcp_config_defaults(&config) == 0);
        CHECK(tcp_connection_allocate(&endpoint, &config, 0U, &handle) ==
              TCP_ERR_GENERATION_EXHAUSTED);
        CHECK(handle == TCP_INVALID_HANDLE &&
              tcp_global_context.last_generation == UINT_MAX);
        CHECK(net_global_context.transport.owner == 0);
        CHECK(tcp_deinit() == 0 && tcp_global_context.last_generation == UINT_MAX);
    }
    CHECK(initialize());
    tcp_global_context.last_generation = UINT_MAX - 1U;
    {
        struct tcp_config config;
        tcp_handle last;
        tcp_handle next = TCP_INVALID_HANDLE;
        CHECK(allocate_model(&last, TCP_STATE_ESTABLISHED) && last == UINT_MAX);
        CHECK(tcp_abort(last) == 0 && tcp_release(last) == 0);
        CHECK(tcp_config_defaults(&config) == 0);
        CHECK(tcp_connection_allocate(&endpoint, &config, 0U, &next) ==
              TCP_ERR_GENERATION_EXHAUSTED);
        CHECK(next == TCP_INVALID_HANDLE && tcp_abort(last) == NET_ERR_STATE);
    }
    return 1;
}

static int test_close_contract(void)
{
    tcp_handle handle;
    struct tcp_connection *connection;
    struct tcp_status status;
    unsigned int started;
    unsigned char byte = 0xA5U;

    CHECK(initialize() && allocate_model(&handle, TCP_STATE_ESTABLISHED));
    connection = &tcp_global_context.connection;
    connection->config.close_grace_ms = 10U;
    network_test_backend_clock_set(UINT_MAX - 2U);
    started = net_clock_now_ms();
    CHECK(tcp_send(handle, 0, 0U, 0U) == 0);
    CHECK(tcp_send(handle, &byte, 1U, 0U) == TCP_ERR_UNSUPPORTED);
    CHECK(tcp_close(handle, 0U) == TCP_ERR_WOULD_BLOCK);
    CHECK(connection->close_started_ms == started && connection->fin_pending);
    CHECK(tcp_release(handle) == NET_ERR_BUSY);
    connection->fin_pending = 0; /* Model one already-issued logical FIN. */
    network_test_backend_clock_advance(4U);
    CHECK(tcp_shutdown_write(handle) == 0);
    CHECK(tcp_close(handle, 0U) == TCP_ERR_WOULD_BLOCK);
    CHECK(!connection->fin_pending && connection->close_started_ms == started);
    CHECK(tcp_send(handle, &byte, 1U, 0U) == TCP_ERR_CLOSED);
    network_test_backend_set_receive_clock_step(2U);
    CHECK(tcp_close(handle, 3U) == NET_ERR_TIMEOUT);
    CHECK(connection->state == TCP_STATE_FIN_WAIT_1 &&
          connection->close_started_ms == started);
    network_test_backend_clock_advance(2U);
    CHECK(tcp_close(handle, 0U) == TCP_ERR_CLOSE_TIMEOUT);
    CHECK(tcp_status(handle, &status) == 0 &&
          status.terminal_result == TCP_ERR_CLOSE_TIMEOUT);
    CHECK(network_test_backend_transmit_count() == 0U); /* No FIN serializer yet. */
    CHECK(tcp_release(handle) == 0);

    CHECK(initialize() && allocate_model(&handle, TCP_STATE_ESTABLISHED));
    connection = &tcp_global_context.connection;
    connection->rx_length = 3U;
    memcpy(connection->rx_queue, "abc", 3U);
    CHECK(tcp_close(handle, 0U) == TCP_ERR_UNREAD && !connection->close_started);
    CHECK(tcp_release(handle) == TCP_ERR_UNREAD);
    CHECK(memcmp(connection->rx_queue, "abc", 3U) == 0);
    CHECK(tcp_shutdown_write(handle) == 0 && connection->rx_length == 3U);
    connection->wire_closed = 1;
    connection->peer_fin = 1;
    CHECK(tcp_connection_complete() == TCP_ERR_UNREAD);
    connection->rx_length = 0U; /* Model delivery; E4 implements actual receives. */
    CHECK(tcp_close(handle, 0U) == TCP_ERR_UNREAD);
    connection->eof_observed = 1;
    CHECK(tcp_close(handle, 0U) == 0);
    CHECK(tcp_connection_complete() == 0 && tcp_close(handle, 0U) == 0);
    CHECK(tcp_recv(handle, &byte, 1U, 0U) == 0 && byte == 0xA5U);
    CHECK(tcp_send(handle, &byte, 1U, 0U) == TCP_ERR_CLOSED);
    CHECK(tcp_status(handle, &status) == 0 && status.state == TCP_STATE_CLOSED);
    CHECK(tcp_release(handle) == 0 && tcp_deinit() == 0);

    CHECK(initialize() && allocate_model(&handle, TCP_STATE_ESTABLISHED));
    network_test_backend_cancel_once();
    CHECK(tcp_close(handle, 0U) == NET_ERR_CANCELLED);
    CHECK(tcp_close(handle, 0U) == NET_ERR_CANCELLED);
    CHECK(tcp_release(handle) == 0);

    CHECK(initialize() && allocate_model(&handle, TCP_STATE_CONNECTING));
    connection = &tcp_global_context.connection;
    connection->config.connect_timeout_ms = 5U;
    CHECK(tcp_connect(&handle, &endpoint, &connection->config, 0U) ==
          TCP_ERR_UNSUPPORTED);
    CHECK(connection->started_ms == 0U);
    {
        struct tcp_endpoint changed_endpoint = endpoint;
        struct tcp_config changed_config = connection->config;
        changed_endpoint.port = 23U;
        ++changed_config.close_grace_ms;
        CHECK(tcp_connect(&handle, &changed_endpoint, &connection->config, 0U) ==
              NET_ERR_INVALID);
        CHECK(tcp_connect(&handle, &endpoint, &changed_config, 0U) ==
              NET_ERR_INVALID);
        CHECK(connection->started_ms == 0U && connection->handle == handle);
    }
    network_test_backend_clock_advance(5U);
    CHECK(net_poll(0U) == TCP_ERR_CONNECT_TIMEOUT);
    CHECK(tcp_recv(handle, &byte, 1U, 0U) == TCP_ERR_CONNECT_TIMEOUT);
    CHECK(tcp_abort(handle) == 0 && tcp_release(handle) == 0);
    return 1;
}

static void arp_reply(unsigned char frame[NET_FRAME_MIN],
                       const struct net_mac_addr *mac)
{
    unsigned char *arp = frame + 14U;

    memset(frame, 0, NET_FRAME_MIN);
    memcpy(frame, net_global_context.device_mac.octets, 6U);
    memcpy(frame + 6U, mac->octets, 6U);
    net_write_be16(frame + 12U, 0x0806U);
    net_write_be16(arp, 1U);
    net_write_be16(arp + 2U, 0x0800U);
    arp[4] = 6U;
    arp[5] = 4U;
    net_write_be16(arp + 6U, 2U);
    memcpy(arp + 8U, mac->octets, 6U);
    memcpy(arp + 14U, endpoint.address.octets, 4U);
    memcpy(arp + 18U, net_global_context.device_mac.octets, 6U);
    memcpy(arp + 24U, net_config.address.octets, 4U);
}

static int test_arp_start_deadline_cleanup(void)
{
    unsigned int mode;
    unsigned int wrap;

    for (mode = 0U; mode < 4U; ++mode) {
        for (wrap = 0U; wrap < 2U; ++wrap) {
            struct net_mac_addr mac;
            struct net_mac_addr saved_mac;
            struct net_ping_result ping_result;
            struct net_ping_result saved_result;
            struct tcp_status status;
            tcp_handle handle = TCP_INVALID_HANDLE;
            unsigned char frame[NET_FRAME_MIN];
            int result;

            CHECK(initialize());
            if (mode >= 2U) {
                CHECK(allocate_model(&handle, TCP_STATE_ESTABLISHED));
            }
            memset(&mac, 0xA5, sizeof(mac));
            saved_mac = mac;
            memset(&ping_result, 0xA5, sizeof(ping_result));
            saved_result = ping_result;
            network_test_backend_clock_set(wrap ? UINT_MAX - 1U : 0U);
            /* Expire during start, after its pre-install deadline check. */
            network_test_backend_set_clock_read_step(1U);
            result = (mode & 1U) != 0U ?
                net_ping(&endpoint.address, 1U, 0, 0U, 2U, &ping_result) :
                net_resolve_arp(&endpoint.address, &mac, 2U);
            network_test_backend_set_clock_read_step(0U);
            CHECK(result == NET_ERR_TIMEOUT);
            CHECK(all_zero(&net_global_context.pending_arp,
                           sizeof(net_global_context.pending_arp)));
            CHECK(!net_global_context.service_active &&
                  !net_global_context.pending_echo.active);
            CHECK(memcmp(&mac, &saved_mac, sizeof(mac)) == 0);
            CHECK(memcmp(&ping_result, &saved_result, sizeof(ping_result)) == 0);
            CHECK(network_test_backend_transmit_count() == 0U);
            if (handle != TCP_INVALID_HANDLE) {
                CHECK(tcp_status(handle, &status) == 0 &&
                      status.state == TCP_STATE_ESTABLISHED &&
                      status.terminal_result == 0);
            }
            /* Both polling and fresh wire resolution must remain usable. */
            CHECK(net_poll(0U) == 0);
            arp_reply(frame, &peer_mac);
            CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
            CHECK(net_resolve_arp(&endpoint.address, &mac, 0U) == 0);
            CHECK(memcmp(mac.octets, peer_mac.octets, 6U) == 0);
            CHECK(network_test_backend_transmit_count() == 1U);
            CHECK(!net_global_context.pending_arp.active &&
                  !net_global_context.service_active);
            if (handle != TCP_INVALID_HANDLE) {
                CHECK(tcp_abort(handle) == 0 && tcp_release(handle) == 0);
            }
        }
    }

    /* The private owned boundary must retain its result until explicit finish. */
    {
        tcp_handle handle;
        CHECK(initialize() && allocate_model(&handle, TCP_STATE_ESTABLISHED));
        network_test_backend_set_clock_read_step(1U);
        CHECK(net_arp_task_start(&net_global_context, &endpoint.address,
                                 handle, 0U, 2U) == NET_ERR_TIMEOUT);
        network_test_backend_set_clock_read_step(0U);
        CHECK(net_global_context.pending_arp.active &&
              net_global_context.pending_arp.owner == handle &&
              net_global_context.pending_arp.result == NET_ERR_TIMEOUT);
        CHECK(net_arp_task_result(&net_global_context, handle, 0) == NET_ERR_TIMEOUT);
        CHECK(net_arp_task_finish(&net_global_context, handle + 1U) == NET_ERR_STATE);
        CHECK(net_global_context.pending_arp.active);
        CHECK(net_arp_task_finish(&net_global_context, handle) == 0);
        CHECK(tcp_abort(handle) == 0 && tcp_release(handle) == 0);
    }
    return 1;
}

static int test_neighbor_tasks(void)
{
    tcp_handle handle;
    struct net_mac_addr mac;
    unsigned char frame[NET_FRAME_MIN];
    unsigned int original_start;

    CHECK(initialize() && allocate_model(&handle, TCP_STATE_ESTABLISHED));
    original_start = net_clock_now_ms();
    CHECK(net_arp_task_start(&net_global_context, &endpoint.address, handle,
                             original_start, 5000U) == NET_ARP_TASK_PENDING);
    CHECK(net_poll(0U) == 1 && net_global_context.pending_arp.attempts == 1U);
    CHECK(net_poll(0U) == 0 && network_test_backend_transmit_count() == 1U);
    CHECK(net_resolve_arp(&endpoint.address, &mac, 0U) == NET_ERR_BUSY);
    CHECK(net_arp_task_result(&net_global_context, handle + 1U, &mac) == NET_ERR_STATE);
    CHECK(net_arp_task_finish(&net_global_context, handle + 1U) == NET_ERR_STATE);
    arp_reply(frame, &peer_mac);
    CHECK(network_test_backend_schedule_receive(frame, sizeof(frame), 25U) == 0);
    network_test_backend_set_receive_clock_step(1U);
    CHECK(net_poll(10U) == 0);
    CHECK(net_global_context.pending_arp.active &&
          net_global_context.pending_arp.started_ms == original_start);
    CHECK(net_arp_task_start(&net_global_context, &endpoint.address, handle,
                             net_clock_now_ms(), 1U) == NET_ARP_TASK_PENDING);
    CHECK(net_global_context.pending_arp.timeout_ms == 5000U);
    CHECK(net_idle(20U) == 0 && network_test_backend_pending_receive_count() == 1U);
    CHECK(net_poll(0U) == 1);
    CHECK(net_arp_task_result(&net_global_context, handle, &mac) == 0);
    CHECK(memcmp(mac.octets, peer_mac.octets, 6U) == 0);
    CHECK(net_arp_task_finish(&net_global_context, handle) == 0);
    CHECK(net_arp_task_finish(&net_global_context, handle) == NET_ERR_STATE);
    CHECK(net_resolve_arp(&endpoint.address, &mac, 0U) == 0);
    network_test_backend_set_receive_clock_step(0U);
    network_test_backend_clock_advance(1001U);
    CHECK(net_arp_cache_lookup(&net_global_context, &endpoint.address, &mac) == 0);
    CHECK(net_arp_task_start(&net_global_context, &endpoint.address, handle,
                             net_clock_now_ms(), 5000U) == NET_ARP_TASK_PENDING);
    arp_reply(frame, &new_mac);
    CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
    CHECK(net_poll(0U) == 2);
    CHECK(net_arp_task_result(&net_global_context, handle, &mac) == 0);
    CHECK(memcmp(mac.octets, new_mac.octets, 6U) == 0);
    CHECK(net_arp_task_finish(&net_global_context, handle) == 0);

    network_test_backend_clock_advance(1001U);
    CHECK(net_arp_task_start(&net_global_context, &endpoint.address, handle,
                             net_clock_now_ms(), 5000U) == NET_ARP_TASK_PENDING);
    CHECK(net_poll(0U) == 1);
    network_test_backend_clock_advance(1000U);
    CHECK(net_poll(0U) == 1);
    CHECK(net_global_context.pending_arp.attempts == 2U);
    network_test_backend_clock_advance(1000U);
    CHECK(net_poll(0U) == 0);
    CHECK(net_arp_task_result(&net_global_context, handle, &mac) == NET_ERR_TIMEOUT);
    CHECK(net_arp_task_result(&net_global_context, handle, &mac) == NET_ERR_TIMEOUT);
    CHECK(net_arp_task_finish(&net_global_context, handle) == 0);

    /* A task deadline also bounds initial throttle delay before any attempt. */
    net_global_context.arp_last_request_ms = net_clock_now_ms();
    CHECK(net_arp_task_start(&net_global_context, &endpoint.address, handle,
                             net_clock_now_ms(), 5U) == NET_ARP_TASK_PENDING);
    CHECK(net_poll(0U) == 0 && net_global_context.pending_arp.attempts == 0U);
    network_test_backend_clock_advance(5U);
    CHECK(net_arp_task_result(&net_global_context, handle, 0) == NET_ERR_TIMEOUT);
    CHECK(tcp_abort(handle) == 0 && !net_global_context.pending_arp.active);
    CHECK(tcp_release(handle) == 0);
    CHECK(allocate_model(&handle, TCP_STATE_ESTABLISHED));
    CHECK(net_arp_task_start(&net_global_context, &endpoint.address, handle - 1U,
                             net_clock_now_ms(), 5000U) == NET_ERR_STATE);
    CHECK(net_arp_task_start(&net_global_context, &endpoint.address, handle,
                             net_clock_now_ms(), 5000U) == NET_ARP_TASK_PENDING);
    network_test_backend_cancel_once();
    CHECK(net_poll(0U) == NET_ERR_CANCELLED && !net_global_context.pending_arp.active);
    CHECK(net_poll(0U) == 0);
    {
        unsigned char byte;
        CHECK(tcp_recv(handle, &byte, 1U, 0U) == NET_ERR_CANCELLED);
    }
    CHECK(tcp_release(handle) == 0);
    CHECK(allocate_model(&handle, TCP_STATE_ESTABLISHED));
    CHECK(net_arp_task_start(&net_global_context, &endpoint.address, handle,
                             net_clock_now_ms(), 5000U) == NET_ARP_TASK_PENDING);
    CHECK(net_arp_task_finish(&net_global_context, handle - 1U) == NET_ERR_STATE);
    CHECK(tcp_abort(handle) == 0 && tcp_release(handle) == 0);

    CHECK(initialize() && allocate_model(&handle, TCP_STATE_ESTABLISHED));
    network_test_backend_clock_set(UINT_MAX - 600U);
    original_start = net_clock_now_ms();
    CHECK(net_arp_task_start(&net_global_context, &endpoint.address, handle,
                             original_start, 5000U) == NET_ARP_TASK_PENDING);
    CHECK(net_poll(0U) == 1);
    network_test_backend_clock_advance(1000U);
    CHECK(net_poll(0U) == 1 && net_global_context.pending_arp.attempts == 2U);
    CHECK(net_global_context.pending_arp.started_ms == original_start);
    network_test_backend_clock_advance(1000U);
    CHECK(net_arp_task_result(&net_global_context, handle, 0) == NET_ERR_TIMEOUT);
    CHECK(tcp_abort(handle) == 0 && tcp_release(handle) == 0);
    return 1;
}

struct observer {
    unsigned int timers;
    unsigned int packets;
    unsigned int failures;
    unsigned int payload_length;
    unsigned char first_byte;
    int nested_result;
};

static int observe_packet(void *owner, const struct net_ethernet_view *ethernet,
                           const struct net_ipv4_view *packet)
{
    struct observer *observer = owner;
    (void)ethernet;
    ++observer->packets;
    observer->payload_length = packet->payload_length;
    observer->first_byte = packet->payload[0];
    observer->nested_result = net_poll(0U);
    return 1;
}

static int observe_timer(void *owner, net_u32 now_ms)
{
    struct observer *observer = owner;
    (void)now_ms;
    ++observer->timers;
    return 0;
}

static void observe_failure(void *owner, int result)
{
    struct observer *observer = owner;
    (void)result;
    ++observer->failures;
}

static unsigned int independent_checksum(const unsigned char *data,
                                          unsigned int length)
{
    unsigned int sum = 0U;
    unsigned int index;
    for (index = 0U; index < length; index += 2U)
        sum += ((unsigned int)data[index] << 8) | data[index + 1U];
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (~sum) & 0xFFFFU;
}

static int test_transport_binding(void)
{
    struct observer observer = {0};
    struct net_transport_binding binding = {
        observe_packet, observe_timer, observe_failure, &observer, 6U, 0U
    };
    unsigned char frame[NET_FRAME_MIN];
    unsigned char *ip = frame + 14U;
    unsigned int index;

    CHECK(initialize());
    memset(frame, 0xA5, sizeof(frame));
    memcpy(frame, net_global_context.device_mac.octets, 6U);
    memcpy(frame + 6U, peer_mac.octets, 6U);
    net_write_be16(frame + 12U, 0x0800U);
    memset(ip, 0, 20U);
    ip[0] = 0x45U; ip[8] = 64U; ip[9] = 6U;
    net_write_be16(ip + 2U, 23U);
    memcpy(ip + 12U, endpoint.address.octets, 4U);
    memcpy(ip + 16U, net_config.address.octets, 4U);
    net_write_be16(ip + 10U, (net_u16)independent_checksum(ip, 20U));
    CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
    CHECK(net_poll(0U) == 0 && net_global_context.counters.unsupported_frames == 1U);
    CHECK(net_transport_register(&net_global_context, &binding) == 0);
    CHECK(net_transport_register(&net_global_context, &binding) == NET_ERR_BUSY);
    binding.timer = 0; /* The registered binding is a copy. */
    CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
    CHECK(net_poll(0U) == 1 && observer.packets == 1U && observer.timers == 1U);
    CHECK(observer.payload_length == 3U && observer.first_byte == 0xA5U);
    CHECK(observer.nested_result == NET_ERR_BUSY);
    for (index = 0U; index < 20U; ++index) {
        CHECK(network_test_backend_queue_receive(frame, 1U) == 0);
        CHECK(net_poll(0U) == 0);
    }
    CHECK(observer.timers == 21U && observer.packets == 1U);
    CHECK(network_test_backend_queue_receive_error(SYS_ERR_DEVICE) == 0);
    CHECK(net_poll(0U) == NET_ERR_DEVICE && observer.failures == 1U);
    CHECK(net_transport_unregister(&net_global_context, &binding) == NET_ERR_STATE);
    CHECK(net_transport_unregister(&net_global_context, &observer) == 0);
    CHECK(network_test_backend_queue_receive(frame, sizeof(frame)) == 0);
    CHECK(net_poll(0U) == 0 && observer.packets == 1U);
    return 1;
}

static int test_poll_error_after_prefix(void)
{
    tcp_handle handle;
    struct tcp_status status;
    unsigned char byte = 0xA5U;
    int accepted;

    CHECK(initialize() && allocate_model(&handle, TCP_STATE_ESTABLISHED));
    memcpy(tcp_global_context.connection.tx_queue, "prefix", 6U);
    tcp_global_context.connection.tx_length = 6U;
    accepted = tcp_connection_progress_result(6U, 0);
    CHECK(accepted == 6);
    CHECK(network_test_backend_queue_receive_error(SYS_ERR_DEVICE) == 0);
    CHECK(net_poll(0U) == NET_ERR_DEVICE);
    CHECK(accepted == 6 && tcp_recv(handle, &byte, 1U, 0U) == NET_ERR_DEVICE);
    CHECK(tcp_status(handle, &status) == 0 && status.terminal_result == NET_ERR_DEVICE);
    CHECK(all_zero(tcp_global_context.connection.tx_queue, TCP_QUEUE_CAPACITY));
    CHECK(tcp_release(handle) == 0);
    return 1;
}

static int cancel_on_third_check(void *context)
{
    unsigned int *checks = context;
    return ++*checks == 3U;
}

static int test_shared_operation_failure(void)
{
    unsigned int mode;

    for (mode = 0U; mode < 3U; ++mode) {
        tcp_handle handle;
        struct net_mac_addr mac;
        struct net_mac_addr saved_mac;
        struct net_ping_result ping_result;
        struct net_ping_result saved_result;
        struct tcp_status status;
        unsigned int checks = 0U;
        unsigned char byte = 0xA5U;
        int reason = mode == 2U ? NET_ERR_DEVICE : NET_ERR_CANCELLED;

        CHECK(initialize() && allocate_model(&handle, TCP_STATE_ESTABLISHED));
        CHECK(net_arp_cache_store(&net_global_context, &endpoint.address,
                                  &peer_mac) == 0);
        memset(&mac, 0xA5, sizeof(mac));
        saved_mac = mac;
        memset(&ping_result, 0xA5, sizeof(ping_result));
        saved_result = ping_result;
        if (mode == 0U) {
            network_test_backend_cancel_once();
            CHECK(net_resolve_arp(&endpoint.address, &mac, 0U) == reason);
            CHECK(memcmp(&mac, &saved_mac, sizeof(mac)) == 0);
        } else {
            if (mode == 1U) {
                CHECK(net_set_cancel_callback(cancel_on_third_check, &checks) == 0);
            } else {
                CHECK(network_test_backend_set_next_send_error(SYS_ERR_DEVICE) == 0);
            }
            CHECK(net_ping(&endpoint.address, 1U, &byte, 1U, 10U,
                           &ping_result) == reason);
            CHECK(memcmp(&ping_result, &saved_result, sizeof(ping_result)) == 0);
        }
        CHECK(!net_global_context.pending_arp.active &&
              !net_global_context.pending_echo.active);
        CHECK(tcp_recv(handle, &byte, 1U, 0U) == reason && byte == 0xA5U);
        CHECK(tcp_status(handle, &status) == 0 && status.terminal_result == reason);
        CHECK(tcp_release(handle) == 0);
    }
    return 1;
}

static int test_backend_extensions(void)
{
    unsigned char frame[NET_FRAME_MIN] = {0};
    unsigned char output[NET_FRAME_MIN];
    unsigned char random[8];
    unsigned char guarded[10];
    unsigned int index;
    unsigned int clock;

    CHECK(initialize());
    CHECK(strcmp(network_test_backend_marker(),
                 "MINI_OS_NETWORK_DETERMINISTIC_BACKEND_ONLY") == 0);
    for (index = 0U; index < NETWORK_TEST_BACKEND_QUEUE_CAPACITY; ++index) {
        frame[0] = (unsigned char)index;
        CHECK(net_send_frame(frame, sizeof(frame)) == (int)sizeof(frame));
    }
    CHECK(net_send_frame(frame, sizeof(frame)) == SYS_ERR_RANGE);
    for (index = 0U; index < NETWORK_TEST_BACKEND_QUEUE_CAPACITY * 4U; ++index) {
        CHECK(network_test_backend_drop_transmits(1U) == 0);
        frame[0] = (unsigned char)(index + NETWORK_TEST_BACKEND_QUEUE_CAPACITY);
        CHECK(net_send_frame(frame, sizeof(frame)) == (int)sizeof(frame));
        CHECK(network_test_backend_transmit_frame(0U)[0] == (unsigned char)(index + 1U));
    }
    CHECK(network_test_backend_total_transmit_count() == NETWORK_TEST_BACKEND_QUEUE_CAPACITY * 5U);
    CHECK(network_test_backend_drop_transmits(NETWORK_TEST_BACKEND_QUEUE_CAPACITY + 1U) == SYS_ERR_RANGE);
    CHECK(network_test_backend_drop_transmits(NETWORK_TEST_BACKEND_QUEUE_CAPACITY) == 0);
    CHECK(network_test_backend_transmit_count() == 0U);
    network_test_backend_clock_set(UINT_MAX - 20U);
    frame[0] = 1U;
    CHECK(network_test_backend_schedule_receive(frame, sizeof(frame), 30U) == 0);
    frame[0] = 2U;
    CHECK(network_test_backend_schedule_receive(frame, sizeof(frame), 10U) == 0);
    CHECK(network_test_backend_schedule_receive_error(SYS_ERR_DEVICE, 20U) == 0);
    CHECK(net_recv_frame(output, sizeof(output)) == 0);
    CHECK(net_idle(50U) == 0 && network_test_backend_pending_receive_count() == 3U);
    CHECK(net_recv_frame(output, sizeof(output)) == (int)sizeof(output) && output[0] == 2U);
    CHECK(net_idle(50U) == 0 && net_recv_frame(output, sizeof(output)) == SYS_ERR_DEVICE);
    CHECK(net_idle(50U) == 0 && net_recv_frame(output, sizeof(output)) == (int)sizeof(output));
    CHECK(output[0] == 1U && network_test_backend_idle_elapsed_ms() == 30U);
    clock = net_clock_now_ms();
    CHECK(net_idle(0U) == 0 && net_clock_now_ms() == clock);
    CHECK(network_test_backend_idle_count() == 3U);
    CHECK(net_idle(0x80000000U) == SYS_ERR_RANGE);
    network_test_backend_cancel_once();
    CHECK(net_idle(50U) == 0 && net_clock_now_ms() == clock);
    CHECK(net_cancel_requested() == 1 && net_cancel_requested() == 0);
    memset(random, 0xA5, sizeof(random));
    CHECK(network_test_backend_set_next_random_result(3) == 0);
    CHECK(net_random_bytes(random, sizeof(random)) == 3);
    CHECK(random[3] == 0xA5U && random[7] == 0xA5U);
    CHECK(network_test_backend_set_next_random_result(SYS_ERR_UNAVAILABLE) == 0);
    CHECK(net_random_bytes(random, sizeof(random)) == SYS_ERR_UNAVAILABLE);
    CHECK(net_random_bytes(random, sizeof(random)) == (int)sizeof(random));
    CHECK(network_test_backend_random_call_count() == 3U);
    memset(guarded, 0xA5, sizeof(guarded));
    crypto_clear(guarded + 1U, 8U);
    CHECK(guarded[0] == 0xA5U && guarded[9] == 0xA5U);
    CHECK(all_zero(guarded + 1U, 8U));
    crypto_clear(0, 0U);
    CHECK(initialize());
    CHECK(net_clock_now_ms() == 0U && net_clock_now_ms() == 0U);
    network_test_backend_clock_set(UINT_MAX);
    network_test_backend_set_clock_read_step(1U);
    CHECK(net_clock_now_ms() == 0U && net_clock_now_ms() == 1U);
    network_test_backend_set_clock_read_step(0U);
    CHECK(net_clock_now_ms() == 1U && net_clock_now_ms() == 1U);
    network_test_backend_set_clock_read_step(3U);
    network_test_backend_reset();
    CHECK(net_clock_now_ms() == 0U && net_clock_now_ms() == 0U);
    return 1;
}

int main(void)
{
    if (!test_public_gate() || !test_terminal_records() || !test_close_contract() ||
        !test_arp_start_deadline_cleanup() || !test_neighbor_tasks() ||
        !test_transport_binding() ||
        !test_poll_error_after_prefix() || !test_shared_operation_failure() ||
        !test_backend_extensions()) return 1;
    puts("E1 TCP foundation, owned ARP, service and backend tests: PASS");
    puts("Private lifecycle fixtures are not TCP wire interoperability evidence.");
    return 0;
}
