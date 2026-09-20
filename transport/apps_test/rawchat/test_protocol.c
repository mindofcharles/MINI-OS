#include "../../apps/rawchat/protocol.h"

#include <stdio.h>
#include <string.h>

static const unsigned char mini_mac[6] = {
    0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
};

static const unsigned char peer_mac[6] = {
    0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
};

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Raw Chat protocol test failed: %s\n", message);
        return 0;
    }
    return 1;
}

static int all_zero(const unsigned char *data, unsigned int length)
{
    unsigned int index;

    for (index = 0U; index < length; ++index) {
        if (data[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

static int test_build_and_parse(void)
{
    static const unsigned char text[] = "hello";
    unsigned char frame[NET_FRAME_MAX];
    unsigned char maximum[RAWCHAT_TEXT_MAX];
    struct rawchat_message message;
    unsigned int length;
    unsigned int index;
    int result;

    memset(frame, 0xCC, sizeof(frame));
    result = rawchat_build_frame(mini_mac, peer_mac, RAWCHAT_MESSAGE_TEXT,
                                 0x11223344U, 0x55667788U,
                                 text, sizeof(text) - 1U,
                                 frame, sizeof(frame), &length);
    if (!require(result == RAWCHAT_BUILD_OK, "build text frame") ||
        !require(length == NET_FRAME_MIN, "minimum padding length") ||
        !require(memcmp(frame, peer_mac, 6U) == 0, "destination MAC") ||
        !require(memcmp(frame + 6U, mini_mac, 6U) == 0, "source MAC") ||
        !require(frame[12] == 0x88U && frame[13] == 0xB5U,
                 "experimental EtherType") ||
        !require(frame[14] == RAWCHAT_SUBTYPE &&
                 frame[15] == RAWCHAT_VERSION &&
                 frame[16] == RAWCHAT_MESSAGE_TEXT && frame[17] == 0U,
                 "protocol discriminator") ||
        !require(frame[18] == 0x11U && frame[19] == 0x22U &&
                 frame[20] == 0x33U && frame[21] == 0x44U,
                 "network-order session") ||
        !require(frame[22] == 0x55U && frame[23] == 0x66U &&
                 frame[24] == 0x77U && frame[25] == 0x88U,
                 "network-order sequence") ||
        !require(frame[26] == 0U && frame[27] == sizeof(text) - 1U,
                 "network-order payload length") ||
        !require(memcmp(frame + RAWCHAT_WIRE_HEADER_SIZE,
                        text, sizeof(text) - 1U) == 0,
                 "text payload") ||
        !require(all_zero(frame + RAWCHAT_WIRE_HEADER_SIZE + sizeof(text) - 1U,
                          NET_FRAME_MIN - RAWCHAT_WIRE_HEADER_SIZE -
                              (sizeof(text) - 1U)),
                 "zero Ethernet padding")) {
        return 0;
    }

    memset(&message, 0, sizeof(message));
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_OK,
                 "parse text frame") ||
        !require(message.type == RAWCHAT_MESSAGE_TEXT,
                 "parsed message type") ||
        !require(message.session_id == 0x11223344U,
                 "parsed session") ||
        !require(message.sequence == 0x55667788U,
                 "parsed sequence") ||
        !require(message.payload_length == sizeof(text) - 1U,
                 "parsed payload length") ||
        !require(memcmp(message.payload, text, sizeof(text)) == 0,
                 "parsed text and terminator")) {
        return 0;
    }

    for (index = 0U; index < sizeof(maximum); ++index) {
        maximum[index] = (unsigned char)('A' + index % 26U);
    }
    if (!require(rawchat_build_frame(mini_mac, peer_mac,
                                     RAWCHAT_MESSAGE_TEXT, 1U, 1U,
                                     maximum, sizeof(maximum),
                                     frame, sizeof(frame), &length) ==
                         RAWCHAT_BUILD_OK,
                 "build maximum text") ||
        !require(length == RAWCHAT_WIRE_HEADER_SIZE + RAWCHAT_TEXT_MAX,
                 "maximum frame length") ||
        !require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_OK,
                 "parse maximum text") ||
        !require(message.payload[RAWCHAT_TEXT_MAX] == 0U,
                 "maximum text terminator")) {
        return 0;
    }
    return 1;
}

static int test_rejection(void)
{
    static const unsigned char text[] = "ok";
    unsigned char frame[NET_FRAME_MIN];
    unsigned char saved[NET_FRAME_MIN];
    unsigned char bad_mac[6];
    struct rawchat_message message;
    struct rawchat_message unchanged;
    unsigned int length;

    if (!require(rawchat_build_frame(mini_mac, peer_mac,
                                     RAWCHAT_MESSAGE_TEXT, 7U, 1U,
                                     text, sizeof(text) - 1U,
                                     frame, sizeof(frame), &length) ==
                         RAWCHAT_BUILD_OK,
                 "build rejection fixture")) {
        return 0;
    }
    memcpy(saved, frame, sizeof(saved));
    memset(&message, 0xA5, sizeof(message));
    unchanged = message;

    frame[0] ^= 1U;
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_UNRELATED,
                 "wrong destination is unrelated")) {
        return 0;
    }
    memcpy(frame, saved, sizeof(frame));
    frame[12] = 0x08U;
    frame[13] = 0x00U;
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_UNRELATED,
                 "wrong EtherType is unrelated")) {
        return 0;
    }
    memcpy(frame, saved, sizeof(frame));
    frame[14] ^= 1U;
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_UNRELATED,
                 "wrong subtype is unrelated")) {
        return 0;
    }

    memcpy(frame, saved, sizeof(frame));
    frame[15] += 1U;
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_MALFORMED,
                 "wrong version is malformed") ||
        !require(memcmp(&message, &unchanged, sizeof(message)) == 0,
                 "failed parse preserves output")) {
        return 0;
    }
    memcpy(frame, saved, sizeof(frame));
    frame[17] = 1U;
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_MALFORMED,
                 "nonzero flags are malformed")) {
        return 0;
    }
    memcpy(frame, saved, sizeof(frame));
    memset(frame + 18U, 0, 4U);
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_MALFORMED,
                 "zero session is malformed")) {
        return 0;
    }
    memcpy(frame, saved, sizeof(frame));
    memset(frame + 22U, 0, 4U);
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_MALFORMED,
                 "zero text sequence is malformed")) {
        return 0;
    }
    memcpy(frame, saved, sizeof(frame));
    frame[26] = 0U;
    frame[27] = RAWCHAT_TEXT_MAX;
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_MALFORMED,
                 "truncated declared payload is malformed")) {
        return 0;
    }
    memcpy(frame, saved, sizeof(frame));
    frame[28] = 10U;
    if (!require(rawchat_parse_frame(frame, length, peer_mac, mini_mac,
                                     &message) == RAWCHAT_PARSE_MALFORMED,
                 "control text is malformed") ||
        !require(rawchat_parse_frame(frame, NET_FRAME_MIN - 1U,
                                     peer_mac, mini_mac, &message) ==
                         RAWCHAT_PARSE_MALFORMED,
                 "short raw frame is malformed")) {
        return 0;
    }

    memcpy(bad_mac, mini_mac, sizeof(bad_mac));
    bad_mac[0] |= 1U;
    if (!require(rawchat_build_frame(0, peer_mac, RAWCHAT_MESSAGE_HELLO,
                                     1U, 0U, 0, 0U, frame, sizeof(frame),
                                     &length) == RAWCHAT_BUILD_INVALID,
                 "null source rejected") ||
        !require(rawchat_build_frame(mini_mac, mini_mac,
                                     RAWCHAT_MESSAGE_HELLO, 1U, 0U, 0, 0U,
                                     frame, sizeof(frame), &length) ==
                         RAWCHAT_BUILD_INVALID,
                 "equal MAC addresses rejected") ||
        !require(rawchat_build_frame(bad_mac, peer_mac,
                                     RAWCHAT_MESSAGE_HELLO, 1U, 0U, 0, 0U,
                                     frame, sizeof(frame), &length) ==
                         RAWCHAT_BUILD_INVALID,
                 "multicast source rejected") ||
        !require(rawchat_build_frame(mini_mac, peer_mac,
                                     RAWCHAT_MESSAGE_HELLO, 0U, 0U, 0, 0U,
                                     frame, sizeof(frame), &length) ==
                         RAWCHAT_BUILD_INVALID,
                 "zero session build rejected") ||
        !require(rawchat_build_frame(mini_mac, peer_mac,
                                     RAWCHAT_MESSAGE_TEXT, 1U, 1U,
                                     text, sizeof(text) - 1U,
                                     frame, NET_FRAME_MIN - 1U, &length) ==
                         RAWCHAT_BUILD_CAPACITY,
                 "small output capacity rejected")) {
        return 0;
    }
    return 1;
}

