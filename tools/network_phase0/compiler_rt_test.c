#include <stdint.h>
#include <stdio.h>

uint64_t __udivdi3(uint64_t numerator, uint64_t denominator);
uint64_t __umoddi3(uint64_t numerator, uint64_t denominator);

static int check(uint64_t numerator, uint64_t denominator)
{
    uint64_t expected_quotient;
    uint64_t expected_remainder;

    expected_quotient = numerator / denominator;
    expected_remainder = numerator % denominator;
    if (__udivdi3(numerator, denominator) != expected_quotient ||
        __umoddi3(numerator, denominator) != expected_remainder) {
        fprintf(stderr, "64-bit division mismatch\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    uint64_t numerator;
    uint64_t denominator;
    unsigned int iteration;

    if (check(0, 1) || check(1, 1) || check(UINT64_MAX, 1) ||
        check(UINT64_MAX, UINT64_MAX) || check(UINT64_MAX, 3) ||
        check(UINT64_C(0x8000000000000000), UINT64_C(0x100000001))) {
        return 1;
    }
    numerator = UINT64_C(0x9e3779b97f4a7c15);
    denominator = UINT64_C(0xd1b54a32d192ed03);
    for (iteration = 0; iteration < 10000; ++iteration) {
        numerator = numerator * UINT64_C(6364136223846793005) + 1;
        denominator = denominator * UINT64_C(1442695040888963407) + 1;
        denominator |= 1;
        if (check(numerator, denominator)) {
            return 1;
        }
    }
    return 0;
}
