#include "deterministic_backend.h"

#include "../../transport/lib/net/net_platform.h"

#include <limits.h>
#include <string.h>

struct backend_frame {
    unsigned char data[NET_FRAME_MAX];
    unsigned int length;
    unsigned int time_ms;
};

struct backend_receive_event {
    struct backend_frame frame;
    int result;
    unsigned int queued_ms;
    unsigned int delay_ms;
};

static const char test_only_marker[] =
    "MINI_OS_NETWORK_DETERMINISTIC_BACKEND_ONLY";
static struct net_device_info device_info;
static struct backend_frame transmitted[NETWORK_TEST_BACKEND_QUEUE_CAPACITY];
static struct backend_receive_event received[NETWORK_TEST_BACKEND_QUEUE_CAPACITY];
static unsigned int transmit_count;
static unsigned int transmit_head;
static unsigned int total_transmit_count;
static unsigned int receive_head;
static unsigned int receive_count;
static unsigned int fake_clock;
static unsigned int clock_read_step;
static unsigned int receive_clock_step;
static unsigned int fake_random_state;
static int forced_cancelled;
static int cancel_once;
static int next_random_result;
static int next_random_result_set;
static unsigned int random_call_count;
static unsigned int idle_count;
static unsigned int idle_elapsed_ms;
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

void network_test_backend_reset(void)
{
    memset(transmitted, 0, sizeof(transmitted));
    memset(received, 0, sizeof(received));
    transmit_count = 0U;
    transmit_head = 0U;
    total_transmit_count = 0U;
    receive_head = 0U;
    receive_count = 0U;
    fake_clock = 0U;
    clock_read_step = 0U;
    receive_clock_step = 0U;
    fake_random_state = 0x4D494E49U;
    forced_cancelled = 0;
    cancel_once = 0;
    next_random_result = 0;
    next_random_result_set = 0;
    random_call_count = 0U;
    idle_count = 0U;
    idle_elapsed_ms = 0U;
    next_info_error = 0;
    next_send_result = 0;
    next_send_result_set = 0;
    active_cancel_callback = 0;
    active_cancel_context = 0;
    initialize_device_info();
}

const char *network_test_backend_marker(void)
{
    return test_only_marker;
}

void network_test_backend_clock_set(unsigned int milliseconds)
{
    fake_clock = milliseconds;
}

void network_test_backend_clock_advance(unsigned int milliseconds)
{
    fake_clock += milliseconds;
}

void network_test_backend_set_clock_read_step(unsigned int milliseconds)
{
    clock_read_step = milliseconds;
}

void network_test_backend_set_receive_clock_step(unsigned int milliseconds)
{
    receive_clock_step = milliseconds;
}

void network_test_backend_random_seed(unsigned int seed)
{
    fake_random_state = seed;
}

void network_test_backend_set_cancelled(int cancelled)
{
    forced_cancelled = cancelled != 0;
}

void network_test_backend_cancel_once(void)
{
    cancel_once = 1;
}

int network_test_backend_set_next_random_result(int result)
{
    next_random_result = result;
    next_random_result_set = 1;
    return 0;
}

unsigned int network_test_backend_random_call_count(void) { return random_call_count; }
unsigned int network_test_backend_idle_count(void) { return idle_count; }
unsigned int network_test_backend_idle_elapsed_ms(void) { return idle_elapsed_ms; }

int network_test_backend_set_device_info(const struct net_device_info *info)
{
    if (info == 0) {
        return SYS_ERR_INVALID;
    }
    device_info = *info;
    return 0;
}

int network_test_backend_set_next_info_error(int error)
{
    if (error >= 0) {
        return SYS_ERR_INVALID;
    }
    next_info_error = error;
    return 0;
}

int network_test_backend_set_next_send_error(int error)
{
    if (error >= 0) {
        return SYS_ERR_INVALID;
    }
    next_send_result = error;
    next_send_result_set = 1;
    return 0;
}

int network_test_backend_set_next_send_result(int result)
{
    if (result < 0) {
        return SYS_ERR_INVALID;
    }
    next_send_result = result;
    next_send_result_set = 1;
    return 0;
}

