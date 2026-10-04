; IN: EDI=struct net_device_info destination.
; OUT: EAX=0 or SYS_ERR_INVALID.
ne2k_get_info:
    test edi, edi
    jz .invalid
    pushad
    mov ebp, edi
    mov ecx, NET_INFO_SIZE
    call zero_buffer
    mov dword [ebp + NET_INFO_ABI_VERSION_OFFSET], NET_RAW_ABI_VERSION
    mov eax, [ne2k_state]
    mov [ebp + NET_INFO_STATE_OFFSET], eax
    mov eax, NET_DRIVER_FLAG_POLLING | NET_DRIVER_FLAG_IRQ_MASKED | \
             NET_DRIVER_FLAG_FCS_STRIPPED | NET_DRIVER_FLAG_SYNC_TX
    cmp dword [ne2k_state], NET_DEVICE_READY
    jne .flags_ready
    or eax, NET_DRIVER_FLAG_AVAILABLE
.flags_ready:
    mov [ebp + NET_INFO_FLAGS_OFFSET], eax
    mov dword [ebp + NET_INFO_IO_BASE_OFFSET], NE2K_IO_BASE
    mov dword [ebp + NET_INFO_IRQ_OFFSET], NE2K_IRQ
    mov dword [ebp + NET_INFO_MTU_OFFSET], NET_IPV4_MTU
    mov dword [ebp + NET_INFO_FRAME_MIN_OFFSET], NET_FRAME_MIN
    mov dword [ebp + NET_INFO_FRAME_MAX_OFFSET], NET_FRAME_MAX
    mov esi, ne2k_mac
    lea edi, [ebp + NET_INFO_MAC_OFFSET]
    mov ecx, 6
    rep movsb
    mov eax, [ne2k_tx_frames]
    mov [ebp + NET_INFO_TX_FRAMES_OFFSET], eax
    mov eax, [ne2k_rx_frames]
    mov [ebp + NET_INFO_RX_FRAMES_OFFSET], eax
    mov eax, [ne2k_malformed_lengths]
    mov [ebp + NET_INFO_MALFORMED_LENGTHS_OFFSET], eax
    mov eax, [ne2k_rx_capacity_drops]
    mov [ebp + NET_INFO_RX_CAPACITY_DROPS_OFFSET], eax
    mov eax, [ne2k_rx_overruns]
    mov [ebp + NET_INFO_RX_OVERRUNS_OFFSET], eax
    mov eax, [ne2k_dma_timeouts]
    mov [ebp + NET_INFO_DMA_TIMEOUTS_OFFSET], eax
    mov eax, [ne2k_tx_timeouts]
    mov [ebp + NET_INFO_TX_TIMEOUTS_OFFSET], eax
    mov eax, [ne2k_reset_timeouts]
    mov [ebp + NET_INFO_RESET_TIMEOUTS_OFFSET], eax
    mov eax, [ne2k_resets]
    mov [ebp + NET_INFO_RESETS_OFFSET], eax
    mov eax, [ne2k_fatal_failures]
    mov [ebp + NET_INFO_FATAL_FAILURES_OFFSET], eax
    popad
    xor eax, eax
    ret
.invalid:
    mov eax, SYS_ERR_INVALID
    ret

; IN: ESI=normalized frame, ECX=length.
; OUT: EAX=length or a stable negative syscall error.
ne2k_send_frame:
    test esi, esi
    jz .invalid
    cmp ecx, NET_FRAME_MIN
    jb .range
    cmp ecx, NET_FRAME_MAX
    ja .range
    cmp dword [ne2k_state], NET_DEVICE_READY
    jne .not_ready

    mov [ne2k_tx_length], ecx
    push esi
    push edi
    push ecx
    mov edi, NET_TX_BUFFER_BASE
    rep movsb
    mov ecx, [ne2k_tx_length]
    mov byte [NET_TX_BUFFER_BASE + ecx], 0
    pop ecx
    pop edi
    pop esi

    mov eax, NE2K_TX_START_PAGE << 8
    mov esi, NET_TX_BUFFER_BASE
    mov ecx, [ne2k_tx_length]
    call ne2k_dma_write
    jc .dma_timeout

    mov dx, NE2K_REG_ISR
    mov al, NE2K_ISR_PTX | NE2K_ISR_TXE
    out dx, al
    mov dx, NE2K_REG_TPSR
    mov al, NE2K_TX_START_PAGE
    out dx, al
    mov eax, [ne2k_tx_length]
    mov dx, NE2K_REG_TBCR0
    out dx, al
    inc dx
    mov al, ah
    out dx, al
    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_START | NE2K_CR_TRANSMIT
    out dx, al

