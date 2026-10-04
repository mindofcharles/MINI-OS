[bits 32]

PIC_MASTER_COMMAND equ 0x20
PIC_MASTER_DATA equ 0x21
PIC_SLAVE_COMMAND equ 0xA0
PIC_SLAVE_DATA equ 0xA1
PIC_EOI equ 0x20
PIC_MASTER_VECTOR equ 0x20
PIC_SLAVE_VECTOR equ 0x28
PIC_MASTER_MASK equ 0xFE
PIC_SLAVE_MASK equ 0xFF

%macro EXCEPTION_NO_ERROR 1
exception_stub_%1:
    push dword 0
    push dword %1
    jmp exception_common
%endmacro

%macro EXCEPTION_WITH_ERROR 1
exception_stub_%1:
    push dword %1
    jmp exception_common
%endmacro

EXCEPTION_NO_ERROR 0
EXCEPTION_NO_ERROR 1
EXCEPTION_NO_ERROR 2
EXCEPTION_NO_ERROR 3
EXCEPTION_NO_ERROR 4
EXCEPTION_NO_ERROR 5
EXCEPTION_NO_ERROR 6
EXCEPTION_NO_ERROR 7
EXCEPTION_WITH_ERROR 8
EXCEPTION_NO_ERROR 9
EXCEPTION_WITH_ERROR 10
EXCEPTION_WITH_ERROR 11
EXCEPTION_WITH_ERROR 12
EXCEPTION_WITH_ERROR 13
EXCEPTION_WITH_ERROR 14
EXCEPTION_NO_ERROR 15
EXCEPTION_NO_ERROR 16
EXCEPTION_WITH_ERROR 17
EXCEPTION_NO_ERROR 18
EXCEPTION_NO_ERROR 19
EXCEPTION_NO_ERROR 20
EXCEPTION_WITH_ERROR 21
EXCEPTION_NO_ERROR 22
EXCEPTION_NO_ERROR 23
EXCEPTION_NO_ERROR 24
EXCEPTION_NO_ERROR 25
EXCEPTION_NO_ERROR 26
EXCEPTION_NO_ERROR 27
EXCEPTION_NO_ERROR 28
EXCEPTION_WITH_ERROR 29
EXCEPTION_WITH_ERROR 30
EXCEPTION_NO_ERROR 31

unexpected_vector_stub:
    push dword 0
    push dword 0xFFFFFFFF
    jmp exception_common

exception_common:
    cli
    cld
    mov eax, [ss:esp]
    mov edx, [ss:esp + 4]
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    mov [exception_vector], eax
    mov [exception_error], edx

    ; Fatal handling never returns, so it can discard a damaged application,
    ; kernel, or IRQ frame and use the known dedicated stack directly.
    mov esp, INTERRUPT_STACK_TOP
    mov esi, msg_exception_fatal_prefix
    call vga_print
    mov eax, [exception_vector]
    call interrupt_print_hex32
    mov esi, msg_exception_fatal_error
    call vga_print
    mov eax, [exception_error]
    call interrupt_print_hex32
    mov esi, msg_exception_fatal_suffix
    call vga_print
    jmp interrupt_fatal_halt

%macro IRQ_STUB 1
irq_stub_%1:
    push dword %1
    jmp irq_common
%endmacro

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

; The interrupt gate has already cleared IF. Save the complete interrupted
; context on its original stack, then run the bounded handler on the dedicated
; stack without ever enabling maskable nesting.
irq_common:
    cld
    push ds
    push es
    push fs
    push gs
    pushad

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    cmp byte [irq_active], 0
    jne interrupt_nested_fatal
    mov byte [irq_active], 1
    mov [irq_saved_context_esp], esp

    mov eax, [esp + 48]
    mov esp, INTERRUPT_STACK_TOP
    mov byte [interrupt_stack_observed], 1
    call irq_dispatch
    cmp esp, INTERRUPT_STACK_TOP
    jne interrupt_stack_fatal

    mov byte [irq_active], 0
    mov esp, [irq_saved_context_esp]
    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 4
    iretd

