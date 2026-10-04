; IN: EAX=NIC address, EDI=destination, ECX=logical byte count.
; Word-wide transfers round both RBCR and the host access to an even count;
; every destination used here has private padding for the possible extra byte.
ne2k_dma_read:
    pushad
    mov ebp, eax
    mov ebx, ecx
    inc ebx
    and ebx, 0xFFFFFFFE
    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_START
    out dx, al
    mov dx, NE2K_REG_ISR
    mov al, NE2K_ISR_RDC
    out dx, al
    mov dx, NE2K_REG_RBCR0
    mov al, bl
    out dx, al
    inc dx
    mov al, bh
    out dx, al
    mov dx, NE2K_REG_RSAR0
    mov eax, ebp
    out dx, al
    inc dx
    mov al, ah
    out dx, al
    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_DMA_READ
    out dx, al
    mov ecx, ebx
    shr ecx, 1
    mov dx, NE2K_DATA_PORT
    rep insw
    mov bl, NE2K_ISR_RDC
    mov ecx, NE2K_DMA_TIMEOUT_MS
    call ne2k_wait_isr
    jc .failed
    mov dx, NE2K_REG_ISR
    mov al, NE2K_ISR_RDC
    out dx, al
    clc
    jmp .done
.failed:
    stc
.done:
    popad
    ret

; IN: EAX=NIC address, ESI=source, ECX=logical byte count.
ne2k_dma_write:
    pushad
    mov ebp, eax
    mov ebx, ecx
    inc ebx
    and ebx, 0xFFFFFFFE

    ; DP8390D Remote Write requires a completed dummy Remote Read to put
    ; PRQ into a known state before the write begins. Reading one word from
    ; the target packet-RAM area is safe and also works with the QEMU model.
    mov eax, ebp
    mov edi, ne2k_dma_dummy_word
    mov ecx, 2
    call ne2k_dma_read
    jc .failed

    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_START
    out dx, al
    mov dx, NE2K_REG_ISR
    mov al, NE2K_ISR_RDC
    out dx, al
    mov dx, NE2K_REG_RBCR0
    mov al, bl
    out dx, al
    inc dx
    mov al, bh
    out dx, al
    mov dx, NE2K_REG_RSAR0
    mov eax, ebp
    out dx, al
    inc dx
    mov al, ah
    out dx, al
    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_DMA_WRITE
    out dx, al
    mov ecx, ebx
    shr ecx, 1
    mov dx, NE2K_DATA_PORT
    rep outsw
%ifdef KERNEL_TEST_NET_DMA_TIMEOUT_ONCE
    cmp byte [ne2k_test_dma_timeout_once], 0
    je .wait_for_completion
    mov byte [ne2k_test_dma_timeout_once], 0
    stc
    jmp .done
.wait_for_completion:
%endif
    mov bl, NE2K_ISR_RDC
    mov ecx, NE2K_DMA_TIMEOUT_MS
    call ne2k_wait_isr
    jc .failed
    mov dx, NE2K_REG_ISR
    mov al, NE2K_ISR_RDC
    out dx, al
    clc
    jmp .done
.failed:
    stc
.done:
    popad
    ret

; Recovery deliberately performs a complete bounded reset and reconfiguration
; so corrupted receive-ring state cannot survive an overrun or DMA timeout.
ne2k_recover:
    call ne2k_configure
    jc ne2k_mark_fatal
    clc
    ret

ne2k_mark_fatal:
    cmp dword [ne2k_state], NET_DEVICE_FATAL
    je .already_fatal
    inc dword [ne2k_fatal_failures]
    mov dword [ne2k_state], NET_DEVICE_FATAL
.already_fatal:
    stc
    ret

