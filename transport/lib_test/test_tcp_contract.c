#include <net/tcp.h>
#include <net/net_platform.h>
#include <net/raw.h>
#include <crypto/hmac_sha256.h>
#include <stdio.h>
#include <string.h>

static const struct net_config network = {
    {{10U, 0U, 2U, 15U}}, {{255U, 255U, 255U, 0U}}, {{10U, 0U, 2U, 2U}},
    NET_ARP_CACHE_TTL_DEFAULT_MS, NET_ARP_RETRY_INTERVAL_MIN_MS,
    NET_ARP_RETRY_COUNT_DEFAULT
};
static const struct tcp_endpoint endpoint = {{{10U, 0U, 2U, 2U}}, 22U};
static const struct tcp_endpoint routed_endpoint = {{{192U, 0U, 2U, 9U}}, 22U};

enum { STREAM_TX_COUNT = 7351, STREAM_RX_COUNT = 7129, RECOVERY_TX_COUNT = 16 * 128 };

static int check_hmac_vector(void)
{
    static const unsigned char input[] = "Hi There";
    /* RFC 4231 test case 1, also checked on the host. */
    static const unsigned char expected[CRYPTO_SHA256_BYTES] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };
    unsigned char key[20];
    unsigned char output[CRYPTO_SHA256_BYTES];

    memset(key, 0x0b, sizeof(key));
    return crypto_hmac_sha256(key, sizeof(key), input, sizeof(input) - 1U,
                              output) == 0 &&
           memcmp(output, expected, sizeof(expected)) == 0;
}

static int check_preflight(struct tcp_config *config)
{
    struct tcp_endpoint invalid = endpoint;
    struct tcp_status status;
    struct tcp_status saved;
    tcp_handle handle = TCP_INVALID_HANDLE;
    unsigned char byte = 0xA5U;

    invalid.port = 0U;
    memset(&status, 0xA5, sizeof(status));
    saved = status;
    return net_init(&network) == 0 && tcp_config_defaults(config) == 0 &&
           config->close_grace_ms > 240000U &&
           tcp_connect(&handle, &invalid, config, 0U) == NET_ERR_INVALID &&
           handle == TCP_INVALID_HANDLE &&
           tcp_status(handle, &status) == NET_ERR_STATE &&
           memcmp(&status, &saved, sizeof(status)) == 0 &&
           tcp_recv(handle, &byte, 0U, 0U) == NET_ERR_INVALID &&
           tcp_send(handle, &byte, 1U, 0U) == NET_ERR_STATE &&
           byte == 0xA5U && net_idle(0U) == 0 &&
           net_idle(1U) == SYS_ERR_UNAVAILABLE;
}

static int check_disabled(void)
{
    tcp_handle handle = TCP_INVALID_HANDLE;

    return tcp_deinit() == 0 &&
           tcp_connect(&handle, &endpoint, 0, 0U) == NET_ERR_STATE &&
           handle == TCP_INVALID_HANDLE;
}

static int check_open(const struct tcp_config *config,
                      const struct tcp_endpoint *peer, int refuse)
{
    struct tcp_status status;
    tcp_handle handle = TCP_INVALID_HANDLE;
    unsigned char byte = 0xA5U;
    int result;

    result = tcp_connect(&handle, peer, config, 0U);
    if (result != TCP_ERR_WOULD_BLOCK ||
        handle == TCP_INVALID_HANDLE ||
        tcp_status(handle, &status) != 0 || status.state != TCP_STATE_CONNECTING) {
        printf("E3 TCP initial connect result: %d\n", result);
        return 0;
    }
    result = tcp_connect(&handle, peer, config, 10000U);
    if (refuse) {
        if (result != TCP_ERR_REFUSED ||
            tcp_status(handle, &status) != 0 ||
            status.state != TCP_STATE_FAILED ||
            status.terminal_result != TCP_ERR_REFUSED ||
            tcp_recv(handle, &byte, 1U, 0U) != TCP_ERR_REFUSED ||
            tcp_connect(&handle, peer, config, 0U) != TCP_ERR_REFUSED ||
            byte != 0xA5U) {
            printf("E3 TCP refusal result: %d\n", result);
            return 0;
        }
    } else if (result != 0 ||
               tcp_status(handle, &status) != 0 ||
               status.state != TCP_STATE_ESTABLISHED ||
               status.readable_bytes != 0U || status.writable_bytes != TCP_QUEUE_CAPACITY ||
               tcp_send(handle, 0, 0U, 0U) != 0 ||
               tcp_close(handle, 0U) != TCP_ERR_UNSUPPORTED ||
               tcp_shutdown_write(handle) != TCP_ERR_UNSUPPORTED ||
               net_poll(10000U) <= 0 ||
               tcp_status(handle, &status) != 0 ||
               status.readable_bytes != 0U || status.writable_bytes != TCP_QUEUE_CAPACITY ||
               status.state != TCP_STATE_ESTABLISHED || tcp_abort(handle) != 0) {
        printf("E3 TCP handshake or ACK response result: %d\n", result);
        return 0;
    }
    return tcp_release(handle) == 0 &&
           tcp_status(handle, &status) == NET_ERR_STATE && check_disabled();
}

