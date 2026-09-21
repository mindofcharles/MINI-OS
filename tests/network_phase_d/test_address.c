#include "deterministic_backend.h"

#include "../../transport/lib/net/address.h"
#include "../../transport/lib/net/internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static struct net_context saved_context;

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Phase D address test failed: %s\n", message);
        return 0;
    }
    return 1;
}

static void set_ipv4(struct net_ipv4_addr *address, unsigned int first,
                     unsigned int second, unsigned int third,
                     unsigned int fourth)
{
    address->octets[0] = (unsigned char)first;
    address->octets[1] = (unsigned char)second;
    address->octets[2] = (unsigned char)third;
    address->octets[3] = (unsigned char)fourth;
}

static struct net_config valid_config(void)
{
    struct net_config config;

    set_ipv4(&config.address, 10U, 0U, 2U, 15U);
    set_ipv4(&config.netmask, 255U, 255U, 255U, 0U);
    set_ipv4(&config.gateway, 10U, 0U, 2U, 2U);
    config.arp_cache_ttl_ms = NET_ARP_CACHE_TTL_DEFAULT_MS;
    config.arp_retry_interval_ms = NET_ARP_RETRY_INTERVAL_MIN_MS;
    config.arp_retry_count = NET_ARP_RETRY_COUNT_DEFAULT;
    return config;
}

static int address_equals(const struct net_ipv4_addr *address,
                          unsigned int first, unsigned int second,
                          unsigned int third, unsigned int fourth)
{
    return address->octets[0] == first && address->octets[1] == second &&
           address->octets[2] == third && address->octets[3] == fourth;
}

static int test_parser(void)
{
    static const char *const invalid[] = {
        "", "1", "1.2.3", "1.2.3.4.5", ".1.2.3", "1..2.3",
        "1.2.3.", "256.0.0.1", "999.0.0.1", "01.2.3.4",
        "1.02.3.4", "1.2.003.4", "+1.2.3.4", "-1.2.3.4",
        " 1.2.3.4", "1.2.3.4 ", "1.2.3.4x", "0x1.2.3.4"
    };
    struct net_ipv4_addr address;
    struct net_ipv4_addr sentinel;
    unsigned int index;

    memset(&address, 0xA5, sizeof(address));
    if (!require(net_ipv4_parse("0.0.0.0", &address) == 0 &&
                 address_equals(&address, 0U, 0U, 0U, 0U),
                 "parse zero address") ||
        !require(net_ipv4_parse("255.255.255.255", &address) == 0 &&
                 address_equals(&address, 255U, 255U, 255U, 255U),
                 "parse maximum address") ||
        !require(net_ipv4_parse("10.0.2.15", &address) == 0 &&
                 address_equals(&address, 10U, 0U, 2U, 15U),
                 "parse configured address") ||
        !require(net_ipv4_parse(0, &address) == NET_ERR_INVALID,
                 "reject null text") ||
        !require(net_ipv4_parse("1.2.3.4", 0) == NET_ERR_INVALID,
                 "reject null output")) {
        return 0;
    }

    for (index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        memset(&address, 0xA5, sizeof(address));
        sentinel = address;
        if (!require(net_ipv4_parse(invalid[index], &address) ==
                         NET_ERR_INVALID,
                     "reject noncanonical address") ||
            !require(memcmp(&address, &sentinel, sizeof(address)) == 0,
                     "parse failure preserves output")) {
            return 0;
        }
    }
    return 1;
}

static int expect_invalid_config(const struct net_config *config,
                                 const char *message)
{
    return require(net_config_validate(config) == NET_ERR_INVALID, message);
}

