#include <stdio.h>
#include <string.h>
#include <platform.h>
#include <net/raw.h>

#define TEST_ETHERTYPE_HIGH 0x88U
#define TEST_ETHERTYPE_DATA 0xB5U
#define TEST_ETHERTYPE_ACK 0xB6U
#define RX_ODD_SEQUENCE 0xE1U
#define RX_ODD_LENGTH 61U
#define RX_TEST_LENGTH 1000U
#define RX_TEST_COUNT 32U
#define WAIT_TIMEOUT_MS 5000U
#define BASE_DRIVER_FLAGS \
    (NET_DRIVER_FLAG_POLLING | NET_DRIVER_FLAG_IRQ_MASKED | \
     NET_DRIVER_FLAG_FCS_STRIPPED | NET_DRIVER_FLAG_SYNC_TX)
#define READY_DRIVER_FLAGS (BASE_DRIVER_FLAGS | NET_DRIVER_FLAG_AVAILABLE)

static unsigned char frame[NET_FRAME_MAX + 2];

static int fail(const char *message)
{
    printf("NETWORK TEST FAIL: %s\n", message);
    return 1;
}

static int wait_for_frame(unsigned int capacity)
{
    unsigned int start;
    int result;

    start = clock_monotonic_ms();
    do {
        result = net_recv_frame(frame, capacity);
        if (result != 0) return result;
    } while ((unsigned int)(clock_monotonic_ms() - start) < WAIT_TIMEOUT_MS);
    return SYS_ERR_TIMEOUT;
}

static int check_ready_info(const struct net_device_info *info)
{
    static const unsigned char expected_mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
    };

    return info->abi_version == NET_RAW_ABI_VERSION &&
           info->state == NET_DEVICE_READY &&
           info->io_base == 0x300U && info->irq == 9U &&
           info->mtu == NET_IPV4_MTU &&
           info->frame_min == NET_FRAME_MIN &&
           info->frame_max == NET_FRAME_MAX &&
           info->flags == READY_DRIVER_FLAGS &&
           info->reserved[0] == 0U && info->reserved[1] == 0U &&
           memcmp(info->mac, expected_mac, 6U) == 0;
}

static int test_info(int expect_ready)
{
    struct net_device_info info;
    struct net_device_info after;
    int result;

    memset(&info, 0xA5, sizeof(info));
    if (net_get_info(NULL) != SYS_ERR_INVALID ||
        net_send_frame(NULL, NET_FRAME_MIN) != SYS_ERR_INVALID ||
        net_recv_frame(NULL, NET_FRAME_MAX) != SYS_ERR_INVALID ||
        net_recv_frame(frame, NET_FRAME_MAX + 1U) != SYS_ERR_RANGE) {
        return fail("argument validation");
    }
    result = net_get_info(&info);
    if (result != 0 || info.abi_version != NET_RAW_ABI_VERSION ||
        info.io_base != 0x300U || info.irq != 9U ||
        info.mtu != NET_IPV4_MTU || info.frame_min != NET_FRAME_MIN ||
        info.frame_max != NET_FRAME_MAX) {
        return fail("device information");
    }
    memset(frame, 0, sizeof(frame));
    if (net_send_frame(frame, NET_FRAME_MIN - 1U) != SYS_ERR_RANGE ||
        net_send_frame(frame, NET_FRAME_MAX + 1U) != SYS_ERR_RANGE) {
        return fail("transmit length validation");
    }
    if (net_get_info(&after) != 0 ||
        after.malformed_lengths != info.malformed_lengths + 2U) {
        return fail("malformed-length counter");
    }
    if (expect_ready) {
        if (!check_ready_info(&info)) return fail("ready information");
        printf("NETWORK INFO TEST: PASS\n");
    }
    else {
        if (info.state != NET_DEVICE_UNAVAILABLE ||
            info.flags != BASE_DRIVER_FLAGS ||
            net_send_frame(frame, NET_FRAME_MIN) != SYS_ERR_UNAVAILABLE ||
            net_recv_frame(frame, NET_FRAME_MAX) != SYS_ERR_UNAVAILABLE) {
            return fail("unavailable device contract");
        }
        printf("NETWORK UNAVAILABLE TEST: PASS\n");
    }
    return 0;
}

