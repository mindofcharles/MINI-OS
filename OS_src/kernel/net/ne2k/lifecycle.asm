; Initialize all software-visible state, then probe and configure the fixed
; device. Absence is nonfatal to boot and remains queryable through get-info.
ne2k_initialize:
    pushad
    mov edi, ne2k_state
    mov ecx, ne2k_state_end - ne2k_state
    call zero_buffer
    mov dword [ne2k_state], NET_DEVICE_UNAVAILABLE
    call ne2k_configure
    jnc .done
    mov dword [ne2k_state], NET_DEVICE_UNAVAILABLE
.done:
    popad
    xor eax, eax
    cmp dword [ne2k_state], NET_DEVICE_READY
    sete al
    ret

; OUT: CF clear when reset, PROM identification, and ring programming succeed.
ne2k_configure:
    call ne2k_reset
    jc .failed
    call ne2k_read_prom
    jc .failed
    call ne2k_program
    jc .failed
    mov dword [ne2k_state], NET_DEVICE_READY
    clc
    ret
.failed:
    call ne2k_quiesce
    stc
    ret

; Leave a failed or unsupported device stopped with all NIC interrupts masked.
; This is safe even when the fixed I/O address does not decode a device.
ne2k_quiesce:
    push eax
    push edx
    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_STOP
    out dx, al
    mov dx, NE2K_REG_IMR
    xor al, al
    out dx, al
    pop edx
    pop eax
    ret

; A reset attempt is counted even when the addressed device is absent.
ne2k_reset:
    push eax
    push ebx
    push ecx
    push edx
    inc dword [ne2k_resets]

%ifdef KERNEL_TEST_NET_RESET_TIMEOUT_ONCE
    cmp byte [ne2k_test_reset_timeout_once], 0
    je .hardware_reset
    cmp dword [ne2k_state], NET_DEVICE_READY
    jne .hardware_reset
    mov byte [ne2k_test_reset_timeout_once], 0
    inc dword [ne2k_reset_timeouts]
    stc
    jmp .done
.hardware_reset:
%endif

    mov dx, NE2K_RESET_PORT
    in al, dx
    out dx, al

    mov bl, NE2K_ISR_RST
    mov ecx, NE2K_RESET_TIMEOUT_MS
    call ne2k_wait_isr
    jc .failed

    mov dx, NE2K_REG_ISR
    mov al, NE2K_ISR_RST
    out dx, al
    clc
    jmp .done
.failed:
    inc dword [ne2k_reset_timeouts]
    stc
.done:
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; IN: BL=one or more ISR bits, ECX=timeout in milliseconds.
; OUT: CF clear once any requested bit appears, CF set on time/iteration bound.
ne2k_wait_isr:
    push eax
    push ecx
    push edx
    push esi
    push edi
    push ebp
    mov edi, ecx
    mov esi, [monotonic_ms]
    mov ebp, NE2K_WAIT_ITERATIONS
.poll:
    mov dx, NE2K_REG_ISR
    in al, dx
    test al, bl
    jnz .ready
    mov eax, [monotonic_ms]
    sub eax, esi
    cmp eax, edi
    jae .timeout
    dec ebp
    jnz .poll
.timeout:
    stc
    jmp .done
.ready:
    clc
.done:
    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop eax
    ret

; Read the 32-byte NE2000 PROM in word-wide mode. The board exposes each
; logical PROM byte twice, so the MAC occupies even offsets 0 through 10 and
; the 0x57 identification bytes occupy even offsets 28 and 30.
ne2k_read_prom:
    pushad
    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_STOP
    out dx, al
    mov dx, NE2K_REG_DCR
    mov al, NE2K_DCR_WORD_WIDE
    out dx, al
    mov dx, NE2K_REG_RBCR0
    xor al, al
    out dx, al
    inc dx
    out dx, al
    mov dx, NE2K_REG_RCR
    mov al, NE2K_RCR_MONITOR
    out dx, al
    mov dx, NE2K_REG_TCR
    mov al, NE2K_TCR_LOOPBACK
    out dx, al
    mov dx, NE2K_REG_IMR
    xor al, al
    out dx, al

    xor eax, eax
    mov edi, NET_RX_BUFFER_BASE
    mov ecx, NE2K_PROM_BYTES
    call ne2k_dma_read
    jnc .prom_ready
    inc dword [ne2k_dma_timeouts]
    jmp .failed
