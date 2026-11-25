#ifndef MINI_OS_TEST_HMAC_ALLOC_H
#define MINI_OS_TEST_HMAC_ALLOC_H

#include <stddef.h>

void *test_hmac_calloc(size_t count, size_t size);
void test_hmac_free(void *memory);

#define MBEDTLS_PLATFORM_CALLOC_MACRO test_hmac_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO test_hmac_free

#endif
