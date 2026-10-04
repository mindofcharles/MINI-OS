[bits 16]

%define PLATFORM_LAYOUT_CONST(name, value) name equ value
%include "OS_src/kernel/platform_layout.def"
%undef PLATFORM_LAYOUT_CONST

[org BOOT_IMAGE_BASE]

; Kernel load target physical address starts at 0x8000.
; We keep offset=0 and grow segment by 0x20 per sector (512 bytes),
; so the transfer buffer never crosses a 64 KiB boundary in one request.
KERNEL_LOAD_SEG equ KERNEL_IMAGE_BASE >> 4
KERNEL_ENTRY    equ KERNEL_IMAGE_BASE

%define FS_LAYOUT_CONST(name, value) name equ value
%include "OS_src/kernel/fs/layout.def"
%undef FS_LAYOUT_CONST
%if FS_BOOT_ID_SIZE <= 0
    %error "boot image identity must not be empty"
%endif
%if FS_BOOT_ID_OFFSET + FS_BOOT_ID_SIZE > 510
    %error "boot image identity overlaps the BIOS signature"
%endif

%ifndef KERNEL_SECTORS
KERNEL_SECTORS  equ 32
%endif
%if BOOT_IMAGE_BASE != 0x00007C00 || BOOT_IMAGE_END - BOOT_IMAGE_BASE != 512
    %error "BIOS boot image range is invalid"
%endif
%if KERNEL_SECTORS <= 0 || KERNEL_SECTORS > 255
    %error "kernel sector count must fit the bootloader byte counter"
%endif
%if KERNEL_SECTORS * 512 > KERNEL_IMAGE_MAX_SIZE
    %error "kernel sector count exceeds the reserved image range"
%endif
%if KERNEL_IMAGE_BASE & 0xF
    %error "kernel image base must be paragraph aligned"
%endif
%if BOOT_STACK_TOP != BOOT_IMAGE_BASE
    %error "boot stack must end at the boot image"
%endif
%if KERNEL_IMAGE_END - KERNEL_IMAGE_BASE != KERNEL_IMAGE_MAX_SIZE
    %error "kernel image reservation is inconsistent"
%endif
%if KERNEL_IMAGE_MAX_SIZE & 0x1FF
    %error "kernel image reservation must use whole sectors"
%endif
%if KERNEL_IMAGE_BASE < BOOT_IMAGE_END
    %error "kernel image overlaps the boot image"
%endif
%if KERNEL_IMAGE_END > PLATFORM_REQUIRED_CONVENTIONAL_END
    %error "kernel image exceeds required conventional memory"
%endif
%if LEGACY_MEMORY_BASE != 0x000A0000 || LEGACY_MEMORY_END != 0x00100000
    %error "legacy x86 memory boundaries are invalid"
%endif
%if A20_TEST_HIGH_ADDR - A20_TEST_LOW_ADDR != 0x00100000
    %error "A20 test addresses must be exactly one MiB apart"
%endif
%if A20_TEST_LOW_ADDR > 0xFFFF
    %error "A20 low test address is not representable as a real-mode offset"
%endif
%if A20_TEST_HIGH_ADDR < 0xFFFF0
    %error "A20 high test address is below the selected real-mode segment"
%endif
%if A20_TEST_HIGH_ADDR - 0xFFFF0 > 0xFFFF
    %error "A20 high test address is not representable in real mode"
%endif
%if PLATFORM_REQUIRED_CONVENTIONAL_END & 0x3FF
    %error "required conventional memory must use the BIOS KiB granularity"
%endif
%if PLATFORM_REQUIRED_CONVENTIONAL_END > LEGACY_MEMORY_BASE
    %error "required conventional memory extends into legacy memory"
%endif
%if PLATFORM_REQUIRED_EXTENDED_END <= LEGACY_MEMORY_END
    %error "required extended memory must end above one MiB"
%endif
%if PLATFORM_REQUIRED_EXTENDED_END > PLATFORM_CONFIGURED_MEMORY_BYTES
    %error "required extended memory exceeds configured guest memory"
%endif
%if PLATFORM_CONFIGURED_MEMORY_BYTES & 0xFFFFF
    %error "configured guest memory must use whole MiB units"
%endif
%if (PLATFORM_REQUIRED_EXTENDED_END - LEGACY_MEMORY_END) & 0x3FF
    %error "required extended memory must use the BIOS KiB granularity"
%endif
%if ((PLATFORM_REQUIRED_EXTENDED_END - LEGACY_MEMORY_END) >> 10) > 0xFFFF
    %error "required extended memory exceeds the BIOS AH=88h range"
%endif
%if A20_TEST_HIGH_ADDR >= PLATFORM_REQUIRED_EXTENDED_END
    %error "A20 test address lies outside required extended memory"
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, BOOT_STACK_TOP
    sti

    mov [boot_drive], dl

    ; Prove that the low kernel-owned regions and the complete high-memory
    ; application area fit in firmware-reported contiguous memory before
    ; loading the kernel or entering protected mode.
    int 0x12
    cmp ax, PLATFORM_REQUIRED_CONVENTIONAL_END >> 10
    jb memory_fail
    mov ah, 0x88
    int 0x15
    jc memory_fail
    cmp ax, (PLATFORM_REQUIRED_EXTENDED_END - LEGACY_MEMORY_END) >> 10
    jb memory_fail

    call load_kernel
    jc boot_fail

    ; Enable the fast A20 gate, then prove that addresses one MiB apart do
    ; not alias before protected-mode code can load an application there.
    cli
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    xor ax, ax
    mov ds, ax
    mov si, A20_TEST_LOW_ADDR
    mov ax, 0xFFFF
    mov es, ax
    mov di, A20_TEST_HIGH_ADDR - 0xFFFF0
    mov al, [si]
    mov ah, [es:di]
    mov byte [si], 0x00
    mov byte [es:di], 0xFF
    cmp byte [si], 0xFF
    mov [es:di], ah
    mov [si], al
