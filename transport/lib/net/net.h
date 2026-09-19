#ifndef MINI_OS_NET_H
#define MINI_OS_NET_H

/* Stable public results returned by the application-level network stack. */
enum net_error {
    NET_ERR_INVALID = -1,      /* Invalid argument or configuration. */
    NET_ERR_STATE = -2,        /* Stack lifecycle does not permit the call. */
    NET_ERR_UNAVAILABLE = -3,  /* The raw network device is unavailable. */
    NET_ERR_DEVICE = -4,       /* The raw device operation failed. */
    NET_ERR_TIMEOUT = -5,      /* The caller's overall deadline expired. */
    NET_ERR_CANCELLED = -6,    /* Nonblocking cancellation was requested. */
    NET_ERR_NO_ROUTE = -7,     /* Static routing cannot reach the address. */
    NET_ERR_BUSY = -8          /* Another synchronous operation is active. */
};

enum {
    NET_TIMEOUT_MAX_MS = 2147483647,
    NET_ARP_CACHE_TTL_DEFAULT_MS = 60000,
    NET_ARP_RETRY_INTERVAL_MIN_MS = 1000,
    NET_ARP_RETRY_COUNT_DEFAULT = 3,
    NET_ICMP_ECHO_PAYLOAD_MAX = 1472
};

struct net_ipv4_addr {
    unsigned char octets[4];
};

struct net_mac_addr {
    unsigned char octets[6];
};

struct net_config {
    struct net_ipv4_addr address;
    struct net_ipv4_addr netmask;
    struct net_ipv4_addr gateway;
    unsigned int arp_cache_ttl_ms;
    unsigned int arp_retry_interval_ms;
    unsigned int arp_retry_count;
};

struct net_ping_result {
    struct net_ipv4_addr source;
    unsigned int sequence;
    unsigned int elapsed_ms;
    unsigned int payload_length;
};

/*
 * Every timeout_ms argument is a relative duration no greater than
 * NET_TIMEOUT_MAX_MS.  Zero permits one nonblocking receive and timer pass.
 */

/* Accepts exactly four decimal components without whitespace or signs. */
int net_ipv4_parse(const char *text, struct net_ipv4_addr *address);

/* Copies a valid static configuration and initializes the single stack. */
int net_init(const struct net_config *config);

/* Returns a positive action count, zero for no work, or a net_error value. */
int net_poll(unsigned int timeout_ms);

/* Resolves one on-link address within one unchanged overall deadline. */
int net_resolve_arp(const struct net_ipv4_addr *address,
                    struct net_mac_addr *mac,
                    unsigned int timeout_ms);

/*
 * Sequence must fit 16 bits, payload length must not exceed the public
 * maximum, payload may be null only at zero length, and result must be
 * nonnull.
 */
int net_ping(const struct net_ipv4_addr *address,
             unsigned int sequence,
             const void *payload,
             unsigned int payload_length,
             unsigned int timeout_ms,
             struct net_ping_result *result);

#endif
