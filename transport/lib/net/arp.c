#include "arp.h"

#include "address.h"
#include "byteorder.h"
#include "net_platform.h"

#include <string.h>

enum {
    ARP_HARDWARE_ETHERNET = 1,
    ARP_PROTOCOL_IPV4 = 0x0800,
    ARP_HARDWARE_LENGTH = 6,
    ARP_PROTOCOL_LENGTH = 4
};

static int ipv4_equal(const struct net_ipv4_addr *left,
                      const struct net_ipv4_addr *right)
{
    return memcmp(left->octets, right->octets,
                  sizeof(left->octets)) == 0;
}

static int ipv4_zero(const struct net_ipv4_addr *address)
{
    static const unsigned char zero[4] = {0};

    return memcmp(address->octets, zero, sizeof(zero)) == 0;
}

int net_arp_parse(const struct net_ethernet_view *view,
                  struct net_arp_packet *packet)
{
    struct net_arp_packet parsed;
    const unsigned char *data;
    net_u16 operation;

    if (view == 0 || packet == 0) {
        return NET_ERR_INVALID;
    }
    if (view->payload_length < NET_ARP_PACKET_SIZE || view->payload == 0) {
        return NET_ARP_MALFORMED;
    }
    data = view->payload;
    if (net_read_be16(data) != ARP_HARDWARE_ETHERNET ||
        net_read_be16(data + 2U) != ARP_PROTOCOL_IPV4 ||
        data[4] != ARP_HARDWARE_LENGTH ||
        data[5] != ARP_PROTOCOL_LENGTH) {
        return NET_ARP_MALFORMED;
    }
    operation = net_read_be16(data + 6U);
    memcpy(parsed.sender_mac.octets, data + 8U, 6U);
    memcpy(parsed.sender_ip.octets, data + 14U, 4U);
    memcpy(parsed.target_mac.octets, data + 18U, 6U);
    memcpy(parsed.target_ip.octets, data + 24U, 4U);
    if (net_mac_validate_unicast(view->source.octets) != 0 ||
        memcmp(parsed.sender_mac.octets, view->source.octets, 6U) != 0) {
        return NET_ARP_MALFORMED;
    }
    if (operation != NET_ARP_REQUEST && operation != NET_ARP_REPLY) {
        return NET_ARP_UNSUPPORTED;
    }
    *packet = parsed;
    return (int)operation;
}

static int cache_expired(const struct net_context *context,
                         const struct net_arp_cache_entry *entry,
                         net_u32 now)
{
    return net_elapsed_ms(entry->updated_ms, now) >=
           context->config.arp_cache_ttl_ms;
}

int net_arp_cache_lookup(struct net_context *context,
                         const struct net_ipv4_addr *address,
                         struct net_mac_addr *mac)
{
    net_u32 now;
    unsigned int index;

