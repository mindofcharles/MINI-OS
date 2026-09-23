#include "arp.h"

#include "address.h"
#include "net_platform.h"

#include <string.h>

/* One nonblocking receive and one timer opportunity, shared by both APIs. */
static int service_once(struct net_context *context,
                        net_u32 start_ms, unsigned int timeout_ms)
{
    struct net_ethernet_view view;
    int actions;
    int received;
    int classification;
    int result;
    net_u32 now = net_clock_now_ms();

    if (timeout_ms != 0U &&
        net_timeout_expired(start_ms, now, timeout_ms)) {
        return 0;
    }
    actions = net_arp_timer(context, now);
    if (actions < 0) {
        return actions;
    }
    received = net_recv_frame(context->rx_frame,
                              sizeof(context->rx_frame));
    if (received == SYS_ERR_UNAVAILABLE) {
        return NET_ERR_UNAVAILABLE;
    }
    if (received < 0) {
        return NET_ERR_DEVICE;
    }
    if (received == 0) {
        return actions;
    }
    classification = net_ethernet_decode(context,
                                          (unsigned int)received, &view);
    if (classification < 0) {
        return classification;
    }
    if (net_cancel_requested() ||
        (timeout_ms != 0U &&
         net_timeout_expired(start_ms, net_clock_now_ms(),
                             timeout_ms))) {
        return actions;
    }
    if (classification == NET_ETHERNET_ARP) {
        result = net_arp_handle(context, &view);
        if (result < 0) {
            return result;
        }
        actions += result;
    } else if (classification == NET_ETHERNET_IPV4) {
        /* IPv4 processing is added in D5; classification is not delivery. */
        ++context->counters.unsupported_frames;
    }
    return actions;
}

static int poll_active(struct net_context *context,
                       unsigned int timeout_ms)
{
    net_u32 started_ms;
    int status;
    int result;

    started_ms = net_clock_now_ms();
    for (;;) {
        if (net_cancel_requested()) {
            return NET_ERR_CANCELLED;
        }
        if (timeout_ms != 0U &&
            net_timeout_expired(started_ms, net_clock_now_ms(),
                                timeout_ms)) {
            return 0;
        }
        result = service_once(context, started_ms, timeout_ms);
        if (result < 0) {
            return result;
        }
        if (net_cancel_requested()) {
            return NET_ERR_CANCELLED;
        }
        if (result > 0 || timeout_ms == 0U) {
            return result;
        }
        if (timeout_ms != 0U &&
            net_timeout_expired(started_ms, net_clock_now_ms(),
                                timeout_ms)) {
            return 0;
        }
        status = net_wait_status(started_ms, net_clock_now_ms(),
                                 timeout_ms);
        if (status == NET_WAIT_CANCELLED) {
            return NET_ERR_CANCELLED;
        }
        if (status == NET_WAIT_TIMEOUT) {
            return 0;
        }
    }
}

int net_poll(unsigned int timeout_ms)
{
    struct net_context *context = &net_global_context;
    int result;

    if (!net_timeout_valid(timeout_ms)) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (context->service_active || context->pending_arp.active ||
        context->pending_echo.active) {
        return NET_ERR_BUSY;
    }
    context->service_active = 1;
    result = poll_active(context, timeout_ms);
    context->service_active = 0;
    return result;
}

static int resolve_active(struct net_context *context,
                          const struct net_ipv4_addr *address,
                          struct net_mac_addr *mac,
                          unsigned int timeout_ms)
{
    struct net_mac_addr cached;
    net_u32 started_ms;
    int result;

    if (!net_ipv4_is_on_link_peer(&context->config, address)) {
        return NET_ERR_NO_ROUTE;
    }
    if (net_cancel_requested()) {
        return NET_ERR_CANCELLED;
    }
    result = net_arp_cache_lookup(context, address, &cached);
    if (result < 0) {
        return result;
    }
    if (result == 1) {
        *mac = cached;
        return 0;
    }

    memset(&context->pending_arp, 0, sizeof(context->pending_arp));
    context->pending_arp.address = *address;
    context->pending_arp.active = 1;
    started_ms = net_clock_now_ms();
    for (;;) {
        if (net_cancel_requested()) {
            result = NET_ERR_CANCELLED;
            break;
        }
        if (timeout_ms != 0U &&
            net_timeout_expired(started_ms, net_clock_now_ms(),
                                timeout_ms)) {
            result = NET_ERR_TIMEOUT;
            break;
        }
        result = service_once(context, started_ms, timeout_ms);
        if (result < 0) {
            break;
        }
        if (net_cancel_requested()) {
            result = NET_ERR_CANCELLED;
            break;
        }
        if (context->pending_arp.resolved) {
            *mac = context->pending_arp.resolved_mac;
            result = 0;
            break;
        }
        if (timeout_ms != 0U &&
            net_timeout_expired(started_ms, net_clock_now_ms(),
                                timeout_ms)) {
            result = NET_ERR_TIMEOUT;
            break;
        }
        if (timeout_ms == 0U ||
            (context->pending_arp.attempts >=
                 context->config.arp_retry_count &&
             net_elapsed_ms(context->pending_arp.last_request_ms,
                            net_clock_now_ms()) >=
                 context->config.arp_retry_interval_ms)) {
            result = NET_ERR_TIMEOUT;
            break;
        }
    }
    memset(&context->pending_arp, 0, sizeof(context->pending_arp));
    return result;
}

int net_resolve_arp(const struct net_ipv4_addr *address,
                    struct net_mac_addr *mac,
                    unsigned int timeout_ms)
{
    struct net_context *context = &net_global_context;
    int result;

    if (address == 0 || mac == 0 || !net_timeout_valid(timeout_ms)) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (context->service_active || context->pending_arp.active ||
        context->pending_echo.active) {
        return NET_ERR_BUSY;
    }
    context->service_active = 1;
    result = resolve_active(context, address, mac, timeout_ms);
    context->service_active = 0;
    return result;
}
