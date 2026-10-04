#ifndef MINI_OS_PLATFORM_H
#define MINI_OS_PLATFORM_H

/* The low 32 bits of monotonic milliseconds since kernel initialization. */
unsigned int clock_monotonic_ms(void);

/* Returns zero when no translated key is pending, or a positive key code. */
int kbd_poll_key(void);

/* Fills the complete request, or returns a negative platform error. */
int get_random(void *buffer, unsigned int length);

#endif