static int test_config(void)
{
    struct net_config config;

    config = valid_config();
    if (!require(net_config_validate(&config) == 0, "default config") ||
        !expect_invalid_config(0, "reject null config")) {
        return 0;
    }

    config = valid_config();
    set_ipv4(&config.netmask, 128U, 0U, 0U, 0U);
    if (!require(net_config_validate(&config) == 0, "accept /1 config")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.address, 10U, 0U, 2U, 13U);
    set_ipv4(&config.netmask, 255U, 255U, 255U, 252U);
    set_ipv4(&config.gateway, 10U, 0U, 2U, 14U);
    if (!require(net_config_validate(&config) == 0, "accept /30 config")) {
        return 0;
    }
    config = valid_config();
    config.arp_cache_ttl_ms = NET_TIMEOUT_MAX_MS;
    config.arp_retry_interval_ms = NET_TIMEOUT_MAX_MS;
    config.arp_retry_count = UINT_MAX;
    if (!require(net_config_validate(&config) == 0,
                 "accept timing upper bounds")) {
        return 0;
    }

    config = valid_config();
    set_ipv4(&config.netmask, 0U, 0U, 0U, 0U);
    if (!expect_invalid_config(&config, "reject /0 mask")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.netmask, 255U, 255U, 255U, 254U);
    if (!expect_invalid_config(&config, "reject /31 mask")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.netmask, 255U, 255U, 255U, 255U);
    if (!expect_invalid_config(&config, "reject /32 mask")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.netmask, 255U, 0U, 255U, 0U);
    if (!expect_invalid_config(&config, "reject noncontiguous mask")) {
        return 0;
    }

    config = valid_config();
    set_ipv4(&config.address, 10U, 0U, 2U, 0U);
    if (!expect_invalid_config(&config, "reject local network address")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.address, 10U, 0U, 2U, 255U);
    if (!expect_invalid_config(&config, "reject local broadcast")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.address, 0U, 0U, 2U, 15U);
    if (!expect_invalid_config(&config, "reject zero-net address")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.address, 127U, 0U, 0U, 1U);
    if (!expect_invalid_config(&config, "reject loopback address")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.address, 224U, 0U, 0U, 1U);
    if (!expect_invalid_config(&config, "reject multicast address")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.address, 240U, 0U, 0U, 1U);
    if (!expect_invalid_config(&config, "reject reserved address")) {
        return 0;
    }

    config = valid_config();
    set_ipv4(&config.gateway, 10U, 0U, 3U, 2U);
    if (!expect_invalid_config(&config, "reject off-link gateway")) {
        return 0;
    }
    config = valid_config();
    config.gateway = config.address;
    if (!expect_invalid_config(&config, "reject local address as gateway")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.gateway, 10U, 0U, 2U, 0U);
    if (!expect_invalid_config(&config, "reject network gateway")) {
        return 0;
    }
    config = valid_config();
    set_ipv4(&config.gateway, 10U, 0U, 2U, 255U);
    if (!expect_invalid_config(&config, "reject broadcast gateway")) {
        return 0;
    }

    config = valid_config();
    config.arp_cache_ttl_ms = 0U;
    if (!expect_invalid_config(&config, "reject zero cache TTL")) {
        return 0;
    }
    config = valid_config();
    config.arp_cache_ttl_ms = (unsigned int)NET_TIMEOUT_MAX_MS + 1U;
    if (!expect_invalid_config(&config, "reject excessive cache TTL")) {
        return 0;
    }
    config = valid_config();
    config.arp_retry_interval_ms = NET_ARP_RETRY_INTERVAL_MIN_MS - 1U;
    if (!expect_invalid_config(&config, "reject short retry interval")) {
        return 0;
    }
    config = valid_config();
    config.arp_retry_interval_ms = (unsigned int)NET_TIMEOUT_MAX_MS + 1U;
    if (!expect_invalid_config(&config, "reject excessive retry interval")) {
        return 0;
    }
    config = valid_config();
    config.arp_retry_count = 0U;
    return expect_invalid_config(&config, "reject zero retry count");
}

static void prepare_atomicity_check(void)
{
    memset(&net_global_context, 0xA5, sizeof(net_global_context));
    net_global_context.initialized = 0;
    memcpy(&saved_context, &net_global_context, sizeof(saved_context));
}

