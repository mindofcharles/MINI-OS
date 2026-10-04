#ifndef MINI_OS_NETWORK_TEST_BACKEND_H
#define MINI_OS_NETWORK_TEST_BACKEND_H

#include "../../transport/lib/net/raw.h"

enum {
    NETWORK_TEST_BACKEND_QUEUE_CAPACITY = 128
};

void network_test_backend_reset(void);
const char *network_test_backend_marker(void);

void network_test_backend_clock_set(unsigned int milliseconds);
void network_test_backend_clock_advance(unsigned int milliseconds);
/* Optional advance before each clock read; reset and zero disable it. */
void network_test_backend_set_clock_read_step(unsigned int milliseconds);
void network_test_backend_set_receive_clock_step(unsigned int milliseconds);
void network_test_backend_random_seed(unsigned int seed);
void network_test_backend_set_cancelled(int cancelled);
void network_test_backend_cancel_once(void);
int network_test_backend_set_next_random_result(int result);
unsigned int network_test_backend_random_call_count(void);
unsigned int network_test_backend_idle_count(void);
unsigned int network_test_backend_idle_elapsed_ms(void);

int network_test_backend_set_device_info(const struct net_device_info *info);
int network_test_backend_set_next_info_error(int error);
int network_test_backend_set_next_send_error(int error);
int network_test_backend_set_next_send_result(int result);

int network_test_backend_queue_receive(const void *frame, unsigned int length);
int network_test_backend_queue_receive_error(int error);
int network_test_backend_schedule_receive(const void *frame, unsigned int length,
                                      unsigned int delay_ms);
int network_test_backend_schedule_receive_error(int error, unsigned int delay_ms);
unsigned int network_test_backend_pending_receive_count(void);

void network_test_backend_clear_transmits(void);
unsigned int network_test_backend_transmit_count(void);
unsigned int network_test_backend_total_transmit_count(void);
int network_test_backend_drop_transmits(unsigned int count);
unsigned int network_test_backend_transmit_length(unsigned int index);
const unsigned char *network_test_backend_transmit_frame(unsigned int index);
unsigned int network_test_backend_transmit_time(unsigned int index);

#endif
