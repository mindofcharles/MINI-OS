#ifndef MINI_OS_NET_SERVICE_H
#define MINI_OS_NET_SERVICE_H

#include "internal.h"

/* These helpers are private; callbacks never call public APIs recursively. */
int net_transport_register(struct net_context *context,
                            const struct net_transport_binding *binding);
int net_transport_unregister(struct net_context *context, void *owner);
int net_service_acquire(struct net_context *context);
void net_service_release(struct net_context *context);
int net_service_fail(struct net_context *context, int result);
int net_service_once(struct net_context *context, net_u32 started_ms,
                       unsigned int wait_ms);

#endif
