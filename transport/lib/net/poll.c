#include "arp.h"

#include "address.h"
#include "icmp.h"
#include "ipv4.h"
#include "net_platform.h"
#include "service.h"

#include <string.h>

static int poll_active(struct net_context *context,
                       unsigned int timeout_ms)
{
    net_u32 started_ms;
    int status;
    int result;

    started_ms = net_clock_now_ms();
    for (;;) {
        if (net_cancel_requested()) {
            return net_service_fail(context, NET_ERR_CANCELLED);
        }
        if (timeout_ms != 0U &&
            net_timeout_expired(started_ms, net_clock_now_ms(),
                                timeout_ms)) {
            return 0;
        }
        result = net_service_once(context, started_ms, timeout_ms);
        if (result < 0) {
            return result;
        }
        if (net_cancel_requested()) {
            return net_service_fail(context, NET_ERR_CANCELLED);
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
            return net_service_fail(context, NET_ERR_CANCELLED);
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
    result = net_service_acquire(context);
    if (result != 0) {
        return result;
    }
    result = poll_active(context, timeout_ms);
    net_service_release(context);
    return result;
}

static int resolve_active(struct net_context *context,
                          const struct net_ipv4_addr *address,
                          struct net_mac_addr *mac,
                          net_u32 started_ms, unsigned int timeout_ms,
                          int *serviced)
{
    int result;

    *serviced = 0;
    if (!net_ipv4_is_on_link_peer(&context->config, address)) {
        return NET_ERR_NO_ROUTE;
    }
    if (net_cancel_requested()) {
        return net_service_fail(context, NET_ERR_CANCELLED);
    }
    result = net_arp_task_start(context, address, NET_ARP_OWNER_SYNC,
                                started_ms,
                                timeout_ms != 0U ? timeout_ms :
                                (unsigned int)NET_TIMEOUT_MAX_MS);
    if (result < 0) {
        /* Start can install a timed-out task before returning its error. */
        goto done;
    }
    if (result == 0) {
        result = net_arp_task_result(context, NET_ARP_OWNER_SYNC, mac);
        goto done;
    }
    for (;;) {
        if (net_cancel_requested()) {
            result = net_service_fail(context, NET_ERR_CANCELLED);
            break;
        }
        if (timeout_ms != 0U &&
            net_timeout_expired(started_ms, net_clock_now_ms(),
                                timeout_ms)) {
            result = NET_ERR_TIMEOUT;
            break;
        }
        result = net_service_once(context, started_ms, timeout_ms);
        *serviced = 1;
        if (result < 0) {
            break;
        }
        if (net_cancel_requested()) {
            result = net_service_fail(context, NET_ERR_CANCELLED);
            break;
        }
        result = net_arp_task_result(context, NET_ARP_OWNER_SYNC, mac);
        if (result != NET_ARP_TASK_PENDING) {
            break;
        }
        if (timeout_ms != 0U &&
            net_timeout_expired(started_ms, net_clock_now_ms(),
                                timeout_ms)) {
            result = NET_ERR_TIMEOUT;
            break;
        }
        if (timeout_ms == 0U) {
            result = NET_ERR_TIMEOUT;
            break;
        }
    }
done:
    (void)net_arp_task_finish(context, NET_ARP_OWNER_SYNC);
    return result;
}

int net_resolve_arp(const struct net_ipv4_addr *address,
                    struct net_mac_addr *mac,
                    unsigned int timeout_ms)
{
    struct net_context *context = &net_global_context;
    int serviced;
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
    result = resolve_active(context, address, mac,
                            net_clock_now_ms(), timeout_ms, &serviced);
    context->service_active = 0;
    return result;
}

int net_ping(const struct net_ipv4_addr *address,
             unsigned int sequence, const void *payload,
             unsigned int payload_length, unsigned int timeout_ms,
             struct net_ping_result *output)
{
    struct net_context *context = &net_global_context;
    struct net_ipv4_addr next_hop;
    struct net_mac_addr next_hop_mac;
    struct net_ping_result completed;
    net_u32 started_ms;
    int serviced;
    int result;

    if (address == 0 || output == 0 || sequence > 0xFFFFU ||
        payload_length > NET_ICMP_ECHO_PAYLOAD_MAX ||
        (payload_length != 0U && payload == 0) ||
        !net_timeout_valid(timeout_ms)) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (context->service_active || context->pending_arp.active ||
        context->pending_echo.active) {
        return NET_ERR_BUSY;
    }
    started_ms = net_clock_now_ms();
    result = net_ipv4_select_next_hop(&context->config, address,
                                       &next_hop);
    if (result != 0) {
        return result;
    }
    context->service_active = 1;
    result = resolve_active(context, &next_hop, &next_hop_mac,
                            started_ms, timeout_ms, &serviced);
    if (result != 0) {
        goto done;
    }
    if ((timeout_ms == 0U && serviced) ||
        (timeout_ms != 0U &&
         net_timeout_expired(started_ms, net_clock_now_ms(),
                             timeout_ms))) {
        result = NET_ERR_TIMEOUT;
        goto done;
    }
    if (net_cancel_requested()) {
        result = net_service_fail(context, NET_ERR_CANCELLED);
        goto done;
    }

    memset(&context->pending_echo, 0, sizeof(context->pending_echo));
    context->pending_echo.destination = *address;
    context->pending_echo.payload = payload;
    context->pending_echo.started_ms = started_ms;
    context->pending_echo.timeout_ms = timeout_ms;
    context->pending_echo.payload_length = payload_length;
    context->pending_echo.identifier =
        ++context->next_echo_identifier;
    context->pending_echo.sequence = (net_u16)sequence;
    context->pending_echo.active = 1;
    result = net_icmp_send_request(context, address, &next_hop_mac,
                                    context->pending_echo.identifier,
                                    context->pending_echo.sequence,
                                    payload, payload_length);
    if (result != 0) {
        result = net_service_fail(context, result);
        goto done;
    }
    for (;;) {
        if (net_cancel_requested()) {
            result = net_service_fail(context, NET_ERR_CANCELLED);
            break;
        }
        if (timeout_ms != 0U &&
            net_timeout_expired(started_ms, net_clock_now_ms(),
                                timeout_ms)) {
            result = NET_ERR_TIMEOUT;
            break;
        }
        result = net_service_once(context, started_ms, timeout_ms);
        if (result < 0) {
            break;
        }
        if (net_cancel_requested()) {
            result = net_service_fail(context, NET_ERR_CANCELLED);
            break;
        }
        if (context->pending_echo.matched) {
            completed.source = context->pending_echo.reply_source;
            completed.sequence = sequence;
            completed.elapsed_ms = context->pending_echo.elapsed_ms;
            completed.payload_length = payload_length;
            *output = completed;
            result = 0;
            break;
        }
        if (timeout_ms == 0U ||
            net_timeout_expired(started_ms, net_clock_now_ms(),
                                timeout_ms)) {
            result = NET_ERR_TIMEOUT;
            break;
        }
    }
done:
    memset(&context->pending_echo, 0, sizeof(context->pending_echo));
    context->service_active = 0;
    return result;
}
