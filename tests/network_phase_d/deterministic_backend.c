#include "deterministic_backend.h"

#include "../../transport/lib/net/net_platform.h"

#include <string.h>

struct backend_frame {
    unsigned char data[NET_FRAME_MAX];
    unsigned int length;
    unsigned int time_ms;
};

struct backend_receive_event {
    struct backend_frame frame;
    int result;
};

static const char test_only_marker[] =
    "MINI_OS_PHASE_D_DETERMINISTIC_BACKEND_ONLY";
static struct net_device_info device_info;
static struct backend_frame transmitted[PHASE_D_BACKEND_QUEUE_CAPACITY];
static struct backend_receive_event received[PHASE_D_BACKEND_QUEUE_CAPACITY];
static unsigned int transmit_count;
static unsigned int receive_head;
static unsigned int receive_count;
static unsigned int fake_clock;
static unsigned int receive_clock_step;
static unsigned int fake_random_state;
static int forced_cancelled;
static int next_info_error;
static int next_send_result;
static int next_send_result_set;
static net_cancel_callback active_cancel_callback;
static void *active_cancel_context;

static void initialize_device_info(void)
{
    static const unsigned char default_mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
    };

    memset(&device_info, 0, sizeof(device_info));
    device_info.abi_version = NET_RAW_ABI_VERSION;
    device_info.state = NET_DEVICE_READY;
    device_info.flags = NET_DRIVER_FLAG_POLLING |
                        NET_DRIVER_FLAG_IRQ_MASKED |
                        NET_DRIVER_FLAG_FCS_STRIPPED |
                        NET_DRIVER_FLAG_SYNC_TX |
                        NET_DRIVER_FLAG_AVAILABLE;
    device_info.io_base = 0x300U;
    device_info.irq = 9U;
    device_info.mtu = NET_IPV4_MTU;
    device_info.frame_min = NET_FRAME_MIN;
    device_info.frame_max = NET_FRAME_MAX;
    memcpy(device_info.mac, default_mac, sizeof(default_mac));
}

void phase_d_backend_reset(void)
{
    memset(transmitted, 0, sizeof(transmitted));
    memset(received, 0, sizeof(received));
    transmit_count = 0U;
    receive_head = 0U;
    receive_count = 0U;
    fake_clock = 0U;
    receive_clock_step = 0U;
    fake_random_state = 0x4D494E49U;
    forced_cancelled = 0;
    next_info_error = 0;
    next_send_result = 0;
    next_send_result_set = 0;
    active_cancel_callback = 0;
    active_cancel_context = 0;
    initialize_device_info();
}

const char *phase_d_backend_marker(void)
{
    return test_only_marker;
}

void phase_d_backend_clock_set(unsigned int milliseconds)
{
    fake_clock = milliseconds;
}

void phase_d_backend_clock_advance(unsigned int milliseconds)
{
    fake_clock += milliseconds;
}

void phase_d_backend_set_receive_clock_step(unsigned int milliseconds)
{
    receive_clock_step = milliseconds;
}

void phase_d_backend_random_seed(unsigned int seed)
{
    fake_random_state = seed;
}

void phase_d_backend_set_cancelled(int cancelled)
{
    forced_cancelled = cancelled != 0;
}

int phase_d_backend_set_device_info(const struct net_device_info *info)
{
    if (info == 0) {
        return SYS_ERR_INVALID;
    }
    device_info = *info;
    return 0;
}

int phase_d_backend_set_next_info_error(int error)
{
    if (error >= 0) {
        return SYS_ERR_INVALID;
    }
    next_info_error = error;
    return 0;
}

int phase_d_backend_set_next_send_error(int error)
{
    if (error >= 0) {
        return SYS_ERR_INVALID;
    }
    next_send_result = error;
    next_send_result_set = 1;
    return 0;
}

int phase_d_backend_set_next_send_result(int result)
{
    if (result < 0) {
        return SYS_ERR_INVALID;
    }
    next_send_result = result;
    next_send_result_set = 1;
    return 0;
}

int phase_d_backend_queue_receive(const void *frame, unsigned int length)
{
    unsigned int tail;

    if (frame == 0) {
        return SYS_ERR_INVALID;
    }
    if (length == 0U || length > NET_FRAME_MAX ||
        receive_count >= PHASE_D_BACKEND_QUEUE_CAPACITY) {
        return SYS_ERR_RANGE;
    }
    tail = (receive_head + receive_count) % PHASE_D_BACKEND_QUEUE_CAPACITY;
    memcpy(received[tail].frame.data, frame, length);
    received[tail].frame.length = length;
    received[tail].result = (int)length;
    ++receive_count;
    return 0;
}

