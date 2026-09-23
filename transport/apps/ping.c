#include <net/net.h>
#include <stdio.h>

enum {
    PING_COUNT = 4,
    PING_TIMEOUT_MS = 3000
};

static const struct net_config ping_config = {
    {{10U, 0U, 2U, 15U}},
    {{255U, 255U, 255U, 0U}},
    {{10U, 0U, 2U, 2U}},
    NET_ARP_CACHE_TTL_DEFAULT_MS,
    NET_ARP_RETRY_INTERVAL_MIN_MS,
    NET_ARP_RETRY_COUNT_DEFAULT
};

static void print_address(const struct net_ipv4_addr *address)
{
    printf("%u.%u.%u.%u",
           (unsigned int)address->octets[0],
           (unsigned int)address->octets[1],
           (unsigned int)address->octets[2],
           (unsigned int)address->octets[3]);
}

int main(int argc, char **argv)
{
    static const char payload[] = "MINI-OS ICMP Echo";
    struct net_ipv4_addr destination;
    struct net_ping_result reply;
    unsigned int sent = 0U;
    unsigned int received = 0U;
    unsigned int sequence;
    int status;

    if (argc != 2 || argv == 0 || argv[1] == 0 ||
        net_ipv4_parse(argv[1], &destination) != 0) {
        printf("usage: ping <IPv4 address>\n");
        return 1;
    }
    status = net_init(&ping_config);
    if (status != 0) {
        printf("ping: network initialization failed: %d\n", status);
        return 1;
    }
    printf("PING ");
    print_address(&destination);
    printf(" (%u data bytes)\n", (unsigned int)(sizeof(payload) - 1U));

    for (sequence = 1U; sequence <= PING_COUNT; ++sequence) {
        ++sent;
        status = net_ping(&destination, sequence, payload,
                          (unsigned int)(sizeof(payload) - 1U),
                          PING_TIMEOUT_MS, &reply);
        if (status == 0) {
            ++received;
            printf("%u bytes from ", reply.payload_length);
            print_address(&reply.source);
            printf(": seq=%u time=%u ms\n",
                   reply.sequence, reply.elapsed_ms);
        } else if (status == NET_ERR_TIMEOUT) {
            printf("seq=%u: timeout\n", sequence);
        } else {
            printf("seq=%u: network error %d\n", sequence, status);
            break;
        }
    }
    printf("ping summary: %u sent, %u received, %u lost\n",
           sent, received, sent - received);
    return sent == PING_COUNT && received == sent ? 0 : 1;
}
