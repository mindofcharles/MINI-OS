#ifndef MINI_OS_NET_RAW_H
#define MINI_OS_NET_RAW_H

#define NET_RAW_CONST(name, value) enum { name = value };
#include "raw.def"
#undef NET_RAW_CONST

#define SYSCALL_CONST(name, value) enum { name = value };
#include "../syscall.def"
#undef SYSCALL_CONST

struct net_device_info {
    unsigned int abi_version;
    unsigned int state;
    unsigned int flags;
    unsigned int io_base;
    unsigned int irq;
    unsigned int mtu;
    unsigned int frame_min;
    unsigned int frame_max;
    unsigned char mac[6];
    unsigned char reserved[2];
    unsigned int tx_frames;
    unsigned int rx_frames;
    unsigned int malformed_lengths;
    unsigned int rx_capacity_drops;
    unsigned int rx_overruns;
    unsigned int dma_timeouts;
    unsigned int tx_timeouts;
    unsigned int reset_timeouts;
    unsigned int resets;
    unsigned int fatal_failures;
};

int net_get_info(struct net_device_info *info);
int net_send_frame(const void *frame, unsigned int length);
int net_recv_frame(void *frame, unsigned int capacity);

#endif
