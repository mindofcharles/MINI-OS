; ----------------------------
; Generic helpers
; ----------------------------
; IN: EDI=buffer, ECX=byte count
zero_buffer:
    push eax
    push ecx
    push edi
    xor eax, eax
    rep stosb
    pop edi
    pop ecx
    pop eax
    ret

; IN: EDI points to a 512-byte sector buffer
zero_sector:
    push ecx
    mov ecx, 512
    call zero_buffer
    pop ecx
    ret

; Fill and verify the memory guards shared by the application, kernel, and
; dedicated interrupt stack. These guards detect boundary corruption but are not
; a privilege or paging boundary.
platform_layout_initialize:
    pushad
    mov edi, BOOT_STACK_BASE
    mov ecx, BOOT_STACK_TOP - BOOT_STACK_BASE
    call zero_buffer
    mov edi, kernel_image_used_end
    mov ecx, KERNEL_IMAGE_END
    sub ecx, edi
    call zero_buffer
    mov edi, BUF_SUPERBLOCK
    mov ecx, KERNEL_BUFFER_SIZE
    call zero_buffer
    mov edi, BUF_BITMAP
    mov ecx, KERNEL_BUFFER_SIZE
    call zero_buffer
    mov edi, BUF_SECTOR
    mov ecx, KERNEL_BUFFER_SIZE
    call zero_buffer
    mov edi, BUF_TEXT
    mov ecx, KERNEL_BUFFER_SIZE
    call zero_buffer
    mov edi, BUF_INODE
    mov ecx, KERNEL_BUFFER_SIZE
    call zero_buffer
    mov edi, BUF_CMD
    mov ecx, KERNEL_BUFFER_SIZE
    call zero_buffer
    mov edi, NET_RX_BUFFER_BASE
    mov ecx, NET_FRAME_BUFFER_SIZE
    call zero_buffer
    mov edi, NET_TX_BUFFER_BASE
    mov ecx, NET_FRAME_BUFFER_SIZE
    call zero_buffer
    mov edi, INTERRUPT_STACK_BASE
    mov ecx, INTERRUPT_STACK_TOP - INTERRUPT_STACK_BASE
    call zero_buffer
    mov edi, APP_IMAGE_BASE
    mov ecx, APP_IMAGE_SIZE
    call zero_buffer

    mov edi, INTERRUPT_STACK_CANARY_BASE
    mov ecx, INTERRUPT_STACK_CANARY_SIZE
    mov al, MEMORY_CANARY_VALUE
    rep stosb
    call platform_prepare_app_memory

    ; Exercise the lower usable interrupt-stack boundary while interrupts are
    ; still disabled, leaving the adjacent canary untouched.
    mov [layout_test_esp], esp
    mov esp, INTERRUPT_STACK_TOP
    sub esp, INTERRUPT_STACK_TOP - INTERRUPT_STACK_BASE
    mov dword [esp], 0x13579BDF
    cmp dword [esp], 0x13579BDF
    mov dword [esp], 0
    jne .test_failed
    mov esp, [layout_test_esp]
    call platform_verify_guards
    mov [layout_guard_result], eax
    jmp .done
.test_failed:
    mov esp, [layout_test_esp]
    mov dword [layout_guard_result], 0
.done:
    popad
    mov eax, [layout_guard_result]
    ret

platform_prepare_app_memory:
    pushad
    mov edi, APP_HEAP_BASE
    mov ecx, APP_HEAP_SIZE
    call zero_buffer
    mov edi, APP_ARG_BLOCK_BASE
    mov ecx, APP_ARG_BLOCK_SIZE
    call zero_buffer
    mov edi, APP_STACK_BASE
    mov ecx, APP_STACK_SIZE
    call zero_buffer
    mov edi, APP_HEAP_CANARY_BASE
    mov ecx, APP_HEAP_CANARY_SIZE
    mov al, MEMORY_CANARY_VALUE
    rep stosb
    mov edi, APP_STACK_CANARY_BASE
    mov ecx, APP_STACK_CANARY_SIZE
    rep stosb
    popad
    ret