static void make_tx_frame(unsigned int length, unsigned int seed)
{
    static const unsigned char destination[6] = {
        0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
    };
    static const unsigned char source[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
    };
    unsigned int index;

    for (index = 0U; index < length; ++index) {
        frame[index] = (unsigned char)((seed + index) & 0xFFU);
    }
    memcpy(frame, destination, 6U);
    memcpy(frame + 6U, source, 6U);
    frame[12] = TEST_ETHERTYPE_HIGH;
    frame[13] = TEST_ETHERTYPE_DATA;
    frame[14] = (unsigned char)seed;
}

static int test_tx(void)
{
    static const unsigned int lengths[] = { 60U, 61U, 1514U };
    struct net_device_info before;
    struct net_device_info after;
    unsigned int index;
    int result;

    if (net_get_info(&before) != 0) return fail("transmit counters before");
    for (index = 0U; index < 3U; ++index) {
        make_tx_frame(lengths[index], 0x20U + index);
        result = net_send_frame(frame, lengths[index]);
        if (result != (int)lengths[index]) return fail("transmit result");
        memset(frame, 0xCC, lengths[index]);
    }
    if (net_get_info(&after) != 0 ||
        after.tx_frames != before.tx_frames + 3U) {
        return fail("transmit counters after");
    }
    printf("NETWORK TX TEST: PASS\n");
    return 0;
}

static int validate_rx_frame(unsigned int sequence, unsigned int length,
                             unsigned int expected_length)
{
    static const unsigned char destination[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
    };
    static const unsigned char source[6] = {
        0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
    };
    unsigned int index;

    if (length != expected_length ||
        memcmp(frame, destination, 6U) != 0 ||
        memcmp(frame + 6U, source, 6U) != 0 ||
        frame[12] != TEST_ETHERTYPE_HIGH ||
        frame[13] != TEST_ETHERTYPE_DATA ||
        frame[14] != (unsigned char)sequence) {
        return 0;
    }
    for (index = 15U; index < length; ++index) {
        if (frame[index] != (unsigned char)((sequence + index) & 0xFFU)) {
            return 0;
        }
    }
    return 1;
}

static int send_ack(unsigned int sequence)
{
    static const unsigned char destination[6] = {
        0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
    };
    static const unsigned char source[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
    };

    memset(frame, 0, NET_FRAME_MIN);
    memcpy(frame, destination, 6U);
    memcpy(frame + 6U, source, 6U);
    frame[12] = TEST_ETHERTYPE_HIGH;
    frame[13] = TEST_ETHERTYPE_ACK;
    frame[14] = (unsigned char)sequence;
    return net_send_frame(frame, NET_FRAME_MIN);
}

static int test_rx(void)
{
    struct net_device_info before;
    struct net_device_info after;
    unsigned int sequence;
    int result;

    if (net_get_info(&before) != 0 || !check_ready_info(&before)) {
        return fail("receive precondition");
    }
    printf("NETWORK RX READY\n");
    result = wait_for_frame(NET_FRAME_MIN);
    if (result != SYS_ERR_RANGE) return fail("capacity drop result");
    printf("NETWORK RX DROP: PASS\n");

    result = wait_for_frame(NET_FRAME_MAX);
    if (result < 0 ||
        !validate_rx_frame(RX_ODD_SEQUENCE, (unsigned int)result,
                           RX_ODD_LENGTH)) {
        return fail("odd-length received frame contents");
    }
    if (send_ack(RX_ODD_SEQUENCE) != NET_FRAME_MIN) {
        return fail("odd-length receive acknowledgement");
    }
    printf("NETWORK RX ODD: PASS\n");

    for (sequence = 0U; sequence < RX_TEST_COUNT; ++sequence) {
        result = wait_for_frame(NET_FRAME_MAX);
        if (result < 0 ||
            !validate_rx_frame(sequence, (unsigned int)result,
                               RX_TEST_LENGTH)) {
            return fail("received frame contents");
        }
        if (send_ack(sequence) != NET_FRAME_MIN) {
            return fail("receive acknowledgement");
        }
    }
    if (net_get_info(&after) != 0 ||
        after.rx_capacity_drops != before.rx_capacity_drops + 1U ||
        after.rx_frames != before.rx_frames + RX_TEST_COUNT + 1U ||
        after.tx_frames != before.tx_frames + RX_TEST_COUNT + 1U ||
        after.rx_overruns != before.rx_overruns ||
        after.malformed_lengths != before.malformed_lengths) {
        return fail("receive counters");
    }
    printf("NETWORK RX WRAP TEST: PASS\n");
    return 0;
}

