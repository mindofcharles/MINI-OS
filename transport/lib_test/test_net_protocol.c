#include <net/net.h>
#include <stdio.h>
#include <string.h>

static const struct net_config default_config = {
    {{10U, 0U, 2U, 15U}},
    {{255U, 255U, 255U, 0U}},
    {{10U, 0U, 2U, 2U}},
    NET_ARP_CACHE_TTL_DEFAULT_MS,
    NET_ARP_RETRY_INTERVAL_MIN_MS,
    2U
};

static const struct net_ipv4_addr gateway = {{10U, 0U, 2U, 2U}};

static int initialize(unsigned int cache_ttl_ms)
{
    struct net_config config = default_config;
    int status;

    config.arp_cache_ttl_ms = cache_ttl_ms;
    status = net_init(&config);
    if (status != 0) {
        printf("D6 INIT: FAIL %d\n", status);
        return 1;
    }
    return 0;
}

static int one_ping(unsigned int sequence, unsigned int timeout_ms)
{
    static const char payload[] = "D6";
    struct net_ping_result result;
    int status;

    status = net_ping(&gateway, sequence, payload,
                      (unsigned int)(sizeof(payload) - 1U),
                      timeout_ms, &result);
    if (status != 0 || result.sequence != sequence ||
        result.payload_length != sizeof(payload) - 1U) {
        printf("D6 PING: FAIL %d\n", status);
        return 1;
    }
    return 0;
}

static int cache_test(void)
{
    int status;

    if (initialize(5000U) != 0 || one_ping(1U, 3000U) != 0) {
        return 1;
    }
    printf("D6 CACHE FIRST: PASS\n");
    if (one_ping(2U, 3000U) != 0) {
        return 1;
    }
    printf("D6 CACHE SECOND: PASS\n");
    status = net_poll(5500U);
    if (status != 0) {
        printf("D6 CACHE WAIT: FAIL %d\n", status);
        return 1;
    }
    if (one_ping(3U, 3000U) != 0) {
        return 1;
    }
    printf("D6 CACHE EXPIRED: PASS\n");
    return 0;
}

static int timeout_test(const char *name)
{
    static const char payload[] = "D6";
    struct net_ping_result result;
    int status;

    if (initialize(NET_ARP_CACHE_TTL_DEFAULT_MS) != 0) {
        return 1;
    }
    status = net_ping(&gateway, 1U, payload,
                      (unsigned int)(sizeof(payload) - 1U),
                      1500U, &result);
    if (status != NET_ERR_TIMEOUT) {
        printf("D6 %s TIMEOUT: FAIL %d\n", name, status);
        return 1;
    }
    printf("D6 %s TIMEOUT: PASS\n", name);
    return 0;
}

static int cancellation_test(void)
{
    static const char payload[] = "D6";
    struct net_ping_result result;
    int status;

    if (initialize(NET_ARP_CACHE_TTL_DEFAULT_MS) != 0) {
        return 1;
    }
    status = net_ping(&gateway, 1U, payload,
                      (unsigned int)(sizeof(payload) - 1U),
                      7000U, &result);
    if (status != NET_ERR_CANCELLED) {
        printf("D6 CANCEL: FAIL %d\n", status);
        return 1;
    }
    printf("D6 CANCEL: PASS\n");
    return 0;
}

static int server_test(void)
{
    int status;

    if (initialize(NET_ARP_CACHE_TTL_DEFAULT_MS) != 0) {
        return 1;
    }
    printf("D6 SERVER READY\n");
    status = net_poll(5000U);
    if (status <= 0) {
        printf("D6 SERVER ARP: FAIL %d\n", status);
        return 1;
    }
    printf("D6 SERVER ARP: PASS\n");
    status = net_poll(5000U);
    if (status <= 0) {
        printf("D6 SERVER ECHO: FAIL %d\n", status);
        return 1;
    }
    printf("D6 SERVER ECHO: PASS\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || argv == 0 || argv[1] == 0) {
        printf("usage: test_net_protocol <mode>\n");
        return 1;
    }
    if (strcmp(argv[1], "cache") == 0) {
        return cache_test();
    }
    if (strcmp(argv[1], "arp_timeout") == 0) {
        return timeout_test("ARP");
    }
    if (strcmp(argv[1], "icmp_timeout") == 0) {
        return timeout_test("ICMP");
    }
    if (strcmp(argv[1], "cancel") == 0) {
        return cancellation_test();
    }
    if (strcmp(argv[1], "server") == 0) {
        return server_test();
    }
    printf("usage: test_net_protocol <mode>\n");
    return 1;
}
