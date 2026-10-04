#include "clear.h"

void crypto_clear(void *buffer, unsigned int length)
{
    volatile unsigned char *bytes = buffer;

    while (length != 0U) {
        *bytes++ = 0U;
        --length;
    }
}
