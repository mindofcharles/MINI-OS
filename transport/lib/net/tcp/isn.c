#include "internal.h"

#include "../address.h"
#include "../byteorder.h"
#include "../net_platform.h"
#include "../../crypto/clear.h"
#include "../../crypto/hmac_sha256.h"

#include <string.h>

enum {
    TCP_EPHEMERAL_PORT_BASE = 49152,
    TCP_EPHEMERAL_PORT_COUNT = 16384,
    TCP_SEQUENCE_TICKS_PER_MS = 250
};

_Static_assert(TCP_EPHEMERAL_PORT_BASE + TCP_EPHEMERAL_PORT_COUNT == 65536 &&
               (TCP_EPHEMERAL_PORT_COUNT & (TCP_EPHEMERAL_PORT_COUNT - 1)) == 0,
               "ephemeral port range must be a power-of-two suffix");

static int entropy_fill(unsigned char *buffer, unsigned int length)
{
    if (net_random_bytes(buffer, length) != (int)length) {
        crypto_clear(buffer, length);
        return TCP_ERR_ENTROPY;
    }
    return 0;
}

static int prepare_isn(const struct tcp_endpoint *endpoint, net_u16 local_port,
                        net_u32 now_ms, net_u32 *iss)
{
    static const unsigned char domain[] = "MINI-OS TCP ISN v1";
    unsigned char message[sizeof(domain) - 1U + 12U];
    unsigned char digest[CRYPTO_SHA256_BYTES];
    unsigned char *tuple = message + sizeof(domain) - 1U;
    int result;

    memcpy(message, domain, sizeof(domain) - 1U);
    memcpy(tuple, net_global_context.config.address.octets, 4U);
    net_write_be16(tuple + 4U, local_port);
    memcpy(tuple + 6U, endpoint->address.octets, 4U);
    net_write_be16(tuple + 10U, (net_u16)endpoint->port);
    result = crypto_hmac_sha256(tcp_global_context.instance_secret,
                                sizeof(tcp_global_context.instance_secret),
                                message, sizeof(message), digest);
    if (result == 0) {
        /* The next generation advances the derived clock even at the same tick. */
        net_u32 clock = now_ms * TCP_SEQUENCE_TICKS_PER_MS +
                         tcp_global_context.last_generation + 1U;

        *iss = clock + net_read_be32(digest);
    }
    crypto_clear(digest, sizeof(digest));
    if (result == CRYPTO_HMAC_NO_MEMORY) {
        return TCP_ERR_NO_MEMORY;
    }
    return result == 0 ? 0 : TCP_ERR_CRYPTO;
}

int tcp_open_prepare(const struct tcp_endpoint *endpoint, net_u32 now_ms,
                      struct tcp_active_open *open)
{
    unsigned char random_port[2];
    int result;

    result = net_ipv4_select_next_hop(&net_global_context.config,
                                     &endpoint->address, &open->next_hop);
    if (result != 0) {
        return result;
    }
    if (!tcp_global_context.secret_ready) {
        result = entropy_fill(tcp_global_context.instance_secret,
                              sizeof(tcp_global_context.instance_secret));
        if (result != 0) {
            return result;
        }
        tcp_global_context.secret_ready = 1;
    }
    result = entropy_fill(random_port, sizeof(random_port));
    if (result != 0) {
        return result;
    }
    open->local_port = (net_u16)(TCP_EPHEMERAL_PORT_BASE +
        (net_read_be16(random_port) & (TCP_EPHEMERAL_PORT_COUNT - 1U)));
    crypto_clear(random_port, sizeof(random_port));
    return prepare_isn(endpoint, open->local_port, now_ms, &open->iss);
}