irq_dispatch:
    cmp eax, 15
    ja interrupt_stack_fatal
    inc dword [irq_counts + eax * 4]
    test eax, eax
    jnz .unexpected
    inc dword [monotonic_ms]
    jmp pic_send_eoi
.unexpected:
    inc dword [unexpected_irq_count]
    jmp pic_send_eoi

; IN: EAX=IRQ number 0..15. This is the only kernel EOI path.
pic_send_eoi:
    cmp eax, 8
    jb .master
    mov dx, PIC_SLAVE_COMMAND
    mov al, PIC_EOI
    out dx, al
    inc dword [pic_slave_eoi_count]
.master:
    mov dx, PIC_MASTER_COMMAND
    mov al, PIC_EOI
    out dx, al
    inc dword [pic_master_eoi_count]
    ret

pic_initialize:
    push eax
    push edx

    mov dx, PIC_MASTER_DATA
    mov al, 0xFF
    out dx, al
    mov dx, PIC_SLAVE_DATA
    out dx, al

    mov dx, PIC_MASTER_COMMAND
    mov al, 0x11
    out dx, al
    call pic_io_wait
    mov dx, PIC_SLAVE_COMMAND
    out dx, al
    call pic_io_wait

    mov dx, PIC_MASTER_DATA
    mov al, PIC_MASTER_VECTOR
    out dx, al
    call pic_io_wait
    mov dx, PIC_SLAVE_DATA
    mov al, PIC_SLAVE_VECTOR
    out dx, al
    call pic_io_wait

    mov dx, PIC_MASTER_DATA
    mov al, 0x04
    out dx, al
    call pic_io_wait
    mov dx, PIC_SLAVE_DATA
    mov al, 0x02
    out dx, al
    call pic_io_wait

    mov dx, PIC_MASTER_DATA
    mov al, 0x01
    out dx, al
    call pic_io_wait
    mov dx, PIC_SLAVE_DATA
    out dx, al
    call pic_io_wait

    ; Phase B keeps every line masked except the PIT on master IRQ0.
    mov dx, PIC_MASTER_DATA
    mov al, PIC_MASTER_MASK
    out dx, al
    mov dx, PIC_SLAVE_DATA
    mov al, PIC_SLAVE_MASK
    out dx, al

    pop edx
    pop eax
    ret

pic_io_wait:
    push eax
    push edx
    xor al, al
    mov dx, 0x80
    out dx, al
    pop edx
    pop eax
    ret

