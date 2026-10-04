#ifndef MINI_OS_PHASE0_ERRNO_H
#define MINI_OS_PHASE0_ERRNO_H

extern int errno;

#define ENOENT 2
#define EINTR 4
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define EWOULDBLOCK EAGAIN

#endif
