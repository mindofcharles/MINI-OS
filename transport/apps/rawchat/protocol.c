#include "protocol.h"

#include <limits.h>
#include <string.h>

#define RAWCHAT_STATIC_ASSERT(name, condition) \
    typedef char rawchat_static_assert_##name[(condition) ? 1 : -1]

RAWCHAT_STATIC_ASSERT(char_bit_is_eight, CHAR_BIT == 8);
RAWCHAT_STATIC_ASSERT(unsigned_int_is_32_bits, UINT_MAX == 0xFFFFFFFFU);
RAWCHAT_STATIC_ASSERT(protocol_header_size_is_stable,
                      RAWCHAT_PROTOCOL_HEADER_SIZE == 14);
RAWCHAT_STATIC_ASSERT(wire_header_size_is_stable,
                      RAWCHAT_WIRE_HEADER_SIZE == 28);
RAWCHAT_STATIC_ASSERT(payload_fits_one_frame,
                      RAWCHAT_WIRE_HEADER_SIZE + RAWCHAT_TEXT_MAX <=
                      NET_FRAME_MAX);

#undef RAWCHAT_STATIC_ASSERT

enum {
    RAWCHAT_DESTINATION_OFFSET = 0,
    RAWCHAT_SOURCE_OFFSET = 6,
    RAWCHAT_ETHERTYPE_OFFSET = 12,
    RAWCHAT_SUBTYPE_OFFSET = 14,
    RAWCHAT_VERSION_OFFSET = 15,
    RAWCHAT_TYPE_OFFSET = 16,
    RAWCHAT_FLAGS_OFFSET = 17,
    RAWCHAT_SESSION_OFFSET = 18,
    RAWCHAT_SEQUENCE_OFFSET = 22,
    RAWCHAT_LENGTH_OFFSET = 26,
    RAWCHAT_PAYLOAD_OFFSET = 28,
    RAWCHAT_MAC_SIZE = 6
};

static void write_u16(unsigned char *output, unsigned int value)
{
    output[0] = (unsigned char)(value >> 8);
    output[1] = (unsigned char)value;
}

static unsigned int read_u16(const unsigned char *input)
{
    return ((unsigned int)input[0] << 8) | (unsigned int)input[1];
}