static int test_recovery(void)
{
    struct net_device_info before;
    struct net_device_info after;
    int result;

    if (net_get_info(&before) != 0 || !check_ready_info(&before)) {
        return fail("recovery precondition");
    }
    result = net_recv_frame(frame, NET_FRAME_MAX);
    if (result != SYS_ERR_OVERRUN) return fail("injected overrun");
    make_tx_frame(NET_FRAME_MIN, 0x70U);
    if (net_send_frame(frame, NET_FRAME_MIN) != SYS_ERR_TIMEOUT) {
        return fail("injected DMA timeout");
    }
    make_tx_frame(NET_FRAME_MIN, 0x71U);
    if (net_send_frame(frame, NET_FRAME_MIN) != SYS_ERR_TIMEOUT) {
        return fail("injected transmit timeout");
    }
    make_tx_frame(NET_FRAME_MIN, 0x72U);
    if (net_send_frame(frame, NET_FRAME_MIN) != NET_FRAME_MIN) {
        return fail("post-reset transmit");
    }
    if (net_get_info(&after) != 0 || !check_ready_info(&after) ||
        after.rx_overruns != before.rx_overruns + 1U ||
        after.dma_timeouts != before.dma_timeouts + 1U ||
        after.tx_timeouts != before.tx_timeouts + 1U ||
        after.reset_timeouts != before.reset_timeouts ||
        after.resets != before.resets + 3U ||
        after.fatal_failures != before.fatal_failures) {
        return fail("recovery counters or state");
    }
    printf("NETWORK RECOVERY TEST: PASS\n");
    return 0;
}

static int test_fatal_recovery(void)
{
    struct net_device_info before;
    struct net_device_info after;

    if (net_get_info(&before) != 0 || !check_ready_info(&before)) {
        return fail("fatal recovery precondition");
    }
    make_tx_frame(NET_FRAME_MIN, 0x80U);
    if (net_send_frame(frame, NET_FRAME_MIN) != SYS_ERR_RESET) {
        return fail("injected reset failure");
    }
    if (net_get_info(&after) != 0 ||
        after.state != NET_DEVICE_FATAL ||
        after.flags != BASE_DRIVER_FLAGS ||
        after.tx_timeouts != before.tx_timeouts + 1U ||
        after.reset_timeouts != before.reset_timeouts + 1U ||
        after.resets != before.resets + 1U ||
        after.fatal_failures != before.fatal_failures + 1U ||
        net_send_frame(frame, NET_FRAME_MIN) != SYS_ERR_DEVICE ||
        net_recv_frame(frame, NET_FRAME_MAX) != SYS_ERR_DEVICE) {
        return fail("stable fatal state");
    }
    printf("NETWORK FATAL RECOVERY TEST: PASS\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) return fail("expected one mode");
    if (strcmp(argv[1], "info") == 0) return test_info(1);
    if (strcmp(argv[1], "unavailable") == 0) return test_info(0);
    if (strcmp(argv[1], "tx") == 0) return test_tx();
    if (strcmp(argv[1], "rx") == 0) return test_rx();
    if (strcmp(argv[1], "recovery") == 0) return test_recovery();
    if (strcmp(argv[1], "fatal") == 0) return test_fatal_recovery();
    return fail("unknown mode");
}
