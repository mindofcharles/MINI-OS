#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

const unsigned char mini_os_phase0_test_entropy_marker[] =
    "MINI_OS_PHASE0_TEST_ENTROPY_ONLY";

int mini_os_phase0_wolf_seed(unsigned char *output, unsigned int size)
{
    unsigned int index;

    for (index = 0; index < size; ++index) {
        output[index] = (unsigned char)(0x5AU ^ index);
    }
    return 0;
}

int fprintf(FILE *stream, const char *format, ...)
{
    (void)stream;
    (void)format;
    return -1;
}

int fputs(const char *string, FILE *stream)
{
    (void)string;
    (void)stream;
    return -1;
}

char *strtok_r(char *string, const char *delimiters, char **save_pointer)
{
    char *token;
    const char *delimiter;
    int matched;

    if (string == NULL) {
        string = *save_pointer;
    }
    while (*string != '\0') {
        matched = 0;
        for (delimiter = delimiters; *delimiter != '\0'; ++delimiter) {
            if (*string == *delimiter) {
                matched = 1;
                break;
            }
        }
        if (!matched) {
            break;
        }
        ++string;
    }
    if (*string == '\0') {
        *save_pointer = string;
        return NULL;
    }
    token = string;
    while (*string != '\0') {
        for (delimiter = delimiters; *delimiter != '\0'; ++delimiter) {
            if (*string == *delimiter) {
                *string = '\0';
                *save_pointer = string + 1;
                return token;
            }
        }
        ++string;
    }
    *save_pointer = string;
    return token;
}

int strncasecmp(const char *left, const char *right, size_t count)
{
    while (count != 0) {
        unsigned char left_value;
        unsigned char right_value;

        left_value = (unsigned char)*left++;
        right_value = (unsigned char)*right++;
        if (left_value >= 'A' && left_value <= 'Z') {
            left_value = (unsigned char)(left_value + ('a' - 'A'));
        }
        if (right_value >= 'A' && right_value <= 'Z') {
            right_value = (unsigned char)(right_value + ('a' - 'A'));
        }
        if (left_value != right_value) {
            return (int)left_value - (int)right_value;
        }
        if (left_value == '\0') {
            return 0;
        }
        --count;
    }
    return 0;
}

time_t time(time_t *result)
{
    static time_t now;
    time_t value;

    value = now++;
    if (result != NULL) {
        *result = value;
    }
    return value;
}