%ifdef KERNEL_TEST_NET_TX_TIMEOUT_ONCE
    cmp byte [ne2k_test_tx_timeout_once], 0
    je .wait_for_transmit
    mov byte [ne2k_test_tx_timeout_once], 0
    jmp .tx_timeout
.wait_for_transmit:
%endif
    mov bl, NE2K_ISR_PTX | NE2K_ISR_TXE
    mov ecx, NE2K_TX_TIMEOUT_MS
    call ne2k_wait_isr
    jc .tx_timeout
    mov dx, NE2K_REG_ISR
    in al, dx
    mov bl, al
    mov al, NE2K_ISR_PTX | NE2K_ISR_TXE
    out dx, al
    test bl, NE2K_ISR_TXE
    jnz .device_error
    test bl, NE2K_ISR_PTX
    jz .device_error
    inc dword [ne2k_tx_frames]
    mov eax, [ne2k_tx_length]
    ret

.dma_timeout:
    inc dword [ne2k_dma_timeouts]
    call ne2k_recover
    jc .reset_failed
    mov eax, SYS_ERR_TIMEOUT
    ret
.tx_timeout:
    inc dword [ne2k_tx_timeouts]
    call ne2k_recover
    jc .reset_failed
    mov eax, SYS_ERR_TIMEOUT
    ret
.reset_failed:
    mov eax, SYS_ERR_RESET
    ret
.device_error:
    mov eax, SYS_ERR_DEVICE
    ret
.not_ready:
    cmp dword [ne2k_state], NET_DEVICE_FATAL
    je .device_error
    mov eax, SYS_ERR_UNAVAILABLE
    ret
.invalid:
    mov eax, SYS_ERR_INVALID
    ret
.range:
    inc dword [ne2k_malformed_lengths]
    mov eax, SYS_ERR_RANGE
    ret

; IN: EDI=destination, ECX=capacity.
; OUT: 0 for no frame, frame length, or a stable negative syscall error.
ne2k_recv_frame:
    test edi, edi
    jz .invalid
    cmp ecx, NET_FRAME_MAX
    ja .range_without_frame
    cmp dword [ne2k_state], NET_DEVICE_READY
    jne .not_ready
    mov [ne2k_rx_destination], edi
    mov [ne2k_rx_capacity], ecx

    mov dx, NE2K_REG_ISR
    in al, dx
%ifdef KERNEL_TEST_NET_OVERRUN_ONCE
    cmp byte [ne2k_test_overrun_once], 0
    je .check_overrun
    mov byte [ne2k_test_overrun_once], 0
    or al, NE2K_ISR_OVW
.check_overrun:
%endif
    test al, NE2K_ISR_OVW
    jnz .overrun

    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_PAGE1_START
    out dx, al
    mov dx, NE2K_PAGE1_CURR
    in al, dx
    mov [ne2k_current_page], al
    cmp al, NE2K_RX_START_PAGE
    jb .malformed
    cmp al, NE2K_RX_STOP_PAGE
    jae .malformed
    mov dx, NE2K_REG_CR
    mov al, NE2K_CMD_START
    out dx, al
    mov dx, NE2K_REG_BNRY
    in al, dx
    cmp al, NE2K_RX_START_PAGE
    jb .malformed
    cmp al, NE2K_RX_STOP_PAGE
    jae .malformed
    inc al
    cmp al, NE2K_RX_STOP_PAGE
    jb .packet_page_ready
    mov al, NE2K_RX_START_PAGE
