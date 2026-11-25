#include "clear.h"

void crypto_clear(void *buffer, size_t length)
{
    volatile unsigned char *bytes = buffer;

    while (length != 0U) {
        *bytes++ = 0U;
        --length;
    }
}
