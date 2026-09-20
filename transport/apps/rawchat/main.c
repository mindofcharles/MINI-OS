#include <stdio.h>
#include <string.h>
#include <platform.h>
#include "protocol.h"

#define SCREEN_WIDTH 78
#define HISTORY_ROWS 16
#define HISTORY_FIRST_ROW 2
#define INPUT_ROW 19
#define STATUS_ROW 20
#define CONTROLS_ROW 22
#define RECEIVE_BUDGET 8

static const unsigned char peer_mac[6] = {
    0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
};

static unsigned char receive_frame[NET_FRAME_MAX];
static unsigned char transmit_frame[NET_FRAME_MAX];
static char history[HISTORY_ROWS][SCREEN_WIDTH + 1];
static char input_text[RAWCHAT_TEXT_BUFFER_SIZE];
static char status_text[SCREEN_WIDTH + 1];
static unsigned int input_length;
static unsigned int local_session;
static unsigned int local_sequence;
static int layout_dirty;
static int history_dirty;
static int input_dirty;
static int status_dirty;

static void write_row(int row, const char *text)
{
    char output[SCREEN_WIDTH];
    int length;

    length = 0;
    while (length < SCREEN_WIDTH && text[length] != '\0') {
        output[length] = text[length];
        ++length;
    }
    while (length < SCREEN_WIDTH) {
        output[length] = ' ';
        ++length;
    }
    set_cursor(row, 0);
    (void)write(1, output, sizeof(output));
}

static void set_status(const char *text)
{
    if (strcmp(status_text, text) != 0) {
        strncpy(status_text, text, SCREEN_WIDTH);
        status_text[SCREEN_WIDTH] = '\0';
        status_dirty = 1;
    }
}

static void set_error_status(const char *operation, int error)
{
    char message[SCREEN_WIDTH + 1];

    snprintf(message, sizeof(message), "%s failed: %d", operation, error);
    set_status(message);
}

static void append_history_bytes(const char *prefix,
                                 const unsigned char *text,
                                 unsigned int length)
{
    unsigned int prefix_length;
    unsigned int copy_length;
    int row;

    for (row = 0; row < HISTORY_ROWS - 1; ++row) {
        memcpy(history[row], history[row + 1], sizeof(history[row]));
    }
    memset(history[HISTORY_ROWS - 1], 0,
           sizeof(history[HISTORY_ROWS - 1]));

    prefix_length = (unsigned int)strlen(prefix);
    if (prefix_length > SCREEN_WIDTH) {
        prefix_length = SCREEN_WIDTH;
    }
    memcpy(history[HISTORY_ROWS - 1], prefix, prefix_length);
    copy_length = length;
    if (copy_length > SCREEN_WIDTH - prefix_length) {
        copy_length = SCREEN_WIDTH - prefix_length;
    }
    if (copy_length != 0U) {
        memcpy(history[HISTORY_ROWS - 1] + prefix_length, text,
               copy_length);
    }
    history[HISTORY_ROWS - 1][prefix_length + copy_length] = '\0';
    history_dirty = 1;
}

static void append_history(const char *text)
{
    append_history_bytes("* ", (const unsigned char *)text,
                         (unsigned int)strlen(text));
}

static void render_screen(void)
{
    char input_row[SCREEN_WIDTH + 1];
    char status_row[SCREEN_WIDTH + 1];
    int row;

    if (!layout_dirty && !history_dirty && !input_dirty && !status_dirty) {
        return;
    }

    if (layout_dirty) {
        write_row(0, "====================== MINI-OS Raw Ethernet Chat ======================");
        write_row(1, "Messages use one local experimental Ethernet protocol; no IP or TCP.");
        write_row(18, "----------------------------------------------------------------------------");
        write_row(21, "");
        write_row(CONTROLS_ROW,
                  "[Enter: send] [Backspace: edit] [ESC: exit] [64 printable ASCII]");
        write_row(23, "");
        history_dirty = 1;
        input_dirty = 1;
        status_dirty = 1;
    }
    if (history_dirty) {
        for (row = 0; row < HISTORY_ROWS; ++row) {
            write_row(HISTORY_FIRST_ROW + row, history[row]);
        }
    }
    if (input_dirty) {
        snprintf(input_row, sizeof(input_row), "You> %s", input_text);
        write_row(INPUT_ROW, input_row);
    }
    if (status_dirty) {
        snprintf(status_row, sizeof(status_row), "Status: %s", status_text);
        write_row(STATUS_ROW, status_row);
    }
    move_cursor(INPUT_ROW, 5 + (int)input_length);
    layout_dirty = 0;
    history_dirty = 0;
    input_dirty = 0;
    status_dirty = 0;
}

