#ifndef MINI_OS_NET_PLATFORM_H
#define MINI_OS_NET_PLATFORM_H

#define NET_WAIT_CONTINUE 0
#define NET_WAIT_TIMEOUT 1
#define NET_WAIT_CANCELLED 2
#define NET_WAIT_INVALID (-1)

typedef int (*net_cancel_callback)(void *context);

int net_timeout_valid(unsigned int duration_ms);
unsigned int net_elapsed_ms(unsigned int start_ms, unsigned int now_ms);
int net_timeout_expired(unsigned int start_ms, unsigned int now_ms,
                        unsigned int duration_ms);
unsigned int net_timeout_remaining(unsigned int start_ms,
                                   unsigned int now_ms,
                                   unsigned int duration_ms);
int net_wait_status(unsigned int start_ms, unsigned int now_ms,
                    unsigned int duration_ms);

unsigned int net_clock_now_ms(void);
int net_random_bytes(void *buffer, unsigned int length);
int net_set_cancel_callback(net_cancel_callback callback, void *context);
int net_cancel_requested(void);

#endif
