typedef unsigned long long mini_uint64_t;

static mini_uint64_t divide_unsigned_64(mini_uint64_t numerator,
                                        mini_uint64_t denominator,
                                        mini_uint64_t *remainder)
{
    mini_uint64_t quotient;
    mini_uint64_t current;
    unsigned int bit;

    if (denominator == 0) {
        __builtin_trap();
    }
    quotient = 0;
    current = 0;
    bit = 64;
    while (bit != 0) {
        unsigned int carry;

        --bit;
        carry = (unsigned int)(current >> 63);
        current = (current << 1) | ((numerator >> bit) & 1U);
        if (carry != 0 || current >= denominator) {
            current -= denominator;
            quotient |= (mini_uint64_t)1U << bit;
        }
    }
    if (remainder != 0) {
        *remainder = current;
    }
    return quotient;
}

mini_uint64_t __udivdi3(mini_uint64_t numerator, mini_uint64_t denominator)
{
    return divide_unsigned_64(numerator, denominator, 0);
}

mini_uint64_t __umoddi3(mini_uint64_t numerator, mini_uint64_t denominator)
{
    mini_uint64_t remainder;

    (void)divide_unsigned_64(numerator, denominator, &remainder);
    return remainder;
}
