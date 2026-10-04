#ifndef MINI_OS_PHASE_D_DETERMINISTIC_BACKEND_H
#define MINI_OS_PHASE_D_DETERMINISTIC_BACKEND_H

#include "../../transport/lib/net/raw.h"

enum {
    PHASE_D_BACKEND_QUEUE_CAPACITY = 16
};

void phase_d_backend_reset(void);
const char *phase_d_backend_marker(void);

void phase_d_backend_clock_set(unsigned int milliseconds);
void phase_d_backend_clock_advance(unsigned int milliseconds);
void phase_d_backend_set_receive_clock_step(unsigned int milliseconds);
void phase_d_backend_random_seed(unsigned int seed);
void phase_d_backend_set_cancelled(int cancelled);

int phase_d_backend_set_device_info(const struct net_device_info *info);
int phase_d_backend_set_next_info_error(int error);
int phase_d_backend_set_next_send_error(int error);
int phase_d_backend_set_next_send_result(int result);

int phase_d_backend_queue_receive(const void *frame, unsigned int length);
int phase_d_backend_queue_receive_error(int error);
unsigned int phase_d_backend_pending_receive_count(void);

void phase_d_backend_clear_transmits(void);
unsigned int phase_d_backend_transmit_count(void);
unsigned int phase_d_backend_transmit_length(unsigned int index);
const unsigned char *phase_d_backend_transmit_frame(unsigned int index);
unsigned int phase_d_backend_transmit_time(unsigned int index);

#endif
