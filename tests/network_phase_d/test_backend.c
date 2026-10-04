#include "deterministic_backend.h"

#include "../../transport/lib/net/internal.h"
#include "../../transport/lib/net/net_platform.h"

#include <stdio.h>
#include <string.h>

struct cancel_state {
    unsigned int calls;
    unsigned int cancel_after;
};

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Phase D backend test failed: %s\n", message);
        return 0;
    }
    return 1;
}

static int cancel_after_calls(void *context)
{
    struct cancel_state *state = context;

    ++state->calls;
    return state->calls >= state->cancel_after;
}

static void fill_frame(unsigned char *frame, unsigned int length,
                       unsigned int seed)
{
    unsigned int index;

    for (index = 0U; index < length; ++index) {
        frame[index] = (unsigned char)((seed + index) & 0xFFU);
    }
}

static int test_device(void)
{
    static const unsigned char expected_mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
    };
    struct net_device_info info;
    struct net_device_info queried;
    unsigned int expected_flags;

    expected_flags = NET_DRIVER_FLAG_POLLING |
                     NET_DRIVER_FLAG_IRQ_MASKED |
                     NET_DRIVER_FLAG_FCS_STRIPPED |
                     NET_DRIVER_FLAG_SYNC_TX |
                     NET_DRIVER_FLAG_AVAILABLE;

    if (!require(net_get_info(0) == SYS_ERR_INVALID, "null device query") ||
        !require(net_get_info(&info) == 0, "default device query") ||
        !require(info.abi_version == NET_RAW_ABI_VERSION,
                 "default ABI version") ||
        !require(info.state == NET_DEVICE_READY, "default ready state") ||
        !require(info.flags == expected_flags, "default driver flags") ||
        !require(info.io_base == 0x300U && info.irq == 9U,
                 "default device resources") ||
        !require(info.mtu == NET_IPV4_MTU, "default MTU") ||
        !require(info.frame_min == NET_FRAME_MIN &&
                 info.frame_max == NET_FRAME_MAX, "default frame bounds") ||
        !require(memcmp(info.mac, expected_mac, sizeof(expected_mac)) == 0,
                 "default MAC address") ||
        !require(phase_d_backend_set_device_info(0) == SYS_ERR_INVALID,
                 "null device configuration") ||
        !require(phase_d_backend_set_next_info_error(0) == SYS_ERR_INVALID,
                 "non-error device result") ||
        !require(phase_d_backend_set_next_info_error(SYS_ERR_DEVICE) == 0,
                 "device error injection") ||
        !require(net_get_info(&info) == SYS_ERR_DEVICE,
                 "injected device error") ||
        !require(net_get_info(&info) == 0, "one-shot device error")) {
        return 0;
    }

    info.state = NET_DEVICE_UNAVAILABLE;
    info.flags &= ~NET_DRIVER_FLAG_AVAILABLE;
    if (!require(phase_d_backend_set_device_info(&info) == 0,
                 "custom device configuration")) {
        return 0;
    }
    info.state = NET_DEVICE_FATAL;
    if (!require(net_get_info(&queried) == 0 &&
                 queried.state == NET_DEVICE_UNAVAILABLE,
                 "custom device query")) {
        return 0;
    }
    return 1;
}

static int test_transmit(void)
{
    unsigned char frame[NET_FRAME_MIN];
    unsigned char saved_first;
    unsigned int index;

    fill_frame(frame, sizeof(frame), 0x20U);
    saved_first = frame[0];
    if (!require(net_send_frame(frame, sizeof(frame)) == (int)sizeof(frame),
                 "captured transmit") ||
        !require(phase_d_backend_transmit_count() == 1U,
                 "transmit count") ||
        !require(phase_d_backend_transmit_length(0U) == sizeof(frame),
                 "transmit length") ||
        !require(phase_d_backend_transmit_length(1U) == 0U,
                 "invalid transmit length index")) {
        return 0;
    }
    frame[0] ^= 0xFFU;
    if (!require(phase_d_backend_transmit_frame(0U)[0] == saved_first,
                 "synchronous transmit copy") ||
        !require(phase_d_backend_transmit_frame(1U) == 0,
                 "invalid transmit index") ||
        !require(phase_d_backend_set_next_send_error(0) == SYS_ERR_INVALID,
                 "non-error send result") ||
        !require(phase_d_backend_set_next_send_error(SYS_ERR_TIMEOUT) == 0,
                 "send error injection") ||
        !require(net_send_frame(frame, sizeof(frame)) == SYS_ERR_TIMEOUT,
                 "injected send error") ||
        !require(phase_d_backend_transmit_count() == 1U,
                 "failed send not captured") ||
        !require(net_send_frame(0, sizeof(frame)) == SYS_ERR_INVALID,
                 "null send") ||
        !require(net_send_frame(frame, NET_FRAME_MIN - 1U) == SYS_ERR_RANGE,
                 "short send") ||
        !require(net_send_frame(frame, NET_FRAME_MAX + 1U) == SYS_ERR_RANGE,
                 "oversized send")) {
        return 0;
    }
    phase_d_backend_clear_transmits();
    if (!require(phase_d_backend_transmit_count() == 0U,
                 "clear transmits")) {
        return 0;
    }

    for (index = 0U; index < PHASE_D_BACKEND_QUEUE_CAPACITY; ++index) {
        fill_frame(frame, sizeof(frame), index);
        if (!require(net_send_frame(frame, sizeof(frame)) ==
                         (int)sizeof(frame),
                     "fill transmit capture")) {
            return 0;
        }
    }
    if (!require(net_send_frame(frame, sizeof(frame)) == SYS_ERR_RANGE,
                 "transmit capture overflow") ||
        !require(phase_d_backend_transmit_count() ==
                     PHASE_D_BACKEND_QUEUE_CAPACITY,
                 "full transmit count")) {
        return 0;
    }
    for (index = 0U; index < PHASE_D_BACKEND_QUEUE_CAPACITY; ++index) {
        if (!require(phase_d_backend_transmit_frame(index)[0] ==
                         (unsigned char)index,
                     "transmit capture order")) {
            return 0;
        }
    }
    return 1;
}

