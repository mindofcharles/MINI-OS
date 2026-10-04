#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "libssh2.h"

typedef union {
    max_align_t alignment;
    size_t size;
} AllocationHeader;

static size_t current_bytes;
static size_t peak_bytes;
static size_t allocation_count;
static size_t maximum_inbound_packet;
static size_t maximum_outbound_packet;

void mini_os_phase0_record_packet(int outgoing, size_t length)
{
    size_t *maximum;

    maximum = outgoing != 0 ? &maximum_outbound_packet :
                              &maximum_inbound_packet;
    if (length > *maximum) {
        *maximum = length;
    }
}

static void account_add(size_t size)
{
    current_bytes += size;
    if (current_bytes > peak_bytes) {
        peak_bytes = current_bytes;
    }
    ++allocation_count;
}

static void *tracked_malloc(size_t size)
{
    AllocationHeader *header;

    if (size > SIZE_MAX - sizeof(*header)) {
        return NULL;
    }
    header = malloc(sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }
    header->size = size;
    account_add(size);
    return header + 1;
}

void phase0_free(void *pointer)
{
    AllocationHeader *header;

    if (pointer == NULL) {
        return;
    }
    header = (AllocationHeader *)pointer - 1;
    current_bytes -= header->size;
    free(header);
}

void *phase0_calloc(size_t count, size_t size)
{
    void *pointer;
    size_t total;

    if (size != 0 && count > SIZE_MAX / size) {
        return NULL;
    }
    total = count * size;
    pointer = tracked_malloc(total);
    if (pointer != NULL) {
        memset(pointer, 0, total);
    }
    return pointer;
}

static void *tracked_realloc(void *pointer, size_t size)
{
    AllocationHeader *old_header;
    AllocationHeader *new_header;
    size_t old_size;

    if (pointer == NULL) {
        return tracked_malloc(size);
    }
    if (size == 0) {
        phase0_free(pointer);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(*new_header)) {
        return NULL;
    }
    old_header = (AllocationHeader *)pointer - 1;
    old_size = old_header->size;
    new_header = realloc(old_header, sizeof(*new_header) + size);
    if (new_header == NULL) {
        return NULL;
    }
    new_header->size = size;
    current_bytes -= old_size;
    account_add(size);
    return new_header + 1;
}

static LIBSSH2_ALLOC_FUNC(session_alloc)
{
    (void)abstract;
    return tracked_malloc(count);
}

static LIBSSH2_REALLOC_FUNC(session_realloc)
{
    (void)abstract;
    return tracked_realloc(ptr, count);
}

static LIBSSH2_FREE_FUNC(session_free)
{
    (void)abstract;
    phase0_free(ptr);
}

int mbedtls_hardware_poll(void *context, unsigned char *output,
                          size_t length, size_t *output_length)
{
    (void)context;
    arc4random_buf(output, length);
    *output_length = length;
    return 0;
}

static int set_algorithms(LIBSSH2_SESSION *session)
{
    return libssh2_session_method_pref(session, LIBSSH2_METHOD_KEX,
                                       "ecdh-sha2-nistp256") ||
           libssh2_session_method_pref(session, LIBSSH2_METHOD_HOSTKEY,
                                       "ecdsa-sha2-nistp256") ||
           libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_CS,
                                       "aes128-ctr") ||
           libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_SC,
                                       "aes128-ctr") ||
           libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_CS,
                                       "hmac-sha2-256") ||
           libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_SC,
                                       "hmac-sha2-256") ||
           libssh2_session_method_pref(session, LIBSSH2_METHOD_COMP_CS,
                                       "none") ||
           libssh2_session_method_pref(session, LIBSSH2_METHOD_COMP_SC,
                                       "none");
}

static void wait_for_socket(int socket_descriptor, LIBSSH2_SESSION *session)
{
    struct pollfd descriptor;
    int directions;

    directions = libssh2_session_block_directions(session);
    descriptor.fd = socket_descriptor;
    descriptor.events = 0;
    descriptor.revents = 0;
    if ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) != 0) {
        descriptor.events |= POLLIN;
    }
    if ((directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0) {
        descriptor.events |= POLLOUT;
    }
    if (descriptor.events == 0) {
        descriptor.events = POLLIN | POLLOUT;
    }
    (void)poll(&descriptor, 1, 100);
}

static void record_cleanup_result(int cleanup_result, int *result,
                                  const char **failure_stage,
                                  const char *stage)
{
    if (cleanup_result != 0 && *result == 0) {
        *result = 1;
        *failure_stage = stage;
    }
}

