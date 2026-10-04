#include <stdio.h>

#define STACK_PROBE_SIZE (28U * 1024U)

static int exercise_application_stack(void)
{
    volatile unsigned char probe[STACK_PROBE_SIZE];
    unsigned int index;

    for (index = 0; index < STACK_PROBE_SIZE; ++index) {
        probe[index] = (unsigned char)(index ^ 0x5AU);
    }
    for (index = 0; index < STACK_PROBE_SIZE; ++index) {
        if (probe[index] != (unsigned char)(index ^ 0x5AU)) {
            puts("STACK CANARY TEST: FAIL");
            return 1;
        }
    }
    puts("STACK CANARY TEST: PASS");
    return 0;
}

int main(void)
{
    return exercise_application_stack();
}
