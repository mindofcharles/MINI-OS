#define PLATFORM_LAYOUT_CONST(name, value) enum { name = value };
#include "../../OS_src/kernel/platform_layout.def"
#undef PLATFORM_LAYOUT_CONST

static int strings_equal(const char *left, const char *right)
{
    while (*left == *right) {
        if (*left == '\0') {
            return 1;
        }
        ++left;
        ++right;
    }
    return 0;
}

int main(int argc, char **argv)
{
    volatile unsigned char *guard;
    unsigned long address;

    if (argc != 2) {
        return 1;
    }
    if (strings_equal(argv[1], "kernel")) {
        address = KERNEL_STACK_CANARY_BASE;
    }
    else if (strings_equal(argv[1], "interrupt")) {
        address = INTERRUPT_STACK_CANARY_BASE;
    }
    else if (strings_equal(argv[1], "heap")) {
        address = APP_HEAP_CANARY_BASE;
    }
    else if (strings_equal(argv[1], "application")) {
        address = APP_STACK_CANARY_BASE;
    }
    else {
        return 1;
    }

    guard = (volatile unsigned char *)address;
    *guard = 0;
    return 0;
}
