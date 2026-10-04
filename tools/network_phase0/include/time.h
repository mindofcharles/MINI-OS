#ifndef MINI_OS_PHASE0_TIME_H
#define MINI_OS_PHASE0_TIME_H

#include <sys/types.h>

time_t time(time_t *result);
double difftime(time_t end, time_t beginning);

#endif