int network_test_backend_queue_receive(const void *frame, unsigned int length)
{
    return network_test_backend_schedule_receive(frame, length, 0U);
}

int network_test_backend_schedule_receive(const void *frame, unsigned int length,
                                      unsigned int delay_ms)
{
    unsigned int tail;

    if (frame == 0) {
        return SYS_ERR_INVALID;
    }
    if (length == 0U || length > NET_FRAME_MAX ||
        receive_count >= NETWORK_TEST_BACKEND_QUEUE_CAPACITY ||
        delay_ms > 0x7FFFFFFFU) {
        return SYS_ERR_RANGE;
    }
    tail = (receive_head + receive_count) % NETWORK_TEST_BACKEND_QUEUE_CAPACITY;
    memcpy(received[tail].frame.data, frame, length);
    received[tail].frame.length = length;
    received[tail].result = (int)length;
    received[tail].queued_ms = fake_clock;
    received[tail].delay_ms = delay_ms;
    ++receive_count;
    return 0;
}

int network_test_backend_queue_receive_error(int error)
{
    return network_test_backend_schedule_receive_error(error, 0U);
}

int network_test_backend_schedule_receive_error(int error, unsigned int delay_ms)
{
    unsigned int tail;

    if (error >= 0) {
        return SYS_ERR_INVALID;
    }
    if (receive_count >= NETWORK_TEST_BACKEND_QUEUE_CAPACITY ||
        delay_ms > 0x7FFFFFFFU) {
        return SYS_ERR_RANGE;
    }
    tail = (receive_head + receive_count) % NETWORK_TEST_BACKEND_QUEUE_CAPACITY;
    received[tail].frame.length = 0U;
    received[tail].result = error;
    received[tail].queued_ms = fake_clock;
    received[tail].delay_ms = delay_ms;
    ++receive_count;
    return 0;
}

unsigned int network_test_backend_pending_receive_count(void)
{
    return receive_count;
}

void network_test_backend_clear_transmits(void)
{
    memset(transmitted, 0, sizeof(transmitted));
    transmit_count = 0U;
    transmit_head = 0U;
}

unsigned int network_test_backend_transmit_count(void)
{
    return transmit_count;
}

unsigned int network_test_backend_total_transmit_count(void)
{
    return total_transmit_count;
}

int network_test_backend_drop_transmits(unsigned int count)
{
    if (count > transmit_count) {
        return SYS_ERR_RANGE;
    }
    while (count != 0U) {
        memset(&transmitted[transmit_head], 0,
               sizeof(transmitted[transmit_head]));
        transmit_head = (transmit_head + 1U) % NETWORK_TEST_BACKEND_QUEUE_CAPACITY;
        --transmit_count;
        --count;
    }
    return 0;
}

unsigned int network_test_backend_transmit_length(unsigned int index)
{
    if (index >= transmit_count) {
        return 0U;
    }
    return transmitted[(transmit_head + index) %
                        NETWORK_TEST_BACKEND_QUEUE_CAPACITY].length;
}

const unsigned char *network_test_backend_transmit_frame(unsigned int index)
{
    if (index >= transmit_count) {
        return 0;
    }
    return transmitted[(transmit_head + index) %
                        NETWORK_TEST_BACKEND_QUEUE_CAPACITY].data;
}

unsigned int network_test_backend_transmit_time(unsigned int index)
{
    if (index >= transmit_count) {
        return 0U;
    }
    return transmitted[(transmit_head + index) %
                        NETWORK_TEST_BACKEND_QUEUE_CAPACITY].time_ms;
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
    unsigned int tail;

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
    if (transmit_count >= NETWORK_TEST_BACKEND_QUEUE_CAPACITY) {
        return SYS_ERR_RANGE;
    }
    tail = (transmit_head + transmit_count) % NETWORK_TEST_BACKEND_QUEUE_CAPACITY;
    memcpy(transmitted[tail].data, frame, length);
    transmitted[tail].length = length;
    transmitted[tail].time_ms = fake_clock;
    ++transmit_count;
    ++total_transmit_count;
    return forced_result_set ? forced_result : (int)length;
}