static unsigned char stream_byte(unsigned int index, unsigned int salt)
{
    return (unsigned char)(index * 29U + index / 251U + salt);
}

static int check_stream(const struct tcp_config *config)
{
    unsigned char buffer[383];
    struct tcp_status status;
    tcp_handle handle = TCP_INVALID_HANDLE;
    unsigned int sent = 0U;
    unsigned int received = 0U;
    unsigned int started, count, index;
    int result;

    result = tcp_connect(&handle, &routed_endpoint, config, 10000U);
    if (result != 0) {
        printf("E4 TCP stream connect: %d\n", result);
        return 0;
    }
    started = net_clock_now_ms();
    while (!net_timeout_expired(started, net_clock_now_ms(), 20000U)) {
        if (sent < STREAM_TX_COUNT) {
            count = STREAM_TX_COUNT - sent;
            if (count > sizeof(buffer)) {
                count = sizeof(buffer);
            }
            for (index = 0U; index < count; ++index) {
                buffer[index] = stream_byte(sent + index, 0U);
            }
            result = tcp_send(handle, buffer, count, 0U);
            if (result > 0 && (unsigned int)result <= count) {
                sent += (unsigned int)result;
            } else if (result != TCP_ERR_WOULD_BLOCK) {
                break;
            }
        }
        if (received < STREAM_RX_COUNT) {
            count = STREAM_RX_COUNT - received;
            if (received == 0U) {
                count = 1U;
            } else if (count > sizeof(buffer)) {
                count = sizeof(buffer);
            }
            result = tcp_recv(handle, buffer, count, 0U);
            if (result > 0 && (unsigned int)result <= count) {
                for (index = 0U; index < (unsigned int)result; ++index) {
                    if (buffer[index] != stream_byte(received + index, 77U)) {
                        printf("E4 TCP stream byte mismatch at %u\n", received + index);
                        return 0;
                    }
                }
                received += (unsigned int)result;
            } else if (result != TCP_ERR_WOULD_BLOCK) {
                break;
            }
        }
        result = net_poll(0U);
        if (result < 0 || tcp_status(handle, &status) != 0 ||
            status.state != TCP_STATE_ESTABLISHED) {
            break;
        }
        if (sent == STREAM_TX_COUNT && received == STREAM_RX_COUNT &&
            status.readable_bytes == 0U && status.writable_bytes == TCP_QUEUE_CAPACITY) {
            return tcp_abort(handle) == 0 && tcp_release(handle) == 0 && check_disabled();
        }
    }
    printf("E4 TCP stream stopped after %u sent and %u received, result %d\n",
           sent, received, result);
    return 0;
}

static int finish_transmit_probe(tcp_handle handle)
{
    struct tcp_status status;
    unsigned int started = net_clock_now_ms();

    while (!net_timeout_expired(started, net_clock_now_ms(), 10000U)) {
        if (net_poll(100U) < 0 || tcp_status(handle, &status) != 0 ||
            status.state != TCP_STATE_ESTABLISHED) {
            return 0;
        }
        if (status.writable_bytes == TCP_QUEUE_CAPACITY && status.readable_bytes == 0U) {
            return tcp_abort(handle) == 0 && tcp_release(handle) == 0 && check_disabled();
        }
    }
    return 0;
}