%ifdef BOOT_FORCE_A20_FAIL
    jmp a20_fail
%else
    je a20_fail
%endif

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp 0x08:init_pm

boot_fail:
    mov al, 'F'
.print:
    xor bx, bx
%ifdef ENABLE_DEBUGCON
    out 0xE9, al
%else
    mov ah, 0x0E
    int 0x10
%endif
    jmp $

a20_fail:
    mov al, 'A'
    jmp boot_fail.print

memory_fail:
    mov al, 'M'
    jmp boot_fail.print

; ----------------------------
; Disk loading strategy
; ----------------------------
; 1) Probe and try INT13h extensions (AH=41h + AH=42h).
; 2) Fallback to CHS one-sector reads (AH=02h) if EDD is unavailable or fails.
; Both paths use one-sector requests and retry each read three times.
; OUT: CF clear on success, set on failure.
load_kernel:
%ifdef BOOT_FORCE_CHS
    ; Build-time verification hook for exercising the fallback in an emulator.
    jmp .chs
%else
    call detect_edd
    jc .chs

    call load_kernel_edd
    jnc .ok
%endif

.chs:
    call load_kernel_chs
    jc .fail

.ok:
    clc
    ret

.fail:
    stc
    ret

; OUT: CF clear if EDD is supported
detect_edd:
    mov ah, 0x41
    mov bx, 0x55aa
    mov dl, [boot_drive]
    int 0x13
    jc .no
    cmp bx, 0xaa55
    jne .no
    test cx, 1
    jz .no
    clc
    ret
.no:
    stc
    ret

; ----------------------------
; EDD path (AH=42h), one sector per request with retries
; OUT: CF clear on success
load_kernel_edd:
    mov word [load_seg], KERNEL_LOAD_SEG
    mov byte [load_remaining], KERNEL_SECTORS

.next_sector:
    mov byte [retry_count], 3
.retry:
    ; Some BIOS implementations report a partial count in this field.
    mov word [disk_packet + 2], 1
    mov ax, [load_seg]
    mov [disk_packet + 6], ax

    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, disk_packet
    int 0x13
    jnc .read_ok

    call disk_reset
    dec byte [retry_count]
    jnz .retry
    stc
    ret

.read_ok:
    add word [load_seg], 0x20
    inc dword [disk_packet + 8]
    dec byte [load_remaining]
    jnz .next_sector

.ok:
    clc
    ret

disk_reset:
    mov ah, 0x00
    mov dl, [boot_drive]
    int 0x13
    ret

; ----------------------------
; CHS fallback path (AH=02h), one sector per request
; OUT: CF clear on success
load_kernel_chs:
    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    jc .fail

    xor ax, ax
    mov al, cl
    and al, 0x3f
    cmp al, 0
    je .fail
    mov [spt], ax

    xor ax, ax
    mov al, dh
    inc ax
    mov [heads], ax

    mov word [load_seg], KERNEL_LOAD_SEG
    mov word [load_lba], 1
    mov byte [load_remaining], KERNEL_SECTORS

.next_sector:
    xor dx, dx
    mov ax, [load_lba]
    div word [spt]               ; AX=tmp, DX=sector-1
    inc dl                       ; sector in [1..spt]
    mov [chs_sector], dl

    xor dx, dx
    div word [heads]             ; AX=cylinder, DX=head
    cmp ax, 1023
    ja .fail
    mov [chs_cyl], ax
    mov [chs_head], dl

    mov byte [retry_count], 3
.retry:
    mov ax, [load_seg]
    mov es, ax
    xor bx, bx
    mov cl, [chs_sector]
    mov ax, [chs_cyl]
    mov ch, al                   ; cylinder bits 0..7
    shr ax, 2
    and al, 0xC0                 ; cylinder bits 8..9 -> CL[7:6]
    or cl, al
    mov dh, [chs_head]
    mov dl, [boot_drive]
    mov ah, 0x02
    mov al, 1
    int 0x13
    jnc .read_ok

    call disk_reset
    dec byte [retry_count]
    jnz .retry
    stc
    ret

.read_ok:
    add word [load_seg], 0x20
    inc word [load_lba]
    dec byte [load_remaining]
    jnz .next_sector

.ok:
    clc
    ret

.fail:
    stc
    ret

; ----------------------------
; GDT and protected-mode entry
; ----------------------------
align 8
gdt_start:
    dq 0x0
gdt_code:
    dw 0xffff
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00
gdt_data:
    dw 0xffff
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

[bits 32]
init_pm:
    mov ax, 0x10
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebp, KERNEL_STACK_TOP
    mov esp, ebp

    jmp KERNEL_ENTRY

[bits 16]
boot_drive db 0
retry_count db 0

load_seg    dw KERNEL_LOAD_SEG
load_lba    dw 1
load_remaining db 0

spt       dw 0
heads     dw 0
chs_cyl   dw 0
chs_head  db 0
chs_sector db 0

align 4
disk_packet:
    db 0x10
    db 0x00
    dw 1
    dw 0x0000
    dw KERNEL_LOAD_SEG
    dd 1
    dd 0

; The host image builder replaces this zero placeholder with a per-image ID.
; It is never modified by the boot code, so the kernel can compare the BIOS
; booted image with the protected-mode ATA target before mounting it.
times FS_BOOT_ID_OFFSET-($-$$) db 0
boot_volume_id times FS_BOOT_ID_SIZE db 0
times 510-($-$$) db 0
dw 0xaa55
