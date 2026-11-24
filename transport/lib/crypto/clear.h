#ifndef MINI_OS_CRYPTO_CLEAR_H
#define MINI_OS_CRYPTO_CLEAR_H

#include <stddef.h>

/* Non-elidable clearing; the caller supplies a valid writable byte range. */
void crypto_clear(void *buffer, size_t length);

#endif
