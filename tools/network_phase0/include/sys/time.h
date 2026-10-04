#ifndef MINI_OS_PHASE0_SYS_TIME_H
#define MINI_OS_PHASE0_SYS_TIME_H

struct timeval {
    long tv_sec;
    long tv_usec;
};

int gettimeofday(struct timeval *time_value, void *timezone_value);

#endif
