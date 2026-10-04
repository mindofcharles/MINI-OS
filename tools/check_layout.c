#include <stdint.h>
#include <stdio.h>

#define PLATFORM_LAYOUT_CONST(name, value) enum { name = value };
#include "../OS_src/kernel/platform_layout.def"
#undef PLATFORM_LAYOUT_CONST

#define NET_RAW_CONST(name, value) enum { name = value };
#include "../transport/lib/net/raw.def"
#undef NET_RAW_CONST

typedef struct {
    const char *name;
    uint32_t begin;
    uint32_t end;
    int requires_ram;
} MemoryRegion;

static int check_range(const MemoryRegion *region) {
    if (region->begin >= region->end) {
        fprintf(stderr, "layout: %s has an empty or reversed range\n", region->name);
        return -1;
    }
    if (region->end > PLATFORM_CONFIGURED_MEMORY_BYTES) {
        fprintf(stderr, "layout: %s exceeds configured guest memory\n", region->name);
        return -1;
    }
    if (!region->requires_ram) return 0;
    if (region->end <= LEGACY_MEMORY_BASE) {
        if (region->end > PLATFORM_REQUIRED_CONVENTIONAL_END) {
            fprintf(stderr, "layout: %s exceeds firmware-checked conventional memory\n",
                    region->name);
            return -1;
        }
        return 0;
    }
    if (region->begin >= LEGACY_MEMORY_END) {
        if (region->end > PLATFORM_REQUIRED_EXTENDED_END) {
            fprintf(stderr, "layout: %s exceeds firmware-checked extended memory\n",
                    region->name);
            return -1;
        }
        return 0;
    }
    fprintf(stderr, "layout: %s crosses the legacy-memory hole\n", region->name);
    return -1;
}

static int ranges_overlap(const MemoryRegion *left, const MemoryRegion *right) {
    return left->begin < right->end && right->begin < left->end;
}

static int check_equal(uint32_t actual, uint32_t expected, const char *name) {
    if (actual != expected) {
        fprintf(stderr, "layout: %s is 0x%08X, expected 0x%08X\n",
                name, actual, expected);
        return -1;
    }
    return 0;
}

static int check_inside(uint32_t begin, uint32_t end,
                        uint32_t outer_begin, uint32_t outer_end,
                        const char *name) {
    if (begin < outer_begin || end < begin || end > outer_end) {
        fprintf(stderr, "layout: %s lies outside its containing region\n", name);
        return -1;
    }
    return 0;
}

static int check_alignment(uint32_t value, uint32_t alignment,
                           const char *name) {
    if (alignment == 0U || value % alignment != 0U) {
        fprintf(stderr, "layout: %s is not %u-byte aligned\n", name, alignment);
        return -1;
    }
    return 0;
}