static int test_receive(void)
{
    unsigned char frame[61];
    unsigned char expected[61];
    unsigned char output[NET_FRAME_MAX];

    fill_frame(frame, sizeof(frame), 0x40U);
    memcpy(expected, frame, sizeof(expected));
    if (!require(net_recv_frame(0, sizeof(output)) == SYS_ERR_INVALID,
                 "null receive") ||
        !require(net_recv_frame(output, NET_FRAME_MAX + 1U) == SYS_ERR_RANGE,
                 "oversized receive capacity") ||
        !require(phase_d_backend_queue_receive(
                     frame, NET_FRAME_MAX + 1U) == SYS_ERR_RANGE,
                 "oversized queued frame") ||
        !require(phase_d_backend_queue_receive(frame, sizeof(frame)) == 0,
                 "queue receive") ||
        !require(phase_d_backend_pending_receive_count() == 1U,
                 "queued receive count")) {
        return 0;
    }
    memset(frame, 0xCC, sizeof(frame));
    if (!require(net_recv_frame(output, sizeof(output)) == (int)sizeof(frame),
                 "receive copied frame") ||
        !require(memcmp(output, expected, sizeof(expected)) == 0,
                 "receive enqueue copy") ||
        !require(phase_d_backend_pending_receive_count() == 0U,
                 "receive queue consumption")) {
        return 0;
    }

    fill_frame(frame, sizeof(frame), 0x50U);
    if (!require(phase_d_backend_queue_receive(frame, sizeof(frame)) == 0,
                 "queue capacity-drop frame") ||
        !require(net_recv_frame(output, NET_FRAME_MIN) == SYS_ERR_RANGE,
                 "capacity drop") ||
        !require(phase_d_backend_pending_receive_count() == 0U,
                 "capacity drop consumption") ||
        !require(phase_d_backend_queue_receive(frame, sizeof(frame)) == 0,
                 "requeue receive") ||
        !require(net_recv_frame(output, sizeof(output)) == (int)sizeof(frame),
                 "receive frame") ||
        !require(memcmp(output, frame, sizeof(frame)) == 0,
                 "received frame contents") ||
        !require(phase_d_backend_queue_receive_error(SYS_ERR_OVERRUN) == 0,
                 "queue receive error") ||
        !require(net_recv_frame(output, sizeof(output)) == SYS_ERR_OVERRUN,
                 "receive injected error") ||
        !require(net_recv_frame(output, sizeof(output)) == 0,
                 "empty receive queue") ||
        !require(phase_d_backend_queue_receive(0, sizeof(frame)) ==
                     SYS_ERR_INVALID,
                 "null queued frame") ||
        !require(phase_d_backend_queue_receive(frame, 0U) == SYS_ERR_RANGE,
                 "zero-length queued frame") ||
        !require(phase_d_backend_queue_receive_error(0) == SYS_ERR_INVALID,
                 "non-error receive event")) {
        return 0;
    }
    return 1;
}

