#include <net/tcp.h>
#include <net/net_platform.h>
#include <net/raw.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const struct net_config config = {
        {{10U, 0U, 2U, 15U}}, {{255U, 255U, 255U, 0U}}, {{10U, 0U, 2U, 2U}},
        NET_ARP_CACHE_TTL_DEFAULT_MS, NET_ARP_RETRY_INTERVAL_MIN_MS,
        NET_ARP_RETRY_COUNT_DEFAULT
    };
    static const struct tcp_endpoint endpoint = {{{10U, 0U, 2U, 2U}}, 22U};
    struct tcp_config tcp_config;
    struct tcp_status status;
    struct tcp_status saved;
    tcp_handle handle = TCP_INVALID_HANDLE;
    unsigned char byte = 0xA5U;

    memset(&status, 0xA5, sizeof(status));
    saved = status;
    if (net_init(&config) != 0 || tcp_config_defaults(&tcp_config) != 0 ||
        tcp_config.close_grace_ms <= 240000U ||
        tcp_connect(&handle, &endpoint, &tcp_config, 0U) != TCP_ERR_UNSUPPORTED ||
        handle != TCP_INVALID_HANDLE ||
        tcp_status(handle, &status) != NET_ERR_STATE ||
        memcmp(&status, &saved, sizeof(status)) != 0 ||
        tcp_recv(handle, &byte, 0U, 0U) != NET_ERR_INVALID ||
        tcp_send(handle, &byte, 1U, 0U) != NET_ERR_STATE ||
        byte != 0xA5U || net_idle(0U) != 0 ||
        net_idle(1U) != SYS_ERR_UNAVAILABLE || tcp_deinit() != 0 ||
        tcp_connect(&handle, &endpoint, 0, 0U) != NET_ERR_STATE) {
        printf("E1 TCP CONTRACT TEST: FAIL\n");
        return 1;
    }
    printf("E1 TCP CONTRACT TEST: PASS\n");
    return 0;
}