static void write_u32(unsigned char *output, unsigned int value)
{
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

static unsigned int read_u32(const unsigned char *input)
{
    return ((unsigned int)input[0] << 24) |
           ((unsigned int)input[1] << 16) |
           ((unsigned int)input[2] << 8) |
           (unsigned int)input[3];
}

static int mac_is_unicast(const unsigned char *mac)
{
    unsigned int index;
    unsigned int nonzero;

    if ((mac[0] & 1U) != 0U) {
        return 0;
    }
    nonzero = 0U;
    for (index = 0U; index < RAWCHAT_MAC_SIZE; ++index) {
        nonzero |= mac[index];
    }
    return nonzero != 0U;
}

static int text_is_printable(const unsigned char *payload,
                             unsigned int payload_length)
{
    unsigned int index;

    for (index = 0U; index < payload_length; ++index) {
        if (payload[index] < 32U || payload[index] > 126U) {
            return 0;
        }
    }
    return 1;
}

static int message_fields_valid(unsigned int message_type,
                                unsigned int session_id,
                                unsigned int sequence,
                                const unsigned char *payload,
                                unsigned int payload_length)
{
    if (session_id == 0U) {
        return 0;
    }
    if (message_type == RAWCHAT_MESSAGE_HELLO) {
        return sequence == 0U && payload_length == 0U;
    }
    if (message_type == RAWCHAT_MESSAGE_BYE) {
        return sequence != 0U && payload_length == 0U;
    }
    if (message_type != RAWCHAT_MESSAGE_TEXT || sequence == 0U ||
        payload_length == 0U || payload_length > RAWCHAT_TEXT_MAX ||
        payload == 0) {
        return 0;
    }
    return text_is_printable(payload, payload_length);
}

int rawchat_build_frame(const unsigned char *source_mac,
                        const unsigned char *destination_mac,
                        unsigned int message_type,
                        unsigned int session_id,
                        unsigned int sequence,
                        const void *payload,
                        unsigned int payload_length,
                        void *frame,
                        unsigned int frame_capacity,
                        unsigned int *frame_length)
{
    const unsigned char *payload_bytes = payload;
    unsigned char *output = frame;
    unsigned int length;

    if (source_mac == 0 || destination_mac == 0 || frame == 0 ||
        frame_length == 0 || !mac_is_unicast(source_mac) ||
        !mac_is_unicast(destination_mac) ||
        memcmp(source_mac, destination_mac, RAWCHAT_MAC_SIZE) == 0 ||
        !message_fields_valid(message_type, session_id, sequence,
                              payload_bytes, payload_length)) {
        return RAWCHAT_BUILD_INVALID;
    }

    length = RAWCHAT_WIRE_HEADER_SIZE + payload_length;
    if (length < NET_FRAME_MIN) {
        length = NET_FRAME_MIN;
    }
    if (frame_capacity < length) {
        return RAWCHAT_BUILD_CAPACITY;
    }

    memset(output, 0, length);
    memcpy(output + RAWCHAT_DESTINATION_OFFSET, destination_mac,
           RAWCHAT_MAC_SIZE);
    memcpy(output + RAWCHAT_SOURCE_OFFSET, source_mac, RAWCHAT_MAC_SIZE);
    write_u16(output + RAWCHAT_ETHERTYPE_OFFSET, RAWCHAT_ETHERTYPE);
    output[RAWCHAT_SUBTYPE_OFFSET] = RAWCHAT_SUBTYPE;
    output[RAWCHAT_VERSION_OFFSET] = RAWCHAT_VERSION;
    output[RAWCHAT_TYPE_OFFSET] = (unsigned char)message_type;
    output[RAWCHAT_FLAGS_OFFSET] = 0U;
    write_u32(output + RAWCHAT_SESSION_OFFSET, session_id);
    write_u32(output + RAWCHAT_SEQUENCE_OFFSET, sequence);
    write_u16(output + RAWCHAT_LENGTH_OFFSET, payload_length);
    if (payload_length != 0U) {
        memcpy(output + RAWCHAT_PAYLOAD_OFFSET, payload, payload_length);
    }
    *frame_length = length;
    return RAWCHAT_BUILD_OK;
}

int rawchat_parse_frame(const void *frame,
                        unsigned int frame_length,
                        const unsigned char *local_mac,
                        const unsigned char *peer_mac,
                        struct rawchat_message *message)
{
    const unsigned char *input = frame;
    struct rawchat_message parsed;
    unsigned int payload_length;

    if (frame == 0 || local_mac == 0 || peer_mac == 0 || message == 0) {
        return RAWCHAT_PARSE_INVALID;
    }
    if (frame_length < NET_FRAME_MIN || frame_length > NET_FRAME_MAX) {
        return RAWCHAT_PARSE_MALFORMED;
    }
    if (memcmp(input + RAWCHAT_DESTINATION_OFFSET, local_mac,
               RAWCHAT_MAC_SIZE) != 0 ||
        memcmp(input + RAWCHAT_SOURCE_OFFSET, peer_mac,
               RAWCHAT_MAC_SIZE) != 0 ||
        read_u16(input + RAWCHAT_ETHERTYPE_OFFSET) != RAWCHAT_ETHERTYPE ||
        input[RAWCHAT_SUBTYPE_OFFSET] != RAWCHAT_SUBTYPE) {
        return RAWCHAT_PARSE_UNRELATED;
    }
    if (input[RAWCHAT_VERSION_OFFSET] != RAWCHAT_VERSION ||
        input[RAWCHAT_FLAGS_OFFSET] != 0U) {
        return RAWCHAT_PARSE_MALFORMED;
    }

    memset(&parsed, 0, sizeof(parsed));
    parsed.type = input[RAWCHAT_TYPE_OFFSET];
    parsed.session_id = read_u32(input + RAWCHAT_SESSION_OFFSET);
    parsed.sequence = read_u32(input + RAWCHAT_SEQUENCE_OFFSET);
    payload_length = read_u16(input + RAWCHAT_LENGTH_OFFSET);
    parsed.payload_length = payload_length;

    if (payload_length > RAWCHAT_TEXT_MAX ||
        RAWCHAT_WIRE_HEADER_SIZE + payload_length > frame_length ||
        !message_fields_valid(parsed.type, parsed.session_id,
                              parsed.sequence,
                              input + RAWCHAT_PAYLOAD_OFFSET,
                              payload_length)) {
        return RAWCHAT_PARSE_MALFORMED;
    }
    if (payload_length != 0U) {
        memcpy(parsed.payload, input + RAWCHAT_PAYLOAD_OFFSET,
               payload_length);
    }
    parsed.payload[payload_length] = 0U;
    *message = parsed;
    return RAWCHAT_PARSE_OK;
}

void rawchat_peer_reset(struct rawchat_peer_state *state)
{
    if (state != 0) {
        memset(state, 0, sizeof(*state));
    }
}

int rawchat_peer_accept(struct rawchat_peer_state *state,
                        const struct rawchat_message *message)
{
    int gap;

    if (state == 0 || message == 0 ||
        !message_fields_valid(message->type, message->session_id,
                              message->sequence, message->payload,
                              message->payload_length)) {
        return RAWCHAT_PEER_INVALID;
    }

    if (message->type == RAWCHAT_MESSAGE_HELLO) {
        if (state->active && state->session_id == message->session_id) {
            return RAWCHAT_PEER_DUPLICATE;
        }
        state->session_id = message->session_id;
        state->last_sequence = 0U;
        state->active = 1;
        return RAWCHAT_PEER_STARTED;
    }

    if (!state->active || state->session_id != message->session_id) {
        return RAWCHAT_PEER_NO_SESSION;
    }
    if (message->sequence <= state->last_sequence) {
        return RAWCHAT_PEER_DUPLICATE;
    }

    gap = message->sequence != state->last_sequence + 1U;
    state->last_sequence = message->sequence;
    if (message->type == RAWCHAT_MESSAGE_BYE) {
        state->active = 0;
    }
    return gap ? RAWCHAT_PEER_GAP : RAWCHAT_PEER_ACCEPTED;
}