.prom_ready:

    cmp byte [NET_RX_BUFFER_BASE + 28], 0x57
    jne .failed
    cmp byte [NET_RX_BUFFER_BASE + 30], 0x57
    jne .failed

    xor eax, eax
    mov ebx, 0xFF
    xor ecx, ecx
.mac_loop:
    mov dl, [NET_RX_BUFFER_BASE + ecx * 2]
    mov [ne2k_mac + ecx], dl
    or al, dl
    and bl, dl
    inc ecx
    cmp ecx, 6
    jb .mac_loop
    test al, al
    jz .failed
    cmp bl, 0xFF
    je .failed
    test byte [ne2k_mac], 1
    jnz .failed
    clc
    jmp .done
.failed:
    stc
.done:
    popad
    ret

; Program a six-page transmit area and the remaining standard 16 KiB packet
; RAM as a receive ring, then read back the page-1 station address and CURR.
ne2k_program:
    pushad
    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_STOP
    out dx, al
    mov dx, NE2K_REG_DCR
    mov al, NE2K_DCR_WORD_WIDE
    out dx, al
    mov dx, NE2K_REG_RBCR0
    xor al, al
    out dx, al
    inc dx
    out dx, al
    mov dx, NE2K_REG_RCR
    mov al, NE2K_RCR_MONITOR
    out dx, al
    mov dx, NE2K_REG_TCR
    mov al, NE2K_TCR_LOOPBACK
    out dx, al
    mov dx, NE2K_REG_TPSR
    mov al, NE2K_TX_START_PAGE
    out dx, al
    mov dx, NE2K_REG_PSTART
    mov al, NE2K_RX_START_PAGE
    out dx, al
    mov dx, NE2K_REG_PSTOP
    mov al, NE2K_RX_STOP_PAGE
    out dx, al
    mov dx, NE2K_REG_BNRY
    mov al, NE2K_RX_START_PAGE
    out dx, al
    mov dx, NE2K_REG_ISR
    mov al, 0xFF
    out dx, al
    mov dx, NE2K_REG_IMR
    xor al, al
    out dx, al

    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_PAGE1_STOP
    out dx, al
    mov dx, NE2K_PAGE1_PAR0
    mov esi, ne2k_mac
    mov ecx, 6
.write_mac:
    lodsb
    out dx, al
    inc dx
    loop .write_mac
    mov dx, NE2K_PAGE1_CURR
    mov al, NE2K_RX_FIRST_PAGE
    out dx, al
    mov dx, NE2K_PAGE1_MAR0
    xor al, al
    mov ecx, 8
.clear_multicast:
    out dx, al
    inc dx
    loop .clear_multicast

    mov dx, NE2K_PAGE1_PAR0
    mov esi, ne2k_mac
    mov ecx, 6
.verify_mac:
    in al, dx
    cmp al, [esi]
    jne .failed
    inc esi
    inc dx
    loop .verify_mac
    mov dx, NE2K_PAGE1_CURR
    in al, dx
    cmp al, NE2K_RX_FIRST_PAGE
    jne .failed

    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_START
    out dx, al
    mov dx, NE2K_REG_ISR
    mov al, 0xFF
    out dx, al
    mov dx, NE2K_REG_RCR
    mov al, NE2K_RCR_BROADCAST
    out dx, al
    mov dx, NE2K_REG_TCR
    xor al, al
    out dx, al
    mov dx, NE2K_REG_IMR
    out dx, al
    clc
    jmp .done
.failed:
    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_STOP
    out dx, al
    stc
.done:
    popad
    ret

