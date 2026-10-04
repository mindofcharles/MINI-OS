#include <net/net.h>
#include <net/net.h>

typedef char phase_d_ipv4_address_size[
    sizeof(struct net_ipv4_addr) == 4U ? 1 : -1
];
typedef char phase_d_mac_address_size[
    sizeof(struct net_mac_addr) == 6U ? 1 : -1
];
typedef char phase_d_config_size[
    sizeof(struct net_config) == 24U ? 1 : -1
];
typedef char phase_d_ping_result_size[
    sizeof(struct net_ping_result) == 16U ? 1 : -1
];
typedef char phase_d_error_values[
    NET_ERR_INVALID == -1 && NET_ERR_STATE == -2 &&
    NET_ERR_UNAVAILABLE == -3 && NET_ERR_DEVICE == -4 &&
    NET_ERR_TIMEOUT == -5 && NET_ERR_CANCELLED == -6 &&
    NET_ERR_NO_ROUTE == -7 && NET_ERR_BUSY == -8 ? 1 : -1
];
typedef char phase_d_constant_values[
    NET_TIMEOUT_MAX_MS == 2147483647 &&
    NET_ARP_CACHE_TTL_DEFAULT_MS == 60000 &&
    NET_ARP_RETRY_INTERVAL_MIN_MS == 1000 &&
    NET_ARP_RETRY_COUNT_DEFAULT == 3 &&
    NET_ICMP_ECHO_PAYLOAD_MAX == 1472 ? 1 : -1
];

int phase_d_public_header_probe(void)
{
    struct net_config config;
    struct net_ping_result result;
    struct net_ipv4_addr address;
    struct net_mac_addr mac;
    int (*parse_function)(const char *, struct net_ipv4_addr *);
    int (*init_function)(const struct net_config *);
    int (*poll_function)(unsigned int);
    int (*resolve_function)(const struct net_ipv4_addr *,
                            struct net_mac_addr *, unsigned int);
    int (*ping_function)(const struct net_ipv4_addr *, unsigned int,
                         const void *, unsigned int, unsigned int,
                         struct net_ping_result *);
    int value;

    parse_function = net_ipv4_parse;
    init_function = net_init;
    poll_function = net_poll;
    resolve_function = net_resolve_arp;
    ping_function = net_ping;
    config.address.octets[0] = 10U;
    config.netmask.octets[0] = 255U;
    config.gateway.octets[0] = 10U;
    config.arp_cache_ttl_ms = NET_ARP_CACHE_TTL_DEFAULT_MS;
    config.arp_retry_interval_ms = NET_ARP_RETRY_INTERVAL_MIN_MS;
    config.arp_retry_count = NET_ARP_RETRY_COUNT_DEFAULT;
    result.source.octets[0] = 10U;
    result.sequence = 1U;
    result.elapsed_ms = 2U;
    result.payload_length = 3U;
    address.octets[0] = config.address.octets[0];
    mac.octets[0] = 0U;
    value = parse_function != 0 && init_function != 0 && poll_function != 0;
    value = value && resolve_function != 0 && ping_function != 0;
    value = value && address.octets[0] == 10U &&
            config.netmask.octets[0] == 255U &&
            config.gateway.octets[0] == 10U && mac.octets[0] == 0U;
    value = value && config.arp_cache_ttl_ms == 60000U &&
            config.arp_retry_interval_ms == 1000U &&
            config.arp_retry_count == 3U;
    value = value && result.source.octets[0] == 10U &&
            result.sequence == 1U && result.elapsed_ms == 2U &&
            result.payload_length == 3U;
    return value ? 0 : 1;
}
