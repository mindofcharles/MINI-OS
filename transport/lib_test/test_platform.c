#include <platform.h>
#include <stdio.h>
#include <string.h>

#define SYS_ERR_INVALID (-1)
#define SYS_ERR_RANGE (-5)
#define SYS_ERR_UNAVAILABLE (-6)
#define RANDOM_REQUEST_MAX 1024U

static int wait_with_keyboard_poll(void)
{
    unsigned int start = clock_monotonic_ms();
    unsigned long attempts = 0UL;

    while ((unsigned int)(clock_monotonic_ms() - start) < 20U) {
        if (kbd_poll_key() < 0) {
            return 0;
        }
        ++attempts;
        if (attempts == 100000000UL) {
            return 0;
        }
    }
    return 1;
}

static int timer_during_file_reads(void)
{
    unsigned char buffer[512];
    unsigned int start;
    unsigned long attempts = 0UL;
    FILE *stream = fopen("/transport/lib_test/test_platform.c", "r");

    if (stream == NULL) {
        return 0;
    }
    start = clock_monotonic_ms();
    while ((unsigned int)(clock_monotonic_ms() - start) < 5U) {
        if (fseek(stream, 0L, SEEK_SET) != 0 ||
            fread(buffer, 1U, sizeof(buffer), stream) == 0U ||
            ferror(stream)) {
            fclose(stream);
            return 0;
        }
        ++attempts;
        if (attempts == 100000UL) {
            fclose(stream);
            return 0;
        }
    }
    return fclose(stream) == 0;
}

static int timer_test(void)
{
    unsigned int first = clock_monotonic_ms();
    unsigned int second = clock_monotonic_ms();

    if ((unsigned int)(second - first) > 0x7FFFFFFFU) {
        return 0;
    }
    if (!wait_with_keyboard_poll() || !timer_during_file_reads()) {
        return 0;
    }
    if (get_random(NULL, 0U) != 0) {
        return 0;
    }
    puts("PLATFORM TIMER TEST: PASS");
    return 1;
}

static int random_positive_test(void)
{
    unsigned char bytes[RANDOM_REQUEST_MAX];
    unsigned int index;
    int any_nonzero = 0;

    memset(bytes, 0, sizeof(bytes));
    if (get_random(bytes, sizeof(bytes)) != (int)sizeof(bytes)) {
        return 0;
    }
    for (index = 0U; index < sizeof(bytes); ++index) {
        if (bytes[index] != 0U) {
            any_nonzero = 1;
        }
    }
    if (!any_nonzero || get_random(NULL, 1U) != SYS_ERR_INVALID) {
        return 0;
    }
    memset(bytes, 0xA5, sizeof(bytes));
    if (get_random(bytes, RANDOM_REQUEST_MAX + 1U) != SYS_ERR_RANGE ||
        bytes[0] != 0xA5U) {
        return 0;
    }
    puts("RANDOM TEST: PASS");
    return 1;
}

static int random_unavailable_test(void)
{
    unsigned char bytes[RANDOM_REQUEST_MAX];
    unsigned int index;

    memset(bytes, 0xA5, sizeof(bytes));
    if (get_random(bytes, sizeof(bytes)) != SYS_ERR_UNAVAILABLE) {
        return 0;
    }
    for (index = 0U; index < sizeof(bytes); ++index) {
        if (bytes[index] != 0U) {
            return 0;
        }
    }
    puts("RANDOM UNAVAILABLE TEST: PASS");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        return timer_test() ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "random") == 0) {
        return random_positive_test() ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "unavailable") == 0) {
        return random_unavailable_test() ? 0 : 1;
    }
    return 1;
}