int main(void) {
    static const MemoryRegion regions[] = {
        { "boot stack", BOOT_STACK_BASE, BOOT_STACK_TOP, 1 },
        { "boot image", BOOT_IMAGE_BASE, BOOT_IMAGE_END, 1 },
        { "kernel image reservation", KERNEL_IMAGE_BASE, KERNEL_IMAGE_END, 1 },
        { "superblock buffer", BUF_SUPERBLOCK,
          BUF_SUPERBLOCK + KERNEL_BUFFER_SIZE, 1 },
        { "bitmap buffer", BUF_BITMAP, BUF_BITMAP + KERNEL_BUFFER_SIZE, 1 },
        { "sector buffer", BUF_SECTOR, BUF_SECTOR + KERNEL_BUFFER_SIZE, 1 },
        { "text buffer", BUF_TEXT, BUF_TEXT + KERNEL_BUFFER_SIZE, 1 },
        { "inode buffer", BUF_INODE, BUF_INODE + KERNEL_BUFFER_SIZE, 1 },
        { "command buffer", BUF_CMD, BUF_CMD + KERNEL_BUFFER_SIZE, 1 },
        { "IDT", IDT_BASE, IDT_BASE + IDT_SIZE, 1 },
        { "network receive buffer", NET_RX_BUFFER_BASE,
          NET_RX_BUFFER_BASE + NET_FRAME_BUFFER_SIZE, 1 },
        { "network transmit buffer", NET_TX_BUFFER_BASE,
          NET_TX_BUFFER_BASE + NET_FRAME_BUFFER_SIZE, 1 },
        { "kernel stack canary", KERNEL_STACK_CANARY_BASE,
          KERNEL_STACK_CANARY_BASE + KERNEL_STACK_CANARY_SIZE, 1 },
        { "kernel stack", KERNEL_STACK_BASE, KERNEL_STACK_TOP, 1 },
        { "interrupt stack canary", INTERRUPT_STACK_CANARY_BASE,
          INTERRUPT_STACK_CANARY_BASE + INTERRUPT_STACK_CANARY_SIZE, 1 },
        { "interrupt stack", INTERRUPT_STACK_BASE, INTERRUPT_STACK_TOP, 1 },
        { "legacy memory", LEGACY_MEMORY_BASE, LEGACY_MEMORY_END, 0 },
        { "application image", APP_IMAGE_BASE, APP_IMAGE_END, 1 },
        { "application heap", APP_HEAP_BASE, APP_HEAP_END, 1 },
        { "application heap canary", APP_HEAP_CANARY_BASE,
          APP_HEAP_CANARY_BASE + APP_HEAP_CANARY_SIZE, 1 },
        { "application arguments", APP_ARG_BLOCK_BASE, APP_ARG_BLOCK_END, 1 },
        { "application stack canary", APP_STACK_CANARY_BASE,
          APP_STACK_CANARY_BASE + APP_STACK_CANARY_SIZE, 1 },
        { "application stack", APP_STACK_BASE, APP_STACK_TOP, 1 }
    };
    size_t i;
    size_t j;

    if (check_equal(BOOT_IMAGE_BASE, 0x00007C00U,
                    "BIOS boot image address") < 0 ||
        check_equal(BOOT_IMAGE_END - BOOT_IMAGE_BASE,
                    512U, "boot image size") < 0 ||
        check_equal(BOOT_STACK_TOP, BOOT_IMAGE_BASE,
                    "boot stack upper boundary") < 0 ||
        check_equal(LEGACY_MEMORY_BASE, 0x000A0000U,
                    "legacy-memory base") < 0 ||
        check_equal(LEGACY_MEMORY_END, 0x00100000U,
                    "legacy-memory end") < 0 ||
        check_equal(KERNEL_IMAGE_END - KERNEL_IMAGE_BASE,
                    KERNEL_IMAGE_MAX_SIZE, "kernel image size") < 0 ||
        check_equal(IDT_SIZE, 256U * 8U, "IDT size") < 0 ||
        check_equal(APP_IMAGE_END - APP_IMAGE_BASE,
                    APP_IMAGE_SIZE, "application image size") < 0 ||
        check_equal(APP_HEAP_END - APP_HEAP_BASE,
                    APP_HEAP_SIZE, "application heap size") < 0 ||
        check_equal(APP_ARG_BLOCK_END - APP_ARG_BLOCK_BASE,
                    APP_ARG_BLOCK_SIZE, "application argument size") < 0 ||
        check_equal(APP_STACK_TOP - APP_STACK_BASE,
                    APP_STACK_SIZE, "application stack size") < 0 ||
        check_equal(A20_TEST_HIGH_ADDR - A20_TEST_LOW_ADDR,
                    0x00100000U, "A20 alias distance") < 0 ||
        check_equal(PLATFORM_REQUIRED_CONVENTIONAL_END,
                    INTERRUPT_STACK_TOP,
                    "required conventional-memory end") < 0 ||
        check_equal(PLATFORM_REQUIRED_EXTENDED_END, APP_STACK_TOP,
                    "required extended-memory end") < 0) {
        return 1;
    }
    if (KERNEL_IMAGE_MAX_SIZE % 512U != 0U ||
        APP_IMAGE_SIZE % 512U != 0U) {
        fprintf(stderr, "layout: disk-loaded regions must use whole sectors\n");
        return 1;
    }
    if (KERNEL_IMAGE_MAX_SIZE / 512U > 255U) {
        fprintf(stderr, "layout: kernel reservation exceeds boot-sector counter\n");
        return 1;
    }
    if (check_alignment(PLATFORM_CONFIGURED_MEMORY_BYTES,
                        1024U * 1024U, "configured guest memory") < 0 ||
        check_alignment(PLATFORM_REQUIRED_CONVENTIONAL_END, 1024U,
                        "required conventional-memory end") < 0 ||
        check_alignment(PLATFORM_REQUIRED_EXTENDED_END, 1024U,
                        "required extended-memory end") < 0) {
        return 1;
    }
    if (PLATFORM_REQUIRED_CONVENTIONAL_END > LEGACY_MEMORY_BASE ||
        PLATFORM_REQUIRED_EXTENDED_END <= LEGACY_MEMORY_END ||
        PLATFORM_REQUIRED_EXTENDED_END > PLATFORM_CONFIGURED_MEMORY_BYTES ||
        (PLATFORM_REQUIRED_EXTENDED_END - LEGACY_MEMORY_END) / 1024U >
            0xFFFFU) {
        fprintf(stderr, "layout: firmware memory requirements are invalid\n");
        return 1;
    }
    if (check_alignment(BOOT_STACK_BASE, 4096U, "boot stack base") < 0 ||
        check_alignment(KERNEL_IMAGE_BASE, 4096U, "kernel image base") < 0 ||
        check_alignment(BUF_SUPERBLOCK, 4096U, "superblock buffer") < 0 ||
        check_alignment(BUF_BITMAP, 4096U, "bitmap buffer") < 0 ||
        check_alignment(BUF_SECTOR, 4096U, "sector buffer") < 0 ||
        check_alignment(BUF_TEXT, 4096U, "text buffer") < 0 ||
        check_alignment(BUF_INODE, 4096U, "inode buffer") < 0 ||
        check_alignment(BUF_CMD, 4096U, "command buffer") < 0 ||
        check_alignment(KERNEL_BUFFER_SIZE, 4096U, "kernel buffer size") < 0 ||
        check_alignment(IDT_BASE, 4096U, "IDT base") < 0 ||
        check_alignment(NET_RX_BUFFER_BASE, 4096U,
                        "network receive buffer") < 0 ||
        check_alignment(NET_TX_BUFFER_BASE, 4096U,
                        "network transmit buffer") < 0 ||
        check_alignment(NET_FRAME_BUFFER_SIZE, 4096U,
                        "network frame-buffer size") < 0 ||
        check_alignment(KERNEL_STACK_CANARY_BASE, 4096U,
                        "kernel stack canary") < 0 ||
        check_alignment(KERNEL_STACK_CANARY_SIZE, 4096U,
                        "kernel stack canary size") < 0 ||
        check_alignment(KERNEL_STACK_BASE, 4096U, "kernel stack base") < 0 ||
        check_alignment(KERNEL_STACK_TOP, 4096U, "kernel stack top") < 0 ||
        check_alignment(INTERRUPT_STACK_CANARY_BASE, 4096U,
                        "interrupt stack canary") < 0 ||
        check_alignment(INTERRUPT_STACK_CANARY_SIZE, 4096U,
                        "interrupt stack canary size") < 0 ||
        check_alignment(INTERRUPT_STACK_BASE, 4096U,
                        "interrupt stack base") < 0 ||
        check_alignment(INTERRUPT_STACK_TOP, 4096U,
                        "interrupt stack top") < 0 ||
        check_alignment(APP_IMAGE_BASE, 4096U, "application image base") < 0 ||
        check_alignment(APP_IMAGE_SIZE, 4096U, "application image size") < 0 ||
        check_alignment(APP_HEAP_BASE, 4096U, "application heap base") < 0 ||
        check_alignment(APP_HEAP_SIZE, 4096U, "application heap size") < 0 ||
        check_alignment(APP_HEAP_CANARY_BASE, 4096U,
                        "application heap canary") < 0 ||
        check_alignment(APP_HEAP_CANARY_SIZE, 4096U,
                        "application heap canary size") < 0 ||
        check_alignment(APP_ARG_BLOCK_BASE, 4096U,
                        "application argument block") < 0 ||
        check_alignment(APP_ARG_BLOCK_SIZE, 4096U,
                        "application argument-block size") < 0 ||
        check_alignment(APP_STACK_CANARY_BASE, 4096U,
                        "application stack canary") < 0 ||
        check_alignment(APP_STACK_CANARY_SIZE, 4096U,
                        "application stack canary size") < 0 ||
        check_alignment(APP_STACK_BASE, 4096U, "application stack base") < 0 ||
        check_alignment(APP_STACK_SIZE, 4096U, "application stack size") < 0 ||
        check_alignment(APP_STACK_TOP, 4096U, "application stack top") < 0 ||
        check_alignment(APP_ARGV_ADDR, 4U, "application argv array") < 0) {
        return 1;
    }
    if (APP_IMAGE_BASE < LEGACY_MEMORY_END) {
        fprintf(stderr, "layout: application image must begin above legacy memory\n");
        return 1;
    }
    if (A20_TEST_LOW_ADDR > 0xFFFFU ||
        A20_TEST_HIGH_ADDR < 0xFFFF0U ||
        A20_TEST_HIGH_ADDR - 0xFFFF0U > 0xFFFFU ||
        A20_TEST_LOW_ADDR >= PLATFORM_REQUIRED_CONVENTIONAL_END ||
        A20_TEST_HIGH_ADDR >= PLATFORM_REQUIRED_EXTENDED_END) {
        fprintf(stderr, "layout: A20 test addresses are not real-mode addressable\n");
        return 1;
    }
    if (check_inside(A20_TEST_HIGH_ADDR, A20_TEST_HIGH_ADDR + 1U,
                     APP_IMAGE_BASE, APP_IMAGE_END,
                     "temporary A20 high scratch byte") < 0) {
        return 1;
    }
    if (APP_ARG0_ADDR + APP_ARG0_CAP > APP_ARG1_ADDR ||
        APP_ARG1_ADDR + APP_ARG1_CAP > APP_ARGV_ADDR ||
        APP_ARGV_ADDR + 12U > APP_ARG_BLOCK_END) {
        fprintf(stderr, "layout: application argument fields overlap\n");
        return 1;
    }
    if (NET_FRAME_BUFFER_SIZE < NET_FRAME_MAX + 1U ||
        MEMORY_CANARY_VALUE == 0U ||
        MEMORY_CANARY_VALUE > 0xFFU) {
        fprintf(stderr, "layout: a frame buffer or canary constant is invalid\n");
        return 1;
    }
    if (check_inside(APP_ARG0_ADDR, APP_ARG0_ADDR + APP_ARG0_CAP,
                     APP_ARG_BLOCK_BASE, APP_ARG_BLOCK_END, "argv[0]") < 0 ||
        check_inside(APP_ARG1_ADDR, APP_ARG1_ADDR + APP_ARG1_CAP,
                     APP_ARG_BLOCK_BASE, APP_ARG_BLOCK_END, "argv[1]") < 0 ||
        check_inside(APP_ARGV_ADDR, APP_ARGV_ADDR + 12U,
                     APP_ARG_BLOCK_BASE, APP_ARG_BLOCK_END, "argv array") < 0) {
        return 1;
    }
    for (i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i) {
        if (check_range(&regions[i]) < 0) return 1;
        if (A20_TEST_LOW_ADDR >= regions[i].begin &&
            A20_TEST_LOW_ADDR < regions[i].end) {
            fprintf(stderr, "layout: A20 low scratch byte overlaps %s\n",
                    regions[i].name);
            return 1;
        }
        for (j = i + 1U; j < sizeof(regions) / sizeof(regions[0]); ++j) {
            if (ranges_overlap(&regions[i], &regions[j])) {
                fprintf(stderr, "layout: %s overlaps %s\n",
                        regions[i].name, regions[j].name);
                return 1;
            }
        }
    }
    if (KERNEL_STACK_CANARY_BASE + KERNEL_STACK_CANARY_SIZE !=
            KERNEL_STACK_BASE ||
        INTERRUPT_STACK_CANARY_BASE + INTERRUPT_STACK_CANARY_SIZE !=
            INTERRUPT_STACK_BASE ||
        APP_HEAP_CANARY_BASE != APP_HEAP_END ||
        APP_HEAP_CANARY_BASE + APP_HEAP_CANARY_SIZE != APP_ARG_BLOCK_BASE ||
        APP_ARG_BLOCK_END != APP_STACK_CANARY_BASE ||
        APP_STACK_CANARY_BASE + APP_STACK_CANARY_SIZE != APP_STACK_BASE) {
        fprintf(stderr, "layout: a canary is not adjacent to its protected region\n");
        return 1;
    }
    printf("Memory layout OK: configured=%u bytes, conventional-end=0x%08X, "
           "extended-end=0x%08X, app-image=%u bytes, heap=%u bytes, "
           "app-stack=%u bytes.\n",
           PLATFORM_CONFIGURED_MEMORY_BYTES,
           PLATFORM_REQUIRED_CONVENTIONAL_END,
           PLATFORM_REQUIRED_EXTENDED_END, APP_IMAGE_SIZE, APP_HEAP_SIZE,
           APP_STACK_SIZE);
    return 0;
}