platform_verify_guards:
    push ecx
    push esi
    mov esi, KERNEL_STACK_CANARY_BASE
    mov ecx, KERNEL_STACK_CANARY_SIZE
    call platform_verify_canary
    test eax, eax
    jz .done
    mov esi, INTERRUPT_STACK_CANARY_BASE
    mov ecx, INTERRUPT_STACK_CANARY_SIZE
    call platform_verify_canary
    test eax, eax
    jz .done
    mov esi, APP_HEAP_CANARY_BASE
    mov ecx, APP_HEAP_CANARY_SIZE
    call platform_verify_canary
    test eax, eax
    jz .done
    mov esi, APP_STACK_CANARY_BASE
    mov ecx, APP_STACK_CANARY_SIZE
    call platform_verify_canary
.done:
    pop esi
    pop ecx
    ret

; IN: ESI=canary base, ECX=canary size
; OUT: EAX=1 when every byte matches, otherwise 0
platform_verify_canary:
    push ecx
    push esi
    mov eax, 1
.loop:
    test ecx, ecx
    jz .done
    cmp byte [esi], MEMORY_CANARY_VALUE
    jne .failed
    inc esi
    dec ecx
    jmp .loop
.failed:
    xor eax, eax
.done:
    pop esi
    pop ecx
    ret

; IN: ESI=src, EDI=dst, ECX=bytes
copy_bytes:
    push ecx
    rep movsb
    pop ecx
    ret

; IN: ESI=src zero-terminated, EDI=dst
copy_string:
    push eax
.loop:
    lodsb
    stosb
    test al, al
    jnz .loop
    pop eax
    ret

; IN: ESI=src zero-terminated, EDI=dst fixed 27 bytes
copy_name_27:
    push eax
    push ecx
    mov ecx, INODE_NAME_LEN
.copy:
    cmp ecx, 0
    je .done
    lodsb
    stosb
    dec ecx
    test al, al
    jz .pad
    jmp .copy
.pad:
    xor al, al
.pad_loop:
    cmp ecx, 0
    je .done
    stosb
    dec ecx
    jmp .pad_loop
.done:
    pop ecx
    pop eax
    ret

; IN: AL
; OUT: AL uppercased if a-z
char_to_upper:
    cmp al, 'a'
    jb .done
    cmp al, 'z'
    ja .done
    sub al, 32
.done:
    ret

; IN: ESI=str1, EDI=str2 (both 0-terminated)
; OUT: AL=1 equal, 0 not equal (case-insensitive)
str_eq_ci:
    push ebx
.loop:
    mov al, [esi]
    mov bl, [edi]

    push eax
    call char_to_upper
    mov dl, al
    pop eax

    mov al, bl
    call char_to_upper
    mov bl, al

    cmp dl, bl
    jne .no

    cmp dl, 0
    je .yes

    inc esi
    inc edi
    jmp .loop

.yes:
    mov al, 1
    pop ebx
    ret

.no:
    mov al, 0
    pop ebx
    ret

; IN: ESI=str1, EDI=str2 (both 0-terminated)
; OUT: AL=1 equal, 0 not equal (case-sensitive)
str_eq:
    push ebx
.loop:
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .no
    test al, al
    jz .yes
    inc esi
    inc edi
    jmp .loop
.yes:
    mov al, 1
    pop ebx
    ret
.no:
    xor al, al
    pop ebx
    ret

; IN: ESI=input string, EDI=inode name field (27 bytes)
; OUT: AL=1 equal, 0 not equal (case-insensitive)
name_field_eq_input:
    push ebx
    push ecx

    mov ecx, INODE_NAME_LEN
.loop:
    mov al, [esi]
    mov bl, [edi]

    push eax
    call char_to_upper
    mov dl, al
    pop eax

    mov al, bl
    call char_to_upper
    mov bl, al

    cmp dl, bl
    jne .no

    cmp dl, 0
    je .yes

    inc esi
    inc edi
    dec ecx
    jnz .loop

    cmp byte [esi], 0
    jne .no

.yes:
    mov al, 1
    pop ecx
    pop ebx
    ret

.no:
    mov al, 0
    pop ecx
    pop ebx
    ret
