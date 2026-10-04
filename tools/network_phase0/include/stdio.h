#ifndef MINI_OS_PHASE0_STDIO_H
#define MINI_OS_PHASE0_STDIO_H

#include "minilibc.h"
#include <stdarg.h>

int vsprintf(char *string, const char *format, va_list arguments);
int vsnprintf(char *string, size_t size, const char *format, va_list arguments);
int fprintf(FILE *stream, const char *format, ...);
int fputs(const char *string, FILE *stream);
void setbuf(FILE *stream, char *buffer);

#define FOPEN_READTEXT "r"

#endif
