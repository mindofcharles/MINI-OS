[bits 32]

PIT_COMMAND_PORT equ 0x43
PIT_CHANNEL0_PORT equ 0x40
PIT_INPUT_HZ equ 1193182
PIT_TICK_HZ equ 1000
PIT_DIVISOR equ (PIT_INPUT_HZ + (PIT_TICK_HZ / 2)) / PIT_TICK_HZ

%if PIT_DIVISOR < 1 || PIT_DIVISOR > 65535
    %error "PIT divisor must fit channel 0"
%endif

align 4, db 0
monotonic_ms dd 0

pit_initialize:
    push eax
    push edx

    mov dword [monotonic_ms], 0

    ; Channel 0, low byte then high byte, rate-generator mode, binary count.
    mov dx, PIT_COMMAND_PORT
    mov al, 0x34
    out dx, al

    mov dx, PIT_CHANNEL0_PORT
    mov ax, PIT_DIVISOR
    out dx, al
    mov al, ah
    out dx, al

    pop edx
    pop eax
    ret