.packet_page_ready:
    mov [ne2k_packet_page], al
    cmp al, [ne2k_current_page]
    je .empty

    movzx eax, byte [ne2k_packet_page]
    shl eax, 8
    mov edi, ne2k_rx_header
    mov ecx, 4
    call ne2k_dma_read
    jc .dma_timeout

    mov al, [ne2k_rx_header]
    test al, NE2K_RSR_PRX
    jz .malformed
    test al, NE2K_RSR_ERROR_MASK
    jnz .malformed
    movzx ebx, byte [ne2k_rx_header + 1]
    cmp ebx, NE2K_RX_START_PAGE
    jb .malformed
    cmp ebx, NE2K_RX_STOP_PAGE
    jae .malformed
    movzx eax, word [ne2k_rx_header + 2]
    cmp eax, NET_FRAME_MIN + 4
    jb .malformed
    cmp eax, NET_FRAME_MAX + 4
    ja .malformed
    mov edx, eax
    sub edx, 4
    mov [ne2k_rx_length], edx

    add eax, 4 + 255
    shr eax, 8
    movzx edx, byte [ne2k_packet_page]
    add eax, edx
    cmp eax, NE2K_RX_STOP_PAGE
    jb .expected_page_ready
    sub eax, NE2K_RX_PAGE_COUNT
.expected_page_ready:
    cmp eax, ebx
    jne .malformed

    mov eax, [ne2k_rx_length]
    cmp eax, [ne2k_rx_capacity]
    ja .capacity_drop
    movzx eax, byte [ne2k_packet_page]
    shl eax, 8
    add eax, 4
    mov edi, NET_RX_BUFFER_BASE
    mov ecx, [ne2k_rx_length]
    call ne2k_dma_read
    jc .dma_timeout
    call ne2k_consume_packet
    mov esi, NET_RX_BUFFER_BASE
    mov edi, [ne2k_rx_destination]
    mov ecx, [ne2k_rx_length]
    rep movsb
    inc dword [ne2k_rx_frames]
    mov eax, [ne2k_rx_length]
    ret

.capacity_drop:
    call ne2k_consume_packet
    inc dword [ne2k_rx_capacity_drops]
    mov eax, SYS_ERR_RANGE
    ret
.malformed:
    inc dword [ne2k_malformed_lengths]
    call ne2k_recover
    jc .reset_failed
    mov eax, SYS_ERR_DEVICE
    ret
.dma_timeout:
    inc dword [ne2k_dma_timeouts]
    call ne2k_recover
    jc .reset_failed
    mov eax, SYS_ERR_TIMEOUT
    ret
.overrun:
    inc dword [ne2k_rx_overruns]
    call ne2k_recover
    jc .reset_failed
    mov eax, SYS_ERR_OVERRUN
    ret
.reset_failed:
    mov eax, SYS_ERR_RESET
    ret
.empty:
    xor eax, eax
    ret
.not_ready:
    cmp dword [ne2k_state], NET_DEVICE_FATAL
    je .device_error
    mov eax, SYS_ERR_UNAVAILABLE
    ret
.device_error:
    mov eax, SYS_ERR_DEVICE
    ret
.invalid:
    mov eax, SYS_ERR_INVALID
    ret
.range_without_frame:
    mov eax, SYS_ERR_RANGE
    ret

; Advance BNRY from a previously validated ring header and acknowledge receive
; status only after the packet can no longer be returned on the next poll.
ne2k_consume_packet:
    push eax
    push edx
    mov al, [ne2k_rx_header + 1]
    cmp al, NE2K_RX_START_PAGE
    jne .ordinary
    mov al, NE2K_RX_STOP_PAGE
.ordinary:
    dec al
    mov dx, NE2K_REG_CR
    push eax
    mov al, NE2K_CMD_START
    out dx, al
    pop eax
    mov dx, NE2K_REG_BNRY
    out dx, al
    mov dx, NE2K_REG_ISR
    mov al, NE2K_ISR_PRX | NE2K_ISR_RXE
    out dx, al
    pop edx
    pop eax
    ret

