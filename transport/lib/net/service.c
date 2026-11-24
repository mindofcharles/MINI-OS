#include "service.h"

#include "arp.h"
#include "ipv4.h"

#include <string.h>

int net_transport_register(struct net_context *context,
                            const struct net_transport_binding *binding)
{
    if (context == 0 || binding == 0 || binding->owner == 0 ||
        binding->protocol != NET_IPV4_PROTOCOL_TCP || binding->input == 0 ||
        binding->timer == 0 || binding->failure == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (context->service_active || context->transport.owner != 0) {
        return NET_ERR_BUSY;
    }
    context->transport = *binding;
    return 0;
}

int net_transport_unregister(struct net_context *context, void *owner)
{
    if (context == 0 || owner == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized || context->transport.owner != owner) {
        return NET_ERR_STATE;
    }
    if (context->service_active ||
        (context->pending_arp.active &&
         context->pending_arp.owner != NET_ARP_OWNER_SYNC)) {
        return NET_ERR_BUSY;
    }
    memset(&context->transport, 0, sizeof(context->transport));
    return 0;
}

int net_service_acquire(struct net_context *context)
{
    if (context == 0) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized) {
        return NET_ERR_STATE;
    }
    if (context->service_active || context->pending_echo.active ||
        (context->pending_arp.active &&
         (context->pending_arp.owner == NET_ARP_OWNER_SYNC ||
          context->transport.owner == 0 ||
          context->transport.arp_owner != context->pending_arp.owner))) {
        return NET_ERR_BUSY;
    }
    context->service_active = 1;
    return 0;
}

void net_service_release(struct net_context *context)
{
    context->service_active = 0;
}

int net_service_fail(struct net_context *context, int result)
{
    if (context->transport.owner != 0) {
        context->transport.failure(context->transport.owner, result);
    }
    return result;
}

static int finish_receive(struct net_context *context, int actions)
{
    int result;

    if (context->transport.after_receive == 0) {
        return actions;
    }
    result = context->transport.after_receive(context->transport.owner);
    return result < 0 ? net_service_fail(context, result) :
           actions + (result != 0);
}

/* At most one raw receive and bounded timer/dispatch work under one guard. */
int net_service_once(struct net_context *context, net_u32 started_ms,
                       unsigned int wait_ms)
{
    struct net_ethernet_view view;
    int actions = 0;
    int received;
    int classification;
    int result;
    net_u32 now;

    if (context == 0 || !net_timeout_valid(wait_ms)) {
        return NET_ERR_INVALID;
    }
    if (!context->initialized || !context->service_active) {
        return NET_ERR_STATE;
    }
    now = net_clock_now_ms();
    if (wait_ms != 0U && net_timeout_expired(started_ms, now, wait_ms)) {
        return 0;
    }
    /* D's synchronous wrappers already check before entering this step. */
    if (context->transport.owner != 0 && net_cancel_requested()) {
        return net_service_fail(context, NET_ERR_CANCELLED);
    }
    if (context->transport.owner != 0) {
        result = context->transport.timer(context->transport.owner, now);
        if (result < 0) {
            return net_service_fail(context, result);
        }
        actions += result != 0;
    }
    if (net_service_io_expired(context)) {
        return actions;
    }
    result = net_arp_timer(context, now);
    if (result < 0) {
        return net_service_fail(context, result);
    }
    actions += result;
    if (net_service_io_expired(context)) {
        return actions;
    }
    received = context->rx_deferred_length != 0U ? (int)context->rx_deferred_length :
               net_recv_frame(context->rx_frame, sizeof(context->rx_frame));
    if (received == SYS_ERR_UNAVAILABLE) {
        return net_service_fail(context, NET_ERR_UNAVAILABLE);
    }
    /* The raw driver returns these receive errors only after recovery. */
    if (received == SYS_ERR_OVERRUN || received == SYS_ERR_TIMEOUT) {
        received = 0;
    }
    if (received < 0) {
        return net_service_fail(context, NET_ERR_DEVICE);
    }
    if (context->transport.owner != 0 && net_cancel_requested()) {
        return net_service_fail(context, NET_ERR_CANCELLED);
    }
    if (received == 0) {
        return finish_receive(context, actions);
    }
    classification = net_ethernet_decode(context, (unsigned int)received,
                                          &view);
    if (classification < 0) {
        return net_service_fail(context, classification);
    }
    if (context->transport.owner == 0 && net_cancel_requested()) {
        context->rx_deferred_length = 0U;
        return net_service_fail(context, NET_ERR_CANCELLED);
    }
    /* An owned transport applies its own protocol deadline to a received frame. */
    if (wait_ms != 0U && context->transport.arp_owner == NET_ARP_OWNER_SYNC &&
        net_timeout_expired(started_ms, net_clock_now_ms(), wait_ms)) {
        if (context->rx_deferred_length != 0U) {
            /* Count a saved request only once, when it is dispatched. */
            --context->counters.received_frames;
        }
        return finish_receive(context, actions);
    }
    context->rx_deferred_length = 0U;
    if (classification == NET_ETHERNET_ARP) {
        result = net_arp_handle(context, &view);
    } else if (classification == NET_ETHERNET_IPV4) {
        result = net_ipv4_handle(context, &view);
    } else {
        result = 0;
    }
    if (result == NET_ETHERNET_TX_DEFERRED) {
        /* ARP and ICMP requests can be redispatched without side effects.
         * TCP copied its data and retains pending output in its own state. */
        context->rx_deferred_length = (unsigned int)received;
        --context->counters.received_frames;
        return finish_receive(context, actions);
    }
    return result < 0 ? net_service_fail(context, result) :
           finish_receive(context, actions + (result != 0));
}

int net_service_io_once(struct net_context *context, net_u32 started_ms,
                          unsigned int wait_ms)
{
    int result;

    if (context == 0 || !net_timeout_valid(wait_ms)) {
        return NET_ERR_INVALID;
    }
    context->io_started_ms = started_ms;
    context->io_wait_ms = wait_ms;
    result = net_service_once(context, started_ms, wait_ms);
    context->io_wait_ms = 0U;
    return result;
}