static int check_retransmission(const struct tcp_config *config)
{
    static const unsigned char data[] = {0x45U, 0U, 0x35U, 0x61U, 0xffU, 0x0dU,
                                         0U, 0x80U, 0x42U};
    tcp_handle handle = TCP_INVALID_HANDLE;
    unsigned char byte = 0xa5U;
    int result;

    result = tcp_connect(&handle, &routed_endpoint, config, 10000U);
    if (result != 0) {
        printf("E5a TCP retransmit connect: %d\n", result);
        return 0;
    }
    if (tcp_send(handle, data, sizeof(data), 0U) != (int)sizeof(data) ||
        tcp_recv(handle, &byte, 1U, 50U) != NET_ERR_TIMEOUT || byte != 0xa5U) {
        printf("E5a TCP retransmit initial send or caller timeout failed\n");
        return 0;
    }
    if (!finish_transmit_probe(handle)) {
        printf("E5a TCP retransmit did not drain the transmit queue\n");
        return 0;
    }
    return 1;
}

static int check_fast_recovery(const struct tcp_config *config)
{
    unsigned char data[RECOVERY_TX_COUNT];
    tcp_handle handle = TCP_INVALID_HANDLE;
    unsigned int index;
    int result;

    result = tcp_connect(&handle, &routed_endpoint, config, 10000U);
    if (result != 0) {
        printf("E5b TCP recovery connect: %d\n", result);
        return 0;
    }
    for (index = 0U; index < sizeof(data); ++index) {
        data[index] = stream_byte(index, 45U);
    }
    return tcp_send(handle, data, sizeof(data), 0U) == (int)sizeof(data) &&
           finish_transmit_probe(handle);
}

static int check_persist(const struct tcp_config *config)
{
    static const unsigned char data[] = {0x78U, 0U, 0xc3U, 0x42U, 0x80U, 0x35U};
    tcp_handle handle = TCP_INVALID_HANDLE;

    return tcp_connect(&handle, &routed_endpoint, config, 10000U) == 0 &&
           tcp_send(handle, data, sizeof(data), 0U) == (int)sizeof(data) &&
           finish_transmit_probe(handle);
}

int main(int argc, char **argv)
{
    struct tcp_config config;
    tcp_handle handle = TCP_INVALID_HANDLE;
    int success;

    if (argc != 2 || argv == 0 || argv[1] == 0 || !check_preflight(&config)) {
        printf("E3 TCP preflight: FAIL\n");
        return 1;
    }
    if (strcmp(argv[1], "entropy") == 0) {
        success = tcp_connect(&handle, &endpoint, &config, 0U) == TCP_ERR_ENTROPY &&
                  handle == TCP_INVALID_HANDLE && check_disabled();
    } else if (strcmp(argv[1], "stream") == 0) {
        success = check_stream(&config);
        printf("E4 TCP stream: %s\n", success ? "PASS" : "FAIL");
        return success ? 0 : 1;
    } else if (strcmp(argv[1], "retransmit") == 0) {
        success = check_retransmission(&config);
        printf("E5a TCP retransmit: %s\n", success ? "PASS" : "FAIL");
        return success ? 0 : 1;
    } else if (strcmp(argv[1], "recovery") == 0) {
        success = check_fast_recovery(&config);
        printf("E5b TCP fast recovery: %s\n", success ? "PASS" : "FAIL");
        return success ? 0 : 1;
    } else if (strcmp(argv[1], "persist") == 0) {
        success = check_persist(&config);
        printf("E5c TCP persist: %s\n", success ? "PASS" : "FAIL");
        return success ? 0 : 1;
    } else if (strcmp(argv[1], "open") == 0 || strcmp(argv[1], "routed") == 0 ||
               strcmp(argv[1], "refuse") == 0) {
        success = check_open(&config, strcmp(argv[1], "routed") == 0 ?
                             &routed_endpoint : &endpoint, strcmp(argv[1], "refuse") == 0);
        if (success && !check_hmac_vector()) {
            printf("E3 HMAC known-answer check: FAIL\n");
            success = 0;
        }
    } else {
        success = 0;
    }
    printf("E3 TCP %s: %s\n", argv[1], success ? "PASS" : "FAIL");
    if (!success) {
        return 1;
    }
    return 0;
}