int net_recv_frame(void *frame, unsigned int capacity)
{
    struct backend_receive_event *event;
    unsigned int selected = NETWORK_TEST_BACKEND_QUEUE_CAPACITY;
    unsigned int oldest_age = 0U;
    unsigned int index;
    int result;

    if (frame == 0) {
        return SYS_ERR_INVALID;
    }
    if (capacity > NET_FRAME_MAX) {
        return SYS_ERR_RANGE;
    }
    fake_clock += receive_clock_step;
    for (index = 0U; index < receive_count; ++index) {
        struct backend_receive_event *candidate =
            &received[(receive_head + index) % NETWORK_TEST_BACKEND_QUEUE_CAPACITY];
        unsigned int elapsed = fake_clock - candidate->queued_ms;

        if (elapsed >= candidate->delay_ms &&
            (selected == NETWORK_TEST_BACKEND_QUEUE_CAPACITY ||
             elapsed - candidate->delay_ms > oldest_age)) {
            selected = index;
            oldest_age = elapsed - candidate->delay_ms;
        }
    }
    if (selected == NETWORK_TEST_BACKEND_QUEUE_CAPACITY) {
        return 0;
    }
    event = &received[(receive_head + selected) % NETWORK_TEST_BACKEND_QUEUE_CAPACITY];
    result = event->result;
    if (result >= 0) {
        if (event->frame.length > capacity) {
            result = SYS_ERR_RANGE;
        } else {
            memcpy(frame, event->frame.data, event->frame.length);
        }
    }
    for (index = selected; index + 1U < receive_count; ++index) {
        received[(receive_head + index) % NETWORK_TEST_BACKEND_QUEUE_CAPACITY] =
            received[(receive_head + index + 1U) % NETWORK_TEST_BACKEND_QUEUE_CAPACITY];
    }
    --receive_count;
    memset(&received[(receive_head + receive_count) %
                     NETWORK_TEST_BACKEND_QUEUE_CAPACITY], 0, sizeof(*event));
    return result;
}

unsigned int net_clock_now_ms(void)
{
    fake_clock += clock_read_step;
    return fake_clock;
}

int net_random_bytes(void *buffer, unsigned int length)
{
    unsigned char *output = buffer;
    unsigned int index;
    int result;

    if (length != 0U && buffer == 0) {
        return SYS_ERR_INVALID;
    }
    if (length > INT_MAX) {
        return SYS_ERR_RANGE;
    }
    if (length == 0U) {
        return 0;
    }
    ++random_call_count;
    result = next_random_result_set ? next_random_result : (int)length;
    next_random_result_set = 0;
    if (result < 0) {
        return result;
    }
    if ((unsigned int)result > length) {
        return SYS_ERR_RANGE;
    }
    for (index = 0U; index < (unsigned int)result; ++index) {
        fake_random_state = fake_random_state * 1664525U + 1013904223U;
        output[index] = (unsigned char)(fake_random_state >> 24);
    }
    return result;
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
    if (cancel_once) {
        cancel_once = 0;
        return 1;
    }
    if (active_cancel_callback == 0) {
        return 0;
    }
    return active_cancel_callback(active_cancel_context) != 0;
}

int net_idle(unsigned int maximum_ms)
{
    unsigned int remaining = maximum_ms;
    unsigned int index;

    if (maximum_ms > 0x7FFFFFFFU) {
        return SYS_ERR_RANGE;
    }
    if (maximum_ms == 0U) {
        return 0;
    }
    ++idle_count;
    if (forced_cancelled || cancel_once) {
        return 0;
    }
    for (index = 0U; index < receive_count; ++index) {
        const struct backend_receive_event *event =
            &received[(receive_head + index) % NETWORK_TEST_BACKEND_QUEUE_CAPACITY];
        unsigned int elapsed = fake_clock - event->queued_ms;
        unsigned int until = elapsed >= event->delay_ms ? 0U :
                             event->delay_ms - elapsed;

        if (until < remaining) {
            remaining = until;
        }
    }
    fake_clock += remaining;
    idle_elapsed_ms += remaining;
    return 0;
}