static unsigned int make_session_id(const struct net_device_info *info)
{
    unsigned int value;
    int result;

    value = 0U;
    result = get_random(&value, sizeof(value));
    if (result != (int)sizeof(value) || value == 0U) {
        value = clock_monotonic_ms() ^ 0x4D494E49U;
        value ^= (unsigned int)info->mac[2] << 24;
        value ^= (unsigned int)info->mac[3] << 16;
        value ^= (unsigned int)info->mac[4] << 8;
        value ^= (unsigned int)info->mac[5];
        if (value == 0U) {
            value = 1U;
        }
    }
    return value;
}

static int send_message(const struct net_device_info *info,
                        unsigned int type,
                        unsigned int sequence,
                        const void *payload,
                        unsigned int payload_length)
{
    unsigned int frame_length;
    int result;

    result = rawchat_build_frame(info->mac, peer_mac, type, local_session,
                                 sequence, payload, payload_length,
                                 transmit_frame, sizeof(transmit_frame),
                                 &frame_length);
    if (result != RAWCHAT_BUILD_OK) {
        return result;
    }
    result = net_send_frame(transmit_frame, frame_length);
    if (result != (int)frame_length) {
        return result;
    }
    return 0;
}

static int send_hello(const struct net_device_info *info)
{
    return send_message(info, RAWCHAT_MESSAGE_HELLO, 0U, 0, 0U);
}

static int restart_local_session(const struct net_device_info *info)
{
    int result;

    local_session = make_session_id(info);
    local_sequence = 0U;
    result = send_hello(info);
    if (result != 0) {
        set_error_status("New-session HELLO", result);
    }
    return result;
}

static void send_input(const struct net_device_info *info)
{
    unsigned int sequence;
    int result;

    if (input_length == 0U) {
        set_status("Type a message before pressing Enter.");
        return;
    }
    if (local_sequence == 0xFFFFFFFFU) {
        if (restart_local_session(info) != 0) {
            return;
        }
    }
    else if (local_sequence == 0U) {
        result = send_hello(info);
        if (result != 0) {
            set_error_status("HELLO", result);
            return;
        }
    }
    sequence = local_sequence + 1U;
    result = send_message(info, RAWCHAT_MESSAGE_TEXT, sequence,
                          input_text, input_length);
    if (result != 0) {
        set_error_status("Send", result);
        return;
    }

    local_sequence = sequence;
    append_history_bytes("You: ", (const unsigned char *)input_text,
                         input_length);
    input_length = 0U;
    input_text[0] = '\0';
    input_dirty = 1;
    set_status("Message sent.");
}

static void handle_key(const struct net_device_info *info, int key,
                       int *running)
{
    unsigned int sequence;

    if (key == 27) {
        if (local_sequence != 0xFFFFFFFFU) {
            sequence = local_sequence + 1U;
            (void)send_message(info, RAWCHAT_MESSAGE_BYE, sequence, 0, 0U);
        }
        *running = 0;
        return;
    }
    if (key == 8 || key == 127) {
        if (input_length != 0U) {
            --input_length;
            input_text[input_length] = '\0';
            input_dirty = 1;
        }
        return;
    }
    if (key == 10 || key == 13) {
        send_input(info);
        return;
    }
    if (key >= 32 && key <= 126) {
        if (input_length >= RAWCHAT_TEXT_MAX) {
            set_status("Message limit reached: 64 characters.");
            return;
        }
        input_text[input_length] = (char)key;
        ++input_length;
        input_text[input_length] = '\0';
        input_dirty = 1;
    }
}

