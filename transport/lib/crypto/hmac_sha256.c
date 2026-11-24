#include "hmac_sha256.h"
#include "clear.h"

#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

void mbedtls_platform_zeroize(void *buffer, size_t length)
{
    crypto_clear(buffer, length);
}

int crypto_hmac_sha256(const unsigned char *key, size_t key_length,
                        const unsigned char *input, size_t input_length,
                        unsigned char output[CRYPTO_SHA256_BYTES])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int result;

    if (output == 0) {
        return CRYPTO_HMAC_FAILED;
    }
    if (info == 0 || key == 0 || input == 0) {
        crypto_clear(output, CRYPTO_SHA256_BYTES);
        return CRYPTO_HMAC_FAILED;
    }
    result = mbedtls_md_hmac(info, key, key_length, input, input_length, output);
    if (result != 0) {
        crypto_clear(output, CRYPTO_SHA256_BYTES);
        return result == MBEDTLS_ERR_MD_ALLOC_FAILED ? CRYPTO_HMAC_NO_MEMORY :
                                                      CRYPTO_HMAC_FAILED;
    }
    return 0;
}
