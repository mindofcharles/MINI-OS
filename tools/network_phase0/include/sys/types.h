#ifndef MINI_OS_PHASE0_SYS_TYPES_H
#define MINI_OS_PHASE0_SYS_TYPES_H

#include <stddef.h>

typedef int ssize_t;
typedef int off_t;
typedef long time_t;
typedef unsigned int u_int;
typedef unsigned char u_char;

ssize_t send(int socket_descriptor, const void *buffer, size_t length, int flags);
ssize_t recv(int socket_descriptor, void *buffer, size_t length, int flags);

#endif
