#include "raw.h"

#define RAW_STATIC_ASSERT(name, expression) _Static_assert((expression), #name)
#define RAW_OFFSET(type, member) __builtin_offsetof(type, member)

RAW_STATIC_ASSERT(info_size,
                  sizeof(struct net_device_info) == NET_INFO_SIZE);
RAW_STATIC_ASSERT(abi_version_offset,
                  RAW_OFFSET(struct net_device_info, abi_version) ==
                      NET_INFO_ABI_VERSION_OFFSET);
RAW_STATIC_ASSERT(state_offset,
                  RAW_OFFSET(struct net_device_info, state) ==
                      NET_INFO_STATE_OFFSET);
RAW_STATIC_ASSERT(flags_offset,
                  RAW_OFFSET(struct net_device_info, flags) ==
                      NET_INFO_FLAGS_OFFSET);
RAW_STATIC_ASSERT(io_base_offset,
                  RAW_OFFSET(struct net_device_info, io_base) ==
                      NET_INFO_IO_BASE_OFFSET);
RAW_STATIC_ASSERT(irq_offset,
                  RAW_OFFSET(struct net_device_info, irq) ==
                      NET_INFO_IRQ_OFFSET);
RAW_STATIC_ASSERT(mtu_offset,
                  RAW_OFFSET(struct net_device_info, mtu) ==
                      NET_INFO_MTU_OFFSET);
RAW_STATIC_ASSERT(frame_min_offset,
                  RAW_OFFSET(struct net_device_info, frame_min) ==
                      NET_INFO_FRAME_MIN_OFFSET);
RAW_STATIC_ASSERT(frame_max_offset,
                  RAW_OFFSET(struct net_device_info, frame_max) ==
                      NET_INFO_FRAME_MAX_OFFSET);
RAW_STATIC_ASSERT(mac_offset,
                  RAW_OFFSET(struct net_device_info, mac) ==
                      NET_INFO_MAC_OFFSET);
RAW_STATIC_ASSERT(reserved_offset,
                  RAW_OFFSET(struct net_device_info, reserved) ==
                      NET_INFO_RESERVED_OFFSET);
RAW_STATIC_ASSERT(tx_frames_offset,
                  RAW_OFFSET(struct net_device_info, tx_frames) ==
                      NET_INFO_TX_FRAMES_OFFSET);
RAW_STATIC_ASSERT(rx_frames_offset,
                  RAW_OFFSET(struct net_device_info, rx_frames) ==
                      NET_INFO_RX_FRAMES_OFFSET);
RAW_STATIC_ASSERT(malformed_lengths_offset,
                  RAW_OFFSET(struct net_device_info, malformed_lengths) ==
                      NET_INFO_MALFORMED_LENGTHS_OFFSET);
RAW_STATIC_ASSERT(rx_capacity_drops_offset,
                  RAW_OFFSET(struct net_device_info, rx_capacity_drops) ==
                      NET_INFO_RX_CAPACITY_DROPS_OFFSET);
RAW_STATIC_ASSERT(rx_overruns_offset,
                  RAW_OFFSET(struct net_device_info, rx_overruns) ==
                      NET_INFO_RX_OVERRUNS_OFFSET);
RAW_STATIC_ASSERT(dma_timeouts_offset,
                  RAW_OFFSET(struct net_device_info, dma_timeouts) ==
                      NET_INFO_DMA_TIMEOUTS_OFFSET);
RAW_STATIC_ASSERT(tx_timeouts_offset,
                  RAW_OFFSET(struct net_device_info, tx_timeouts) ==
                      NET_INFO_TX_TIMEOUTS_OFFSET);
RAW_STATIC_ASSERT(reset_timeouts_offset,
                  RAW_OFFSET(struct net_device_info, reset_timeouts) ==
                      NET_INFO_RESET_TIMEOUTS_OFFSET);
RAW_STATIC_ASSERT(resets_offset,
                  RAW_OFFSET(struct net_device_info, resets) ==
                      NET_INFO_RESETS_OFFSET);
RAW_STATIC_ASSERT(fatal_failures_offset,
                  RAW_OFFSET(struct net_device_info, fatal_failures) ==
                      NET_INFO_FATAL_FAILURES_OFFSET);

int net_get_info(struct net_device_info *info)
{
    int result;

    __asm__ __volatile__(
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_NR_NET_GET_INFO), "b"(info)
        : "memory"
    );
    return result;
}

int net_send_frame(const void *frame, unsigned int length)
{
    int result;

    __asm__ __volatile__(
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_NR_NET_SEND_FRAME), "b"(frame), "c"(length)
        : "memory"
    );
    return result;
}

int net_recv_frame(void *frame, unsigned int capacity)
{
    int result;

    __asm__ __volatile__(
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_NR_NET_RECV_FRAME), "b"(frame), "c"(capacity)
        : "memory"
    );
    return result;
}