int phase_d_backend_queue_receive_error(int error)
{
    unsigned int tail;

    if (error >= 0) {
        return SYS_ERR_INVALID;
    }
    if (receive_count >= PHASE_D_BACKEND_QUEUE_CAPACITY) {
        return SYS_ERR_RANGE;
    }
    tail = (receive_head + receive_count) % PHASE_D_BACKEND_QUEUE_CAPACITY;
    received[tail].frame.length = 0U;
    received[tail].result = error;
    ++receive_count;
    return 0;
}

unsigned int phase_d_backend_pending_receive_count(void)
{
    return receive_count;
}

void phase_d_backend_clear_transmits(void)
{
    memset(transmitted, 0, sizeof(transmitted));
    transmit_count = 0U;
}

unsigned int phase_d_backend_transmit_count(void)
{
    return transmit_count;
}

unsigned int phase_d_backend_transmit_length(unsigned int index)
{
    if (index >= transmit_count) {
        return 0U;
    }
    return transmitted[index].length;
}

const unsigned char *phase_d_backend_transmit_frame(unsigned int index)
{
    if (index >= transmit_count) {
        return 0;
    }
    return transmitted[index].data;
}

unsigned int phase_d_backend_transmit_time(unsigned int index)
{
    if (index >= transmit_count) {
        return 0U;
    }
    return transmitted[index].time_ms;
}

int net_get_info(struct net_device_info *info)
{
    int error;

    if (info == 0) {
        return SYS_ERR_INVALID;
    }
    error = next_info_error;
    next_info_error = 0;
    if (error < 0) {
        return error;
    }
    *info = device_info;
    return 0;
}

int net_send_frame(const void *frame, unsigned int length)
{
    int forced_result;
    int forced_result_set;

    if (frame == 0) {
        return SYS_ERR_INVALID;
    }
    if (length < NET_FRAME_MIN || length > NET_FRAME_MAX) {
        return SYS_ERR_RANGE;
    }
    forced_result = next_send_result;
    forced_result_set = next_send_result_set;
    next_send_result = 0;
    next_send_result_set = 0;
    if (forced_result_set && forced_result < 0) {
        return forced_result;
    }
    if (transmit_count >= PHASE_D_BACKEND_QUEUE_CAPACITY) {
        return SYS_ERR_RANGE;
    }
    memcpy(transmitted[transmit_count].data, frame, length);
    transmitted[transmit_count].length = length;
    transmitted[transmit_count].time_ms = fake_clock;
    ++transmit_count;
    return forced_result_set ? forced_result : (int)length;
}

int net_recv_frame(void *frame, unsigned int capacity)
{
    struct backend_receive_event *event;
    int result;

    if (frame == 0) {
        return SYS_ERR_INVALID;
    }
    if (capacity > NET_FRAME_MAX) {
        return SYS_ERR_RANGE;
    }
    fake_clock += receive_clock_step;
    if (receive_count == 0U) {
        return 0;
    }
    event = &received[receive_head];
    result = event->result;
    receive_head = (receive_head + 1U) % PHASE_D_BACKEND_QUEUE_CAPACITY;
    --receive_count;
    if (result < 0) {
        return result;
    }
    if (event->frame.length > capacity) {
        return SYS_ERR_RANGE;
    }
    memcpy(frame, event->frame.data, event->frame.length);
    return (int)event->frame.length;
}

unsigned int net_clock_now_ms(void)
{
    return fake_clock;
}

int net_random_bytes(void *buffer, unsigned int length)
{
    unsigned char *output = buffer;
    unsigned int index;

    if (length != 0U && buffer == 0) {
        return SYS_ERR_INVALID;
    }
    for (index = 0U; index < length; ++index) {
        fake_random_state = fake_random_state * 1664525U + 1013904223U;
        output[index] = (unsigned char)(fake_random_state >> 24);
    }
    return (int)length;
}

int net_set_cancel_callback(net_cancel_callback callback, void *context)
{
    active_cancel_callback = callback;
    active_cancel_context = context;
    return 0;
}

int net_cancel_requested(void)
{
    if (forced_cancelled) {
        return 1;
    }
    if (active_cancel_callback == 0) {
        return 0;
    }
    return active_cancel_callback(active_cancel_context) != 0;
}
