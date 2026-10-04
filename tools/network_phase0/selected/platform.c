#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include "libssh2.h"

int errno;

const unsigned char mini_os_phase0_test_entropy_marker[] =
    "MINI_OS_PHASE0_TEST_ENTROPY_ONLY";

time_t time(time_t *result)
{
    static time_t now;
    time_t value;

    value = now++;
    if (result != NULL) {
        *result = value;
    }
    return value;
}

double difftime(time_t end, time_t beginning)
{
    return (double)(end - beginning);
}

int poll(struct pollfd *descriptors, nfds_t count, int timeout_ms)
{
    (void)descriptors;
    (void)count;
    (void)timeout_ms;
    return 0;
}

int gettimeofday(struct timeval *time_value, void *timezone_value)
{
    (void)timezone_value;
    if (time_value != NULL) {
        time_value->tv_sec = time(NULL);
        time_value->tv_usec = 0;
    }
    return 0;
}

void setbuf(FILE *stream, char *buffer)
{
    (void)stream;
    (void)buffer;
}

int fprintf(FILE *stream, const char *format, ...)
{
    (void)stream;
    (void)format;
    return -1;
}

ssize_t send(int socket_descriptor, const void *buffer, size_t length,
             int flags)
{
    (void)socket_descriptor;
    (void)buffer;
    (void)length;
    (void)flags;
    errno = EAGAIN;
    return -1;
}

ssize_t recv(int socket_descriptor, void *buffer, size_t length, int flags)
{
    (void)socket_descriptor;
    (void)buffer;
    (void)length;
    (void)flags;
    errno = EAGAIN;
    return -1;
}

int mbedtls_hardware_poll(void *context, unsigned char *output,
                          size_t length, size_t *output_length)
{
    size_t index;

    (void)context;
    for (index = 0; index < length; ++index) {
        output[index] = (unsigned char)(0xA5U ^ (unsigned char)index);
    }
    *output_length = length;
    return 0;
}