    if (context == 0 || address == 0 || mac == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    now = net_clock_now_ms();
    for (index = 0U; index < NET_ARP_CACHE_CAPACITY; ++index) {
        struct net_arp_cache_entry *entry = &context->arp_cache[index];

        if (!entry->valid) {
            continue;
        }
        if (cache_expired(context, entry, now)) {
            entry->valid = 0;
            continue;
        }
        if (ipv4_equal(&entry->address, address)) {
            *mac = entry->mac;
            return 1;
        }
    }
    return 0;
}

int net_arp_cache_store(struct net_context *context,
                        const struct net_ipv4_addr *address,
                        const struct net_mac_addr *mac)
{
    net_u32 now;
    net_u32 oldest_age = 0U;
    unsigned int existing = NET_ARP_CACHE_CAPACITY;
    unsigned int vacant = NET_ARP_CACHE_CAPACITY;
    unsigned int oldest = NET_ARP_CACHE_CAPACITY;
    unsigned int index;
    unsigned int chosen;

    if (context == 0 || address == 0 || mac == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (!net_ipv4_is_on_link_peer(&context->config, address) ||
        net_mac_validate_unicast(mac->octets) != 0 ||
        memcmp(mac->octets, context->device_mac.octets, 6U) == 0) {
        return NET_ERR_INVALID;
    }
    now = net_clock_now_ms();
    for (index = 0U; index < NET_ARP_CACHE_CAPACITY; ++index) {
        struct net_arp_cache_entry *entry = &context->arp_cache[index];
        net_u32 age;

        if (entry->valid && ipv4_equal(&entry->address, address)) {
            existing = index;
        }
        if (entry->valid && cache_expired(context, entry, now)) {
            entry->valid = 0;
        }
        if (!entry->valid) {
            if (vacant == NET_ARP_CACHE_CAPACITY) {
                vacant = index;
            }
            continue;
        }
        age = net_elapsed_ms(entry->updated_ms, now);
        if (oldest == NET_ARP_CACHE_CAPACITY || age > oldest_age) {
            oldest = index;
            oldest_age = age;
        }
    }
    chosen = existing != NET_ARP_CACHE_CAPACITY ? existing :
             vacant != NET_ARP_CACHE_CAPACITY ? vacant : oldest;
    context->arp_cache[chosen].address = *address;
    context->arp_cache[chosen].mac = *mac;
    context->arp_cache[chosen].updated_ms = now;
    context->arp_cache[chosen].valid = 1;
    ++context->counters.arp_cache_updates;
    return 0;
}

static int send_packet(struct net_context *context,
                       const struct net_mac_addr *destination,
                       net_u16 operation,
                       const struct net_ipv4_addr *sender_ip,
                       const struct net_mac_addr *target_mac,
                       const struct net_ipv4_addr *target_ip)
{
    unsigned char *data = net_ethernet_begin_tx(context);

    if (data == 0) {
        return NET_ERR_STATE;
    }
    net_write_be16(data, ARP_HARDWARE_ETHERNET);
    net_write_be16(data + 2U, ARP_PROTOCOL_IPV4);
    data[4] = ARP_HARDWARE_LENGTH;
    data[5] = ARP_PROTOCOL_LENGTH;
    net_write_be16(data + 6U, operation);
    memcpy(data + 8U, context->device_mac.octets, 6U);
    memcpy(data + 14U, sender_ip->octets, 4U);
    memcpy(data + 18U, target_mac->octets, 6U);
    memcpy(data + 24U, target_ip->octets, 4U);
    return net_ethernet_send(context, destination, NET_ETHERTYPE_ARP,
                             NET_ARP_PACKET_SIZE);
}

int net_arp_handle(struct net_context *context,
                   const struct net_ethernet_view *view)
{
    struct net_arp_packet packet;
    int classification;
    int result;

    if (context == 0 || view == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    classification = net_arp_parse(view, &packet);
    if (classification == NET_ARP_MALFORMED) {
        ++context->counters.malformed_frames;
        return 0;
    }
    if (classification == NET_ARP_UNSUPPORTED) {
        ++context->counters.unsupported_frames;
        return 0;
    }
    if (classification < 0) {
        return classification;
    }
    if (!ipv4_equal(&packet.target_ip, &context->config.address) ||
        ipv4_equal(&packet.sender_ip, &context->config.address) ||
        memcmp(packet.sender_mac.octets,
               context->device_mac.octets, 6U) == 0) {
        return 0;
    }
    if (classification == NET_ARP_REQUEST) {
        if (!ipv4_zero(&packet.sender_ip) &&
            net_ipv4_is_on_link_peer(&context->config,
                                     &packet.sender_ip)) {
            result = net_arp_cache_store(context, &packet.sender_ip,
                                         &packet.sender_mac);
            if (result != 0) {
                return result;
            }
        }
        result = send_packet(context, &packet.sender_mac, NET_ARP_REPLY,
                             &context->config.address, &packet.sender_mac,
                             &packet.sender_ip);
        return result == 0 ? 1 : result;
    }
    if (!context->pending_arp.active ||
        context->pending_arp.attempts == 0U ||
        context->pending_arp.resolved ||
        !ipv4_equal(&packet.sender_ip,
                    &context->pending_arp.address) ||
        memcmp(packet.target_mac.octets,
               context->device_mac.octets, 6U) != 0 ||
        !net_ipv4_is_on_link_peer(&context->config,
                                  &packet.sender_ip)) {
        return 0;
    }
    result = net_arp_cache_store(context, &packet.sender_ip,
                                 &packet.sender_mac);
    if (result != 0) {
        return result;
    }
    context->pending_arp.resolved_mac = packet.sender_mac;
    context->pending_arp.resolved = 1;
    return 1;
}

int net_arp_timer(struct net_context *context, net_u32 now)
{
    static const struct net_mac_addr broadcast = {
        {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU}
    };
    static const struct net_mac_addr zero = {{0}};
    int result;

    if (context == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (!context->pending_arp.active || context->pending_arp.resolved ||
        context->pending_arp.attempts >=
            context->config.arp_retry_count) {
        return 0;
    }
    if (context->arp_request_seen &&
        net_elapsed_ms(context->arp_last_request_ms, now) <
            NET_ARP_RETRY_INTERVAL_MIN_MS) {
        return 0;
    }
    if (context->pending_arp.attempts != 0U &&
        net_elapsed_ms(context->pending_arp.last_request_ms, now) <
            context->config.arp_retry_interval_ms) {
        return 0;
    }
    context->arp_last_request_ms = now;
    context->arp_request_seen = 1;
    context->pending_arp.last_request_ms = now;
    ++context->pending_arp.attempts;
    result = send_packet(context, &broadcast, NET_ARP_REQUEST,
                         &context->config.address, &zero,
                         &context->pending_arp.address);
    return result == 0 ? 1 : result;
}
