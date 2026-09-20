#ifndef MINI_OS_RAWCHAT_PROTOCOL_H
#define MINI_OS_RAWCHAT_PROTOCOL_H

#include <net/raw.h>

enum {
    RAWCHAT_ETHERTYPE = 0x88B5,
    RAWCHAT_SUBTYPE = 1,
    RAWCHAT_VERSION = 1,
    RAWCHAT_MESSAGE_HELLO = 1,
    RAWCHAT_MESSAGE_TEXT = 2,
    RAWCHAT_MESSAGE_BYE = 3,
    RAWCHAT_PROTOCOL_HEADER_SIZE = 14,
    RAWCHAT_WIRE_HEADER_SIZE = 14 + RAWCHAT_PROTOCOL_HEADER_SIZE,
    RAWCHAT_TEXT_MAX = 64,
    RAWCHAT_TEXT_BUFFER_SIZE = RAWCHAT_TEXT_MAX + 1
};

enum rawchat_build_result {
    RAWCHAT_BUILD_OK = 0,
    RAWCHAT_BUILD_INVALID = -1,
    RAWCHAT_BUILD_CAPACITY = -2
};

enum rawchat_parse_result {
    RAWCHAT_PARSE_OK = 0,
    RAWCHAT_PARSE_UNRELATED = 1,
    RAWCHAT_PARSE_MALFORMED = 2,
    RAWCHAT_PARSE_INVALID = -1
};

enum rawchat_peer_result {
    RAWCHAT_PEER_ACCEPTED = 0,
    RAWCHAT_PEER_GAP = 1,
    RAWCHAT_PEER_STARTED = 2,
    RAWCHAT_PEER_DUPLICATE = 3,
    RAWCHAT_PEER_NO_SESSION = 4,
    RAWCHAT_PEER_INVALID = -1
};

struct rawchat_message {
    unsigned int type;
    unsigned int session_id;
    unsigned int sequence;
    unsigned int payload_length;
    unsigned char payload[RAWCHAT_TEXT_BUFFER_SIZE];
};

struct rawchat_peer_state {
    unsigned int session_id;
    unsigned int last_sequence;
    int active;
};

int rawchat_build_frame(const unsigned char *source_mac,
                        const unsigned char *destination_mac,
                        unsigned int message_type,
                        unsigned int session_id,
                        unsigned int sequence,
                        const void *payload,
                        unsigned int payload_length,
                        void *frame,
                        unsigned int frame_capacity,
                        unsigned int *frame_length);

int rawchat_parse_frame(const void *frame,
                        unsigned int frame_length,
                        const unsigned char *local_mac,
                        const unsigned char *peer_mac,
                        struct rawchat_message *message);

void rawchat_peer_reset(struct rawchat_peer_state *state);

int rawchat_peer_accept(struct rawchat_peer_state *state,
                        const struct rawchat_message *message);

#endif