; Exercise the same common path used by real IRQs while IF is still clear.
; Software INT ignores PIC masks, so IRQ1 and IRQ8 cover masked master/slave
; paths without depending on external hardware timing.
interrupts_initialize:
    pushad
    call pic_initialize

    mov byte [interrupt_self_test_result], 0
    mov byte [interrupt_stack_observed], 0
    mov dword [monotonic_ms], 0
    mov dword [pic_master_eoi_count], 0
    mov dword [pic_slave_eoi_count], 0
    mov dword [unexpected_irq_count], 0
    mov edi, irq_counts
    mov ecx, 16
    xor eax, eax
    rep stosd

    ; Prove that the common entry restores the complete software-visible
    ; context, including null auxiliary segment selectors and DF. The IRQ
    ; handler itself must still run with the flat data selector and DF clear.
    push dword 0x51A7C0DE
    xor eax, eax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov eax, 0x10203040
    mov ebx, 0x11223344
    mov ecx, 0x55667788
    mov edx, 0x99AABBCC
    mov ebp, 0x13579BDF
    mov esi, 0x2468ACE0
    mov edi, 0x0BADF00D
    std
    int 0x20
    cmp eax, 0x10203040
    jne .context_failed
    cmp ebx, 0x11223344
    jne .context_failed
    cmp ecx, 0x55667788
    jne .context_failed
    cmp edx, 0x99AABBCC
    jne .context_failed
    cmp ebp, 0x13579BDF
    jne .context_failed
    cmp esi, 0x2468ACE0
    jne .context_failed
    cmp edi, 0x0BADF00D
    jne .context_failed
    pushfd
    pop eax
    test eax, 1 << 10
    jz .context_failed
    mov ax, ds
    cmp ax, 0x10
    jne .context_failed
    mov ax, es
    test ax, ax
    jnz .context_failed
    mov ax, fs
    test ax, ax
    jnz .context_failed
    mov ax, gs
    test ax, ax
    jnz .context_failed
    mov ax, 0x10
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    cmp dword [esp], 0x51A7C0DE
    jne .stack_failed
    add esp, 4

    push dword 0x51A7C0DE
    int 0x21
    cmp dword [esp], 0x51A7C0DE
    jne .stack_failed
    add esp, 4

    push dword 0x51A7C0DE
    int 0x28
    cmp dword [esp], 0x51A7C0DE
    jne .stack_failed
    add esp, 4

    cmp dword [monotonic_ms], 1
    jne .failed
    cmp dword [pic_master_eoi_count], 3
    jne .failed
    cmp dword [pic_slave_eoi_count], 1
    jne .failed
    cmp dword [unexpected_irq_count], 2
    jne .failed
    cmp dword [irq_counts], 1
    jne .failed
    cmp dword [irq_counts + 4], 1
    jne .failed
    cmp dword [irq_counts + 32], 1
    jne .failed
    cmp byte [irq_active], 0
    jne .failed
    cmp byte [interrupt_stack_observed], 1
    jne .failed
    call platform_verify_guards
    test eax, eax
    jz .failed

    call pit_initialize
    mov dword [pic_master_eoi_count], 0
    mov dword [pic_slave_eoi_count], 0
    mov dword [unexpected_irq_count], 0
    mov edi, irq_counts
    mov ecx, 16
    xor eax, eax
    rep stosd
    mov byte [interrupt_self_test_result], 1
    jmp .done

.context_failed:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
.stack_failed:
    add esp, 4
.failed:
    mov byte [interrupt_self_test_result], 0
.done:
    popad
    movzx eax, byte [interrupt_self_test_result]
    ret

interrupt_nested_fatal:
    mov esp, INTERRUPT_STACK_TOP
    mov esi, msg_interrupt_nested_fatal
    call vga_print
    jmp interrupt_fatal_halt

interrupt_stack_fatal:
    mov esp, INTERRUPT_STACK_TOP
    mov esi, msg_interrupt_stack_fatal
    call vga_print

interrupt_fatal_halt:
    cli
    hlt
    jmp interrupt_fatal_halt

; IN: EAX=value
interrupt_print_hex32:
    push eax
    push ebx
    push ecx
    push edx
    mov ebx, eax
    mov ecx, 8
.digit:
    rol ebx, 4
    mov edx, ebx
    and edx, 0x0F
    mov al, [interrupt_hex_digits + edx]
    call vga_putc
    loop .digit
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

align 4, db 0
exception_vector dd 0
exception_error dd 0
irq_saved_context_esp dd 0
pic_master_eoi_count dd 0
pic_slave_eoi_count dd 0
unexpected_irq_count dd 0
irq_counts times 16 dd 0
irq_active db 0
interrupt_stack_observed db 0
interrupt_self_test_result db 0

interrupt_hex_digits db "0123456789ABCDEF"
msg_exception_fatal_prefix db "MINI_OS: fatal exception vector=0x", 0
msg_exception_fatal_error db " error=0x", 0
msg_exception_fatal_suffix db "; system halted.", 10, 0
msg_interrupt_nested_fatal db "MINI_OS: nested hardware interrupt; system halted.", 10, 0
msg_interrupt_stack_fatal db "MINI_OS: interrupt stack invariant failed; system halted.", 10, 0

align 4, db 0
exception_stub_table:
%assign exception_index 0
%rep 32
    dd exception_stub_%[exception_index]
%assign exception_index exception_index + 1
%endrep

irq_stub_table:
%assign irq_index 0
%rep 16
    dd irq_stub_%[irq_index]
%assign irq_index irq_index + 1
%endrep