int main(int argument_count, char **arguments)
{
    struct sockaddr_in address;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    const char *failure_stage;
    char output[256];
    char *port_end;
    long port;
    size_t output_length;
    int socket_descriptor;
    int socket_flags;
    int result;
    int attempts;
    int initialized;

    if (argument_count != 6) {
        fprintf(stderr, "Usage: %s <IPv4> <port> <user> <public-key> <private-key>\n",
                arguments[0]);
        return 2;
    }
    port = strtol(arguments[2], &port_end, 10);
    if (*arguments[2] == '\0' || *port_end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", arguments[2]);
        return 2;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, arguments[1], &address.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", arguments[1]);
        return 2;
    }

    session = NULL;
    channel = NULL;
    socket_descriptor = -1;
    socket_flags = -1;
    output_length = 0;
    result = 1;
    initialized = 0;
    failure_stage = "libssh2_init";
    if (libssh2_init(0) != 0) {
        goto cleanup;
    }
    initialized = 1;
    failure_stage = "socket";
    socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0) {
        goto cleanup;
    }
    failure_stage = "connect";
    if (connect(socket_descriptor, (struct sockaddr *)&address,
                sizeof(address)) != 0) {
        goto cleanup;
    }
    failure_stage = "session_init";
    session = libssh2_session_init_ex(session_alloc, session_free,
                                      session_realloc, NULL);
    if (session == NULL || set_algorithms(session) != 0) {
        goto cleanup;
    }
    libssh2_session_set_timeout(session, 5000);
    failure_stage = "handshake";
    if (libssh2_session_handshake(session, socket_descriptor) != 0) {
        goto cleanup;
    }
    printf("after_handshake=%zu peak=%zu packets_rx=%zu packets_tx=%zu\n",
           current_bytes, peak_bytes, maximum_inbound_packet,
           maximum_outbound_packet);

    failure_stage = "publickey_auth";
    if (libssh2_userauth_publickey_fromfile(session, arguments[3],
                                            arguments[4], arguments[5],
                                            NULL) != 0) {
        goto cleanup;
    }
    printf("after_auth=%zu peak=%zu packets_rx=%zu packets_tx=%zu\n",
           current_bytes, peak_bytes, maximum_inbound_packet,
           maximum_outbound_packet);

    failure_stage = "channel_open";
    channel = libssh2_channel_open_session(session);
    if (channel == NULL) {
        goto cleanup;
    }
    failure_stage = "channel_exec";
    if (libssh2_channel_exec(channel, "printf phase0-ok") != 0) {
        goto cleanup;
    }
    socket_flags = fcntl(socket_descriptor, F_GETFL, 0);
    if (socket_flags < 0 ||
        fcntl(socket_descriptor, F_SETFL, socket_flags | O_NONBLOCK) != 0) {
        failure_stage = "nonblocking_socket";
        goto cleanup;
    }
    libssh2_session_set_blocking(session, 0);
    failure_stage = "channel_read";
    for (attempts = 0; attempts < 100 && output_length < 9; ++attempts) {
        ssize_t received;

        received = libssh2_channel_read(channel, output + output_length,
                                        sizeof(output) - output_length);
        if (received > 0) {
            output_length += (size_t)received;
        }
        else if (received == LIBSSH2_ERROR_EAGAIN) {
            wait_for_socket(socket_descriptor, session);
        }
        else {
            break;
        }
    }
    if (output_length != 9 || memcmp(output, "phase0-ok", 9) != 0) {
        goto cleanup;
    }
    printf("after_command=%zu peak=%zu packets_rx=%zu packets_tx=%zu\n",
           current_bytes, peak_bytes, maximum_inbound_packet,
           maximum_outbound_packet);
    result = 0;
    failure_stage = "complete";

cleanup:
    if (socket_descriptor >= 0 && socket_flags >= 0) {
        record_cleanup_result(
            fcntl(socket_descriptor, F_SETFL, socket_flags),
            &result, &failure_stage, "restore_socket_flags");
    }
    if (session != NULL) {
        libssh2_session_set_blocking(session, 1);
    }
    if (channel != NULL) {
        record_cleanup_result(libssh2_channel_close(channel),
                              &result, &failure_stage, "channel_close");
        record_cleanup_result(libssh2_channel_free(channel),
                              &result, &failure_stage, "channel_free");
    }
    if (session != NULL) {
        record_cleanup_result(
            libssh2_session_disconnect(session, "phase0 complete"),
            &result, &failure_stage, "session_disconnect");
        record_cleanup_result(libssh2_session_free(session),
                              &result, &failure_stage, "session_free");
    }
    if (socket_descriptor >= 0) {
        record_cleanup_result(close(socket_descriptor),
                              &result, &failure_stage, "socket_close");
    }
    if (initialized != 0) {
        libssh2_exit();
    }
    if (result == 0 && current_bytes != 0) {
        result = 1;
        failure_stage = "cleanup_leak";
    }
    printf("final_current=%zu peak=%zu allocations=%zu result=%d stage=%s\n",
           current_bytes, peak_bytes, allocation_count, result,
           failure_stage);
    return result;
}