static int test_platform(void)
{
    struct cancel_state cancellation = {0U, 2U};
    unsigned char first[8];
    unsigned char second[8];

    phase_d_backend_clock_set(0xFFFFFFF0U);
    phase_d_backend_clock_advance(32U);
    if (!require(net_clock_now_ms() == 0x00000010U, "wrapping clock")) {
        return 0;
    }

    phase_d_backend_random_seed(123U);
    if (!require(net_random_bytes(0, 0U) == 0,
                 "zero-length random request") ||
        !require(net_random_bytes(first, sizeof(first)) == (int)sizeof(first),
                 "first random request")) {
        return 0;
    }
    phase_d_backend_random_seed(123U);
    if (!require(net_random_bytes(second, sizeof(second)) ==
                     (int)sizeof(second),
                 "second random request") ||
        !require(memcmp(first, second, sizeof(first)) == 0,
                 "repeatable random stream") ||
        !require(net_random_bytes(0, 1U) == SYS_ERR_INVALID,
                 "null random request")) {
        return 0;
    }

    net_set_cancel_callback(cancel_after_calls, &cancellation);
    if (!require(!net_cancel_requested(), "premature callback cancellation") ||
        !require(net_cancel_requested(), "callback cancellation") ||
        !require(cancellation.calls == 2U, "callback call count")) {
        return 0;
    }
    phase_d_backend_set_cancelled(1);
    cancellation.calls = 0U;
    if (!require(net_cancel_requested(), "forced cancellation") ||
        !require(cancellation.calls == 0U, "forced cancellation callback")) {
        return 0;
    }
    phase_d_backend_set_cancelled(0);
    net_set_cancel_callback(0, 0);
    return require(!net_cancel_requested(), "cleared cancellation");
}

static int test_receive_queue_boundaries(void)
{
    unsigned char frame[NET_FRAME_MIN];
    unsigned char output[NET_FRAME_MAX];
    unsigned int index;

    for (index = 0U; index < PHASE_D_BACKEND_QUEUE_CAPACITY; ++index) {
        fill_frame(frame, sizeof(frame), index);
        if (!require(phase_d_backend_queue_receive(frame, sizeof(frame)) == 0,
                     "fill receive queue")) {
            return 0;
        }
    }
    if (!require(phase_d_backend_queue_receive(frame, sizeof(frame)) ==
                     SYS_ERR_RANGE,
                 "receive queue overflow") ||
        !require(phase_d_backend_queue_receive_error(SYS_ERR_DEVICE) ==
                     SYS_ERR_RANGE,
                 "receive error queue overflow") ||
        !require(phase_d_backend_pending_receive_count() ==
                     PHASE_D_BACKEND_QUEUE_CAPACITY,
                 "full receive count")) {
        return 0;
    }

    for (index = 0U; index < PHASE_D_BACKEND_QUEUE_CAPACITY / 2U; ++index) {
        if (!require(net_recv_frame(output, sizeof(output)) ==
                         (int)sizeof(frame),
                     "drain receive queue prefix") ||
            !require(output[0] == (unsigned char)index,
                     "receive queue prefix order")) {
            return 0;
        }
    }
    for (index = PHASE_D_BACKEND_QUEUE_CAPACITY;
         index < PHASE_D_BACKEND_QUEUE_CAPACITY * 3U / 2U; ++index) {
        fill_frame(frame, sizeof(frame), index);
        if (!require(phase_d_backend_queue_receive(frame, sizeof(frame)) == 0,
                     "wrap receive queue")) {
            return 0;
        }
    }
    if (!require(phase_d_backend_pending_receive_count() ==
                     PHASE_D_BACKEND_QUEUE_CAPACITY,
                 "wrapped receive count")) {
        return 0;
    }

    for (index = PHASE_D_BACKEND_QUEUE_CAPACITY / 2U;
         index < PHASE_D_BACKEND_QUEUE_CAPACITY * 3U / 2U; ++index) {
        if (!require(net_recv_frame(output, sizeof(output)) ==
                         (int)sizeof(frame),
                     "drain wrapped receive queue") ||
            !require(output[0] == (unsigned char)index,
                     "wrapped receive FIFO order")) {
            return 0;
        }
    }
    return require(phase_d_backend_pending_receive_count() == 0U,
                   "empty wrapped receive queue");
}

int main(void)
{
    phase_d_backend_reset();
    if (!require(strcmp(phase_d_backend_marker(),
                        "MINI_OS_PHASE_D_DETERMINISTIC_BACKEND_ONLY") == 0,
                 "test-only marker") ||
        !require(sizeof(net_u8) == 1U && sizeof(net_u16) == 2U &&
                 sizeof(net_u32) == 4U, "private integer widths") ||
        !require(sizeof(((struct net_context *)0)->tx_frame) == NET_FRAME_MAX &&
                 sizeof(((struct net_context *)0)->rx_frame) == NET_FRAME_MAX,
                 "fixed context frame buffers") ||
        !test_device()) {
        return 1;
    }

    phase_d_backend_reset();
    if (!test_transmit()) {
        return 1;
    }
    phase_d_backend_reset();
    if (!test_receive()) {
        return 1;
    }
    phase_d_backend_reset();
    if (!test_platform()) {
        return 1;
    }
    phase_d_backend_reset();
    if (!test_receive_queue_boundaries()) {
        return 1;
    }

    puts("Phase D deterministic backend test: PASS");
    return 0;
}
