#include "address.h"

#include "internal.h"

static net_u32 ipv4_value(const struct net_ipv4_addr *address)
{
    return ((net_u32)address->octets[0] << 24) |
           ((net_u32)address->octets[1] << 16) |
           ((net_u32)address->octets[2] << 8) |
           (net_u32)address->octets[3];
}

static int ipv4_is_usable_unicast(net_u32 address, net_u32 mask)
{
    net_u32 network = address & mask;
    net_u32 broadcast = network | ~mask;
    net_u8 first = (net_u8)(address >> 24);

    if (first == 0U || first == 127U || first >= 224U) {
        return 0;
    }
    return address != network && address != broadcast;
}

static int netmask_is_supported(net_u32 mask)
{
    net_u32 host_mask = ~mask;

    if (mask == 0U || host_mask < 3U) {
        return 0;
    }
    return (host_mask & (host_mask + 1U)) == 0U;
}

int net_ipv4_is_on_link_peer(const struct net_config *config,
                             const struct net_ipv4_addr *address)
{
    net_u32 mask;
    net_u32 value;
    net_u32 local;

    if (config == 0 || address == 0) {
        return 0;
    }
    mask = ipv4_value(&config->netmask);
    value = ipv4_value(address);
    local = ipv4_value(&config->address);
    return value != local &&
           (value & mask) == (local & mask) &&
           ipv4_is_usable_unicast(value, mask);
}

int net_ipv4_select_next_hop(const struct net_config *config,
                              const struct net_ipv4_addr *destination,
                              struct net_ipv4_addr *next_hop)
{
    net_u32 value;
    net_u8 first;
    net_u32 mask;
    net_u32 local;

    if (config == 0 || destination == 0 || next_hop == 0) {
        return NET_ERR_INVALID;
    }
    value = ipv4_value(destination);
    first = destination->octets[0];
    mask = ipv4_value(&config->netmask);
    local = ipv4_value(&config->address);
    if (first == 0U || first == 127U || first >= 224U ||
        value == local) {
        return NET_ERR_NO_ROUTE;
    }
    if ((value & mask) == (local & mask)) {
        if (!ipv4_is_usable_unicast(value, mask)) {
            return NET_ERR_NO_ROUTE;
        }
        *next_hop = *destination;
    } else {
        *next_hop = config->gateway;
    }
    return 0;
}

int net_ipv4_parse(const char *text, struct net_ipv4_addr *address)
{
    struct net_ipv4_addr parsed;
    unsigned int component;

    if (text == 0 || address == 0) {
        return NET_ERR_INVALID;
    }
    for (component = 0U; component < 4U; ++component) {
        unsigned int value = 0U;
        unsigned int digits = 0U;

        if (*text < '0' || *text > '9') {
            return NET_ERR_INVALID;
        }
        if (*text == '0' && text[1] >= '0' && text[1] <= '9') {
            return NET_ERR_INVALID;
        }
        while (*text >= '0' && *text <= '9') {
            value = value * 10U + (unsigned int)(*text - '0');
            ++digits;
            if (value > 255U || digits > 3U) {
                return NET_ERR_INVALID;
            }
            ++text;
        }
        parsed.octets[component] = (net_u8)value;
        if (component == 3U) {
            if (*text != '\0') {
                return NET_ERR_INVALID;
            }
        } else {
            if (*text != '.') {
                return NET_ERR_INVALID;
            }
            ++text;
        }
    }
    *address = parsed;
    return 0;
}

int net_config_validate(const struct net_config *config)
{
    net_u32 address;
    net_u32 mask;
    net_u32 gateway;

    if (config == 0) {
        return NET_ERR_INVALID;
    }
    address = ipv4_value(&config->address);
    mask = ipv4_value(&config->netmask);
    gateway = ipv4_value(&config->gateway);

    if (!netmask_is_supported(mask) ||
        !ipv4_is_usable_unicast(address, mask) ||
        !ipv4_is_usable_unicast(gateway, mask) ||
        (address & mask) != (gateway & mask) || address == gateway) {
        return NET_ERR_INVALID;
    }
    if (config->arp_cache_ttl_ms == 0U ||
        config->arp_cache_ttl_ms > NET_TIMEOUT_MAX_MS ||
        config->arp_retry_interval_ms < NET_ARP_RETRY_INTERVAL_MIN_MS ||
        config->arp_retry_interval_ms > NET_TIMEOUT_MAX_MS ||
        config->arp_retry_count == 0U) {
        return NET_ERR_INVALID;
    }
    return 0;
}

int net_mac_validate_unicast(const unsigned char *octets)
{
    unsigned int index;
    unsigned int nonzero = 0U;

    if (octets == 0 || (octets[0] & 1U) != 0U) {
        return NET_ERR_INVALID;
    }
    for (index = 0U; index < 6U; ++index) {
        nonzero |= octets[index];
    }
    return nonzero != 0U ? 0 : NET_ERR_INVALID;
}