static int require_unchanged(const char *message)
{
    return require(memcmp(&net_global_context, &saved_context,
                          sizeof(net_global_context)) == 0,
                   message);
}

static int expect_device_failure(const struct net_device_info *info,
                                 int expected, const char *message)
{
    struct net_config config = valid_config();

    phase_d_backend_reset();
    if (!require(phase_d_backend_set_device_info(info) == 0,
                 "configure failing device")) {
        return 0;
    }
    prepare_atomicity_check();
    if (!require(net_init(&config) == expected, message) ||
        !require_unchanged("device failure preserves context") ||
        !require(phase_d_backend_transmit_count() == 0U,
                 "initialization never transmits")) {
        return 0;
    }
    return 1;
}

static int test_init_failures(void)
{
    struct net_config config = valid_config();
    struct net_device_info base;
    struct net_device_info info;
    unsigned int required_flags[] = {
        NET_DRIVER_FLAG_POLLING,
        NET_DRIVER_FLAG_IRQ_MASKED,
        NET_DRIVER_FLAG_FCS_STRIPPED,
        NET_DRIVER_FLAG_SYNC_TX
    };
    unsigned int index;

    phase_d_backend_reset();
    if (!require(net_get_info(&base) == 0, "read default device")) {
        return 0;
    }

    phase_d_backend_reset();
    prepare_atomicity_check();
    if (!require(phase_d_backend_set_next_info_error(SYS_ERR_UNAVAILABLE) == 0,
                 "inject unavailable query") ||
        !require(net_init(&config) == NET_ERR_UNAVAILABLE,
                 "map unavailable query") ||
        !require_unchanged("unavailable query preserves context")) {
        return 0;
    }
    phase_d_backend_reset();
    prepare_atomicity_check();
    if (!require(phase_d_backend_set_next_info_error(SYS_ERR_TIMEOUT) == 0,
                 "inject failed query") ||
        !require(net_init(&config) == NET_ERR_DEVICE,
                 "map failed query") ||
        !require_unchanged("failed query preserves context")) {
        return 0;
    }

    info = base;
    info.state = NET_DEVICE_UNAVAILABLE;
    if (!expect_device_failure(&info, NET_ERR_UNAVAILABLE,
                               "reject unavailable state")) {
        return 0;
    }
    info = base;
    info.flags &= ~NET_DRIVER_FLAG_AVAILABLE;
    if (!expect_device_failure(&info, NET_ERR_UNAVAILABLE,
                               "reject unavailable flag")) {
        return 0;
    }
    info = base;
    info.state = NET_DEVICE_FATAL;
    info.flags &= ~NET_DRIVER_FLAG_AVAILABLE;
    if (!expect_device_failure(&info, NET_ERR_DEVICE,
                               "reject fatal state")) {
        return 0;
    }
    info = base;
    info.abi_version += 1U;
    info.flags &= ~NET_DRIVER_FLAG_AVAILABLE;
    if (!expect_device_failure(&info, NET_ERR_DEVICE,
                               "reject incompatible ABI")) {
        return 0;
    }
    for (index = 0U; index < sizeof(required_flags) / sizeof(required_flags[0]);
         ++index) {
        info = base;
        info.flags &= ~required_flags[index];
        if (!expect_device_failure(&info, NET_ERR_DEVICE,
                                   "reject missing driver contract flag")) {
            return 0;
        }
    }
    info = base;
    info.mtu -= 1U;
    if (!expect_device_failure(&info, NET_ERR_DEVICE,
                               "reject incompatible MTU")) {
        return 0;
    }
    info = base;
    info.frame_min += 1U;
    if (!expect_device_failure(&info, NET_ERR_DEVICE,
                               "reject incompatible frame minimum")) {
        return 0;
    }
    info = base;
    info.frame_max -= 1U;
    if (!expect_device_failure(&info, NET_ERR_DEVICE,
                               "reject incompatible frame maximum")) {
        return 0;
    }
    info = base;
    memset(info.mac, 0, sizeof(info.mac));
    if (!expect_device_failure(&info, NET_ERR_DEVICE,
                               "reject zero MAC")) {
        return 0;
    }
    info = base;
    info.mac[0] |= 1U;
    if (!expect_device_failure(&info, NET_ERR_DEVICE,
                               "reject multicast MAC")) {
        return 0;
    }

    phase_d_backend_reset();
    prepare_atomicity_check();
    config.arp_retry_count = 0U;
    if (!require(phase_d_backend_set_next_info_error(SYS_ERR_DEVICE) == 0,
                 "arm unused device failure") ||
        !require(net_init(0) == NET_ERR_INVALID,
                 "reject null initialization config") ||
        !require_unchanged("null config preserves context") ||
        !require(net_init(&config) == NET_ERR_INVALID,
                 "validate config before device query") ||
        !require_unchanged("invalid config preserves context") ||
        !require(net_get_info(&info) == SYS_ERR_DEVICE,
                 "invalid config does not consume device query")) {
        return 0;
    }
    return 1;
}

