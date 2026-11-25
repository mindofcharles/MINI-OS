#ifndef MINI_OS_CRYPTO_HMAC_SHA256_H
#define MINI_OS_CRYPTO_HMAC_SHA256_H

#include <stddef.h>

enum { CRYPTO_SHA256_BYTES = 32 };

enum crypto_hmac_error {
    CRYPTO_HMAC_NO_MEMORY = -1,
    CRYPTO_HMAC_FAILED = -2
};

/* Returns zero on success and clears the output on failure. */
int crypto_hmac_sha256(const unsigned char *key, size_t key_length,
                        const unsigned char *input, size_t input_length,
                        unsigned char output[CRYPTO_SHA256_BYTES]);

#endif
