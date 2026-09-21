#include "internal.h"

#include "address.h"

#include <string.h>

_Static_assert(sizeof(struct net_context) >= NET_FRAME_MAX * 2U,
               "network context must contain two complete frame buffers");
_Static_assert(sizeof(struct net_context) <= 4096U,
               "Phase D network context must remain within 4 KiB");

struct net_context net_global_context;

static int validate_device(const struct net_device_info *info)
{
    unsigned int required_flags = NET_DRIVER_FLAG_POLLING |
                                  NET_DRIVER_FLAG_IRQ_MASKED |
                                  NET_DRIVER_FLAG_FCS_STRIPPED |
                                  NET_DRIVER_FLAG_SYNC_TX;

    if (info->abi_version != NET_RAW_ABI_VERSION) {
        return NET_ERR_DEVICE;
    }
    if (info->state == NET_DEVICE_UNAVAILABLE) {
        return NET_ERR_UNAVAILABLE;
    }
    if (info->state != NET_DEVICE_READY) {
        return NET_ERR_DEVICE;
    }
    if ((info->flags & NET_DRIVER_FLAG_AVAILABLE) == 0U) {
        return NET_ERR_UNAVAILABLE;
    }
    if ((info->flags & required_flags) != required_flags ||
        info->mtu != NET_IPV4_MTU || info->frame_min != NET_FRAME_MIN ||
        info->frame_max != NET_FRAME_MAX ||
        net_mac_validate_unicast(info->mac) != 0) {
        return NET_ERR_DEVICE;
    }
    return 0;
}

int net_init(const struct net_config *config)
{
    struct net_config committed_config;
    struct net_device_info info;
    int result;

    if (config == 0) {
        return NET_ERR_INVALID;
    }
    committed_config = *config;
    result = net_config_validate(&committed_config);
    if (result != 0) {
        return result;
    }
    if (net_global_context.initialized) {
        return NET_ERR_STATE;
    }

    memset(&info, 0, sizeof(info));
    result = net_get_info(&info);
    if (result == SYS_ERR_UNAVAILABLE) {
        return NET_ERR_UNAVAILABLE;
    }
    if (result != 0) {
        return NET_ERR_DEVICE;
    }
    result = validate_device(&info);
    if (result != 0) {
        return result;
    }

    memset(&net_global_context, 0, sizeof(net_global_context));
    net_global_context.config = committed_config;
    memcpy(net_global_context.device_mac.octets, info.mac,
           sizeof(net_global_context.device_mac.octets));
    net_global_context.initialized = 1;
    return 0;
}