static int test_peer_state(void)
{
    struct rawchat_peer_state state;
    struct rawchat_message message;

    rawchat_peer_reset(&state);
    memset(&message, 0, sizeof(message));
    message.type = RAWCHAT_MESSAGE_TEXT;
    message.session_id = 10U;
    message.sequence = 1U;
    message.payload_length = 1U;
    message.payload[0] = 'A';
    if (!require(rawchat_peer_accept(&state, &message) ==
                     RAWCHAT_PEER_NO_SESSION,
                 "text before HELLO rejected")) {
        return 0;
    }

    message.type = RAWCHAT_MESSAGE_HELLO;
    message.sequence = 0U;
    message.payload_length = 0U;
    if (!require(rawchat_peer_accept(&state, &message) ==
                     RAWCHAT_PEER_STARTED,
                 "HELLO starts session") ||
        !require(state.active && state.session_id == 10U &&
                     state.last_sequence == 0U,
                 "HELLO state") ||
        !require(rawchat_peer_accept(&state, &message) ==
                     RAWCHAT_PEER_DUPLICATE,
                 "duplicate HELLO ignored")) {
        return 0;
    }

    message.type = RAWCHAT_MESSAGE_TEXT;
    message.sequence = 1U;
    message.payload_length = 1U;
    message.payload[0] = 'A';
    if (!require(rawchat_peer_accept(&state, &message) ==
                     RAWCHAT_PEER_ACCEPTED,
                 "next text accepted") ||
        !require(rawchat_peer_accept(&state, &message) ==
                     RAWCHAT_PEER_DUPLICATE,
                 "duplicate text ignored")) {
        return 0;
    }
    message.sequence = 3U;
    if (!require(rawchat_peer_accept(&state, &message) == RAWCHAT_PEER_GAP,
                 "sequence gap reported") ||
        !require(state.last_sequence == 3U, "gap advances sequence")) {
        return 0;
    }

    message.type = RAWCHAT_MESSAGE_BYE;
    message.sequence = 4U;
    message.payload_length = 0U;
    if (!require(rawchat_peer_accept(&state, &message) ==
                     RAWCHAT_PEER_ACCEPTED,
                 "BYE accepted") ||
        !require(!state.active, "BYE ends session")) {
        return 0;
    }
    message.type = RAWCHAT_MESSAGE_TEXT;
    message.sequence = 5U;
    message.payload_length = 1U;
    message.payload[0] = 'B';
    if (!require(rawchat_peer_accept(&state, &message) ==
                     RAWCHAT_PEER_NO_SESSION,
                 "text after BYE rejected")) {
        return 0;
    }

    message.type = RAWCHAT_MESSAGE_HELLO;
    message.session_id = 11U;
    message.sequence = 0U;
    message.payload_length = 0U;
    return require(rawchat_peer_accept(&state, &message) ==
                       RAWCHAT_PEER_STARTED &&
                       state.active && state.session_id == 11U,
                   "new HELLO replaces old session");
}

int main(void)
{
    if (!test_build_and_parse() || !test_rejection() ||
        !test_peer_state()) {
        return 1;
    }
    puts("Raw Chat protocol tests: PASS");
    return 0;
}
