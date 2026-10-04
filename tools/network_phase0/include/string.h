#ifndef MINI_OS_PHASE0_STRING_H
#define MINI_OS_PHASE0_STRING_H

#include "minilibc.h"

char *strtok_r(char *string, const char *delimiters, char **save_pointer);
int strncasecmp(const char *left, const char *right, size_t count);

#endif