static int test_init_success(void)
{
    static const unsigned char expected_mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
    };
    struct net_config config = valid_config();
    struct net_config expected = config;
    struct net_device_info info;

    phase_d_backend_reset();
    if (!require(net_get_info(&info) == 0, "query successful device")) {
        return 0;
    }
    info.io_base = 0U;
    info.irq = 0U;
    info.flags |= 0x80000000U;
    if (!require(phase_d_backend_set_device_info(&info) == 0,
                 "configure protocol-independent resources")) {
        return 0;
    }
    memset(&net_global_context, 0xA5, sizeof(net_global_context));
    net_global_context.initialized = 0;
    if (!require(net_init(&config) == 0, "initialize stack") ||
        !require(net_global_context.initialized == 1,
                 "commit initialized state") ||
        !require(memcmp(&net_global_context.config, &expected,
                        sizeof(expected)) == 0,
                 "copy configuration") ||
        !require(memcmp(net_global_context.device_mac.octets, expected_mac,
                        sizeof(expected_mac)) == 0,
                 "copy device MAC") ||
        !require(net_global_context.tx_frame[0] == 0U &&
                 net_global_context.tx_frame[NET_FRAME_MAX - 1U] == 0U &&
                 net_global_context.rx_frame[0] == 0U &&
                 net_global_context.rx_frame[NET_FRAME_MAX - 1U] == 0U,
                 "clear fixed frame buffers") ||
        !require(net_global_context.counters.received_frames == 0U &&
                 net_global_context.pending_arp.active == 0 &&
                 net_global_context.pending_echo.active == 0,
                 "clear protocol state") ||
        !require(phase_d_backend_transmit_count() == 0U,
                 "successful initialization does not transmit")) {
        return 0;
    }

    config.address.octets[0] = 192U;
    info.mac[0] = 0x02U;
    if (!require(memcmp(&net_global_context.config, &expected,
                        sizeof(expected)) == 0,
                 "configuration has value ownership") ||
        !require(memcmp(net_global_context.device_mac.octets, expected_mac,
                        sizeof(expected_mac)) == 0,
                 "MAC has value ownership")) {
        return 0;
    }

    memcpy(&saved_context, &net_global_context, sizeof(saved_context));
    config = valid_config();
    if (!require(net_init(&config) == NET_ERR_STATE,
                 "reject repeated initialization") ||
        !require_unchanged("repeated initialization preserves context")) {
        return 0;
    }
    config.arp_retry_count = 0U;
    return require(net_init(&config) == NET_ERR_INVALID,
                   "invalid input takes precedence over lifecycle state") &&
           require_unchanged("invalid repeated initialization preserves context");
}

int main(void)
{
    if (!test_parser() || !test_config() || !test_init_failures() ||
        !test_init_success()) {
        return 1;
    }
    puts("Phase D address and initialization tests passed.");
    return 0;
}