static void handle_received_message(const struct net_device_info *info,
                                    struct rawchat_peer_state *peer_state,
                                    const struct rawchat_message *message)
{
    int result;
    int send_result;

    result = rawchat_peer_accept(peer_state, message);
    if (result == RAWCHAT_PEER_DUPLICATE) {
        set_status("A duplicate peer message was ignored.");
        return;
    }
    if (result == RAWCHAT_PEER_NO_SESSION) {
        set_status("A peer message without a current HELLO was ignored.");
        return;
    }
    if (result == RAWCHAT_PEER_INVALID) {
        set_status("An invalid peer state transition was ignored.");
        return;
    }
    if (result == RAWCHAT_PEER_GAP) {
        append_history("One or more peer messages were lost.");
    }

    if (message->type == RAWCHAT_MESSAGE_HELLO) {
        append_history("Peer session started.");
        send_result = send_hello(info);
        if (send_result != 0) {
            set_error_status("HELLO reply", send_result);
        }
        else {
            set_status("Peer is ready; full-duplex messaging is active.");
        }
    }
    else if (message->type == RAWCHAT_MESSAGE_TEXT) {
        append_history_bytes("Peer: ", message->payload,
                             message->payload_length);
        set_status("Message received.");
    }
    else {
        append_history("Peer session ended.");
        set_status("Waiting for a new peer HELLO.");
    }
}

static void receive_messages(const struct net_device_info *info,
                             struct rawchat_peer_state *peer_state)
{
    struct rawchat_message message;
    int count;
    int result;
    int parse_result;

    for (count = 0; count < RECEIVE_BUDGET; ++count) {
        result = net_recv_frame(receive_frame, sizeof(receive_frame));
        if (result == 0) {
            return;
        }
        if (result < 0) {
            set_error_status("Receive", result);
            return;
        }
        parse_result = rawchat_parse_frame(receive_frame,
                                           (unsigned int)result,
                                           info->mac, peer_mac, &message);
        if (parse_result == RAWCHAT_PARSE_OK) {
            handle_received_message(info, peer_state, &message);
        }
        else if (parse_result == RAWCHAT_PARSE_MALFORMED) {
            set_status("A malformed Raw Chat frame was ignored.");
        }
    }
}

static int device_is_compatible(const struct net_device_info *info)
{
    unsigned int required_flags;

    required_flags = NET_DRIVER_FLAG_AVAILABLE |
                     NET_DRIVER_FLAG_FCS_STRIPPED |
                     NET_DRIVER_FLAG_SYNC_TX;
    return info->abi_version == NET_RAW_ABI_VERSION &&
           info->state == NET_DEVICE_READY &&
           (info->flags & required_flags) == required_flags &&
           info->frame_min == NET_FRAME_MIN &&
           info->frame_max == NET_FRAME_MAX &&
           info->mtu == NET_IPV4_MTU;
}

int main(void)
{
    struct net_device_info info;
    struct rawchat_peer_state peer_state;
    int key;
    int result;
    int running;

    result = net_get_info(&info);
    if (result != 0) {
        printf("Network device query failed: %d\n", result);
        return 1;
    }
    if (!device_is_compatible(&info)) {
        puts("The raw network device is unavailable or incompatible.");
        return 1;
    }

    memset(history, 0, sizeof(history));
    memset(input_text, 0, sizeof(input_text));
    memset(status_text, 0, sizeof(status_text));
    input_length = 0U;
    rawchat_peer_reset(&peer_state);
    local_session = make_session_id(&info);
    local_sequence = 0U;
    layout_dirty = 1;
    history_dirty = 0;
    input_dirty = 0;
    status_dirty = 0;
    append_history("Local session started.");
    result = send_hello(&info);
    if (result == 0) {
        set_status("HELLO sent; messages can be typed immediately.");
    }
    else {
        set_error_status("Initial HELLO", result);
    }

    save_screen();
    clear_screen();
    running = 1;
    while (running) {
        render_screen();
        key = kbd_poll_key();
        if (key > 0) {
            handle_key(&info, key, &running);
        }
        if (running) {
            receive_messages(&info, &peer_state);
        }
    }
    restore_screen();
    puts("Raw Ethernet Chat closed.");
    return 0;
}
