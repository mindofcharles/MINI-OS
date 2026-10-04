#include <net/raw.h>

#define ABI_OFFSET(type, member) __builtin_offsetof(type, member)

int main(void)
{
    if (sizeof(struct net_device_info) != NET_INFO_SIZE ||
        ABI_OFFSET(struct net_device_info, abi_version) !=
            NET_INFO_ABI_VERSION_OFFSET ||
        ABI_OFFSET(struct net_device_info, state) != NET_INFO_STATE_OFFSET ||
        ABI_OFFSET(struct net_device_info, flags) != NET_INFO_FLAGS_OFFSET ||
        ABI_OFFSET(struct net_device_info, io_base) != NET_INFO_IO_BASE_OFFSET ||
        ABI_OFFSET(struct net_device_info, irq) != NET_INFO_IRQ_OFFSET ||
        ABI_OFFSET(struct net_device_info, mtu) != NET_INFO_MTU_OFFSET ||
        ABI_OFFSET(struct net_device_info, frame_min) !=
            NET_INFO_FRAME_MIN_OFFSET ||
        ABI_OFFSET(struct net_device_info, frame_max) !=
            NET_INFO_FRAME_MAX_OFFSET ||
        ABI_OFFSET(struct net_device_info, mac) != NET_INFO_MAC_OFFSET ||
        ABI_OFFSET(struct net_device_info, reserved) !=
            NET_INFO_RESERVED_OFFSET ||
        ABI_OFFSET(struct net_device_info, tx_frames) !=
            NET_INFO_TX_FRAMES_OFFSET ||
        ABI_OFFSET(struct net_device_info, rx_frames) !=
            NET_INFO_RX_FRAMES_OFFSET ||
        ABI_OFFSET(struct net_device_info, malformed_lengths) !=
            NET_INFO_MALFORMED_LENGTHS_OFFSET ||
        ABI_OFFSET(struct net_device_info, rx_capacity_drops) !=
            NET_INFO_RX_CAPACITY_DROPS_OFFSET ||
        ABI_OFFSET(struct net_device_info, rx_overruns) !=
            NET_INFO_RX_OVERRUNS_OFFSET ||
        ABI_OFFSET(struct net_device_info, dma_timeouts) !=
            NET_INFO_DMA_TIMEOUTS_OFFSET ||
        ABI_OFFSET(struct net_device_info, tx_timeouts) !=
            NET_INFO_TX_TIMEOUTS_OFFSET ||
        ABI_OFFSET(struct net_device_info, reset_timeouts) !=
            NET_INFO_RESET_TIMEOUTS_OFFSET ||
        ABI_OFFSET(struct net_device_info, resets) != NET_INFO_RESETS_OFFSET ||
        ABI_OFFSET(struct net_device_info, fatal_failures) !=
            NET_INFO_FATAL_FAILURES_OFFSET) {
        return 1;
    }
    return 0;
}
