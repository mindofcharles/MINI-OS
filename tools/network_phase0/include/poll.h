#ifndef MINI_OS_PHASE0_POLL_H
#define MINI_OS_PHASE0_POLL_H

typedef unsigned int nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLHUP 0x0010

int poll(struct pollfd *descriptors, nfds_t count, int timeout_ms);

#endif
