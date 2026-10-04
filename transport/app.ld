OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    . = APP_LINK_BASE;
    __app_image_start = .;

    .text : ALIGN(16) { *(.text .text.*) }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data : ALIGN(16) { *(.data .data.*) }
    .data.rel.ro : ALIGN(16) { *(.data.rel.ro .data.rel.ro.*) }
    .got : ALIGN(16) { *(.got .got.* .got.plt) }
    .preinit_array : ALIGN(16) { *(.preinit_array .preinit_array.*) }
    .init_array : ALIGN(16) { *(.init_array .init_array.*) }
    .fini_array : ALIGN(16) { *(.fini_array .fini_array.*) }
    .ctors : ALIGN(16) { *(.ctors .ctors.*) }
    .dtors : ALIGN(16) { *(.dtors .dtors.*) }
    .tdata : ALIGN(16) { *(.tdata .tdata.*) }
    .tbss (NOLOAD) : ALIGN(16) { *(.tbss .tbss.*) }
    .eh_frame : ALIGN(16) { *(.eh_frame .eh_frame.*) }
    .bss (NOLOAD) : ALIGN(16) { *(.bss .bss.* COMMON) }

    __app_image_end = .;
    ASSERT(__app_image_end - __app_image_start <= APP_LINK_SIZE,
           "application allocatable image exceeds APP_LINK_SIZE")
    ASSERT(SIZEOF(.preinit_array) == 0,
           "application preinitializers are not supported")
    ASSERT(SIZEOF(.init_array) == 0,
           "application constructors are not supported")
    ASSERT(SIZEOF(.fini_array) == 0,
           "application destructors are not supported")
    ASSERT(SIZEOF(.ctors) == 0,
           "legacy application constructors are not supported")
    ASSERT(SIZEOF(.dtors) == 0,
           "legacy application destructors are not supported")
    ASSERT(SIZEOF(.tdata) == 0 && SIZEOF(.tbss) == 0,
           "application thread-local storage is not supported")

    .symtab 0 : { *(.symtab) }
    .strtab 0 : { *(.strtab) }
    .shstrtab 0 : { *(.shstrtab) }
    /DISCARD/ : { *(.comment .note.GNU-stack .llvm_addrsig) }
}
