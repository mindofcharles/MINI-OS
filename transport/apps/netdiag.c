#include <stdio.h>
#include <net/raw.h>

static void print_hex_byte(unsigned int value)
{
    static const char digits[] = "0123456789abcdef";

    putchar(digits[(value >> 4) & 15U]);
    putchar(digits[value & 15U]);
}

static const char *state_name(unsigned int state)
{
    if (state == NET_DEVICE_UNAVAILABLE) return "unavailable";
    if (state == NET_DEVICE_READY) return "ready";
    if (state == NET_DEVICE_FATAL) return "fatal";
    return "unknown";
}

int main(void)
{
    struct net_device_info info;
    unsigned int index;
    int result;

    result = net_get_info(&info);
    if (result < 0) {
        printf("net_get_info failed: %d\n", result);
        return 1;
    }

    printf("NE2000 state: %s\n", state_name(info.state));
    printf("ABI version: %u\n", info.abi_version);
    printf("MAC: ");
    for (index = 0U; index < 6U; ++index) {
        if (index != 0U) putchar(':');
        print_hex_byte(info.mac[index]);
    }
    putchar('\n');
    printf("I/O base: 0x%x\n", info.io_base);
    printf("IRQ: %u\n", info.irq);
    printf("MTU: %u\n", info.mtu);
    printf("Frame bounds: %u-%u\n", info.frame_min, info.frame_max);
    printf("Driver flags: 0x%x\n", info.flags);
    printf("TX frames: %u\n", info.tx_frames);
    printf("RX frames: %u\n", info.rx_frames);
    printf("Malformed lengths: %u\n", info.malformed_lengths);
    printf("RX capacity drops: %u\n", info.rx_capacity_drops);
    printf("RX overruns: %u\n", info.rx_overruns);
    printf("DMA timeouts: %u\n", info.dma_timeouts);
    printf("TX timeouts: %u\n", info.tx_timeouts);
    printf("Reset timeouts: %u\n", info.reset_timeouts);
    printf("Resets: %u\n", info.resets);
    printf("Fatal failures: %u\n", info.fatal_failures);
    return 0;
}
