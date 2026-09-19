[bits 32]

; ----------------------------
; IDT & System Call Module
; ----------------------------
%if IDT_SIZE != 256 * 8
    %error "IDT_SIZE must describe all 256 eight-byte entries"
%endif

idt_descriptor:
    dw IDT_SIZE - 1
    dd IDT_BASE

idt_init:
    pushad

    ; Zero the complete shared-layout IDT region.
    mov edi, IDT_BASE
    mov ecx, IDT_SIZE
    call zero_buffer

    ; Give every vector a present Ring-0 fatal gate before installing the
    ; architected exception, remapped PIC, and syscall entries.
    xor ecx, ecx
.default_loop:
    mov eax, unexpected_vector_stub
    mov dl, 0x8E
    call idt_set_gate
    inc ecx
    cmp ecx, 256
    jb .default_loop

    xor ecx, ecx
.exception_loop:
    mov eax, [exception_stub_table + ecx * 4]
    mov dl, 0x8E
    call idt_set_gate
    inc ecx
    cmp ecx, 32
    jb .exception_loop

    xor ebx, ebx
.irq_loop:
    mov ecx, ebx
    add ecx, PIC_MASTER_VECTOR
    mov eax, [irq_stub_table + ebx * 4]
    mov dl, 0x8E
    call idt_set_gate
    inc ebx
    cmp ebx, 16
    jb .irq_loop

    ; A trap gate leaves IF unchanged so IRQ0 can advance while a trusted
    ; application is inside a filesystem or console system call.
    mov ecx, 0x80
    mov eax, syscall_entry
    mov dl, 0xEF
    call idt_set_gate

    lidt [idt_descriptor]
    popad
    ret

; IN: EAX=handler, ECX=vector, DL=type/attribute
idt_set_gate:
    push edi
    mov edi, ecx
    shl edi, 3
    add edi, IDT_BASE
    mov [edi], ax            ; Offset 0..15
    mov word [edi + 2], 0x08  ; Segment Selector (Code Segment)
    mov byte [edi + 4], 0    ; Reserved
    mov [edi + 5], dl
    shr eax, 16
    mov [edi + 6], ax        ; Offset 16..31
    pop edi
    ret

; ----------------------------
; System Call Handler (int 0x80)
; ----------------------------
; MINI-OS syscall ABI:
; EAX=number, EBX=arg1, ECX=arg2, EDX=arg3, EAX=return value.
; All general-purpose registers except EAX are preserved.
; Numbers, open flags, and negative errors come from transport/lib/syscall.def.
syscall_entry:
    pushad
    ; String and port-string operations in trusted syscall implementations
    ; always run forward. IRET restores the caller's saved EFLAGS, including
    ; its original DF, on ordinary returns.
    cld

    cmp eax, SYS_NR_EXIT
    je near .sys_exit
    cmp eax, SYS_NR_READ
    je near .sys_read
    cmp eax, SYS_NR_WRITE
    je near .sys_write
    cmp eax, SYS_NR_OPEN
    je near .sys_open
    cmp eax, SYS_NR_CLOSE
    je near .sys_close
    cmp eax, SYS_NR_GETKEY
    je near .sys_getkey
    cmp eax, SYS_NR_BRK
    je near .sys_brk

    cmp eax, SYS_NR_READ_FILE
    je near .sys_read_file
    cmp eax, SYS_NR_WRITE_FILE
    je near .sys_write_file
    cmp eax, SYS_NR_LSEEK
    je near .sys_lseek
    cmp eax, SYS_NR_MOVE_CURSOR
    je near .sys_move_cursor
    cmp eax, SYS_NR_CLEAR_SCREEN
    je near .sys_clear_screen
    cmp eax, SYS_NR_SET_CURSOR
    je near .sys_set_cursor_nosync
    cmp eax, SYS_NR_SAVE_SCREEN
    je near .sys_save_screen
    cmp eax, SYS_NR_RESTORE_SCREEN
    je near .sys_restore_screen
    cmp eax, SYS_NR_GET_CURSOR
    je near .sys_get_cursor
    cmp eax, SYS_NR_CLOCK_MONOTONIC_MS
    je near .sys_clock_monotonic_ms
    cmp eax, SYS_NR_KBD_POLL_KEY
    je near .sys_kbd_poll_key
    cmp eax, SYS_NR_GET_RANDOM
    je near .sys_get_random
    cmp eax, SYS_NR_NET_GET_INFO
    je near .sys_net_get_info
    cmp eax, SYS_NR_NET_SEND_FRAME
    je near .sys_net_send_frame
    cmp eax, SYS_NR_NET_RECV_FRAME
    je near .sys_net_recv_frame

    mov eax, SYS_ERR_INVALID
    jmp .syscall_return

.syscall_return:
    mov [esp + 28], eax
    popad
    iret

.sys_exit:
    mov byte [cursor_auto_sync], 1
    ; Reset file table slots 3..15
    mov ecx, 3

.clear_ft_loop:
    cmp ecx, 16
    jge .clear_ft_done
    mov eax, ecx
    shl eax, 4
    mov dword [file_table + eax], 0
    inc ecx
    jmp .clear_ft_loop
.clear_ft_done:

    ; Reset heap break pointer for next application run
    mov dword [current_brk], APP_HEAP_BASE
    ; Restore Shell stack and return to Shell
    mov esp, [saved_kernel_esp]
    jmp strict dword return_to_shell

.sys_brk:
    test ebx, ebx
    jz .sys_brk_done

    cmp ebx, APP_HEAP_BASE
    jb .sys_brk_done
    cmp ebx, APP_HEAP_END
    ja .sys_brk_done

    mov [current_brk], ebx

.sys_brk_done:
    mov eax, [current_brk]
    jmp .syscall_return

.sys_read:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    cmp ebx, 0
    jne .sys_read_bad_fd
    test edx, edx
    jz .sys_read_zero

    mov edi, ecx
    xor esi, esi

.sys_read_loop:
    cmp esi, edx         ; Reached max requested bytes?
    jge near .sys_read_done

    call kbd_read_char_blocking
    test al, al          ; Ignore 0 (key releases / unmapped keys)
    jz near .sys_read_loop

    cmp al, 13           ; Carriage return -> newline
    je near .sys_read_newline
    cmp al, 10           ; Line feed -> newline
    je near .sys_read_newline

    ; Check backspace (8)
    cmp al, 8
    jne near .sys_read_store_char

    ; Backspace pressed
    test esi, esi
    jz near .sys_read_loop    ; Nothing to backspace
    dec esi
    dec edi
    ; Echo backspace to VGA console (backspace, space, backspace)
    call vga_putc
    mov al, ' '
    call vga_putc
    mov al, 8
    call vga_putc
    jmp near .sys_read_loop

.sys_read_store_char:
    ; Store ASCII char in buffer
    mov [edi], al
    inc edi
    inc esi

    ; Echo character to VGA screen
    call vga_putc
    jmp near .sys_read_loop

.sys_read_newline:
    mov al, 10
    call vga_putc
    jmp .sys_read_done

.sys_read_done:
    mov [tmp_read_cnt], esi
    jmp .sys_read_exit

.sys_read_bad_fd:
    mov dword [tmp_read_cnt], SYS_ERR_BAD_FD
    jmp .sys_read_exit

.sys_read_zero:
    mov dword [tmp_read_cnt], 0

.sys_read_exit:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    mov eax, [tmp_read_cnt]
    jmp .syscall_return

.sys_getkey:
.sys_getkey_loop:
    call kbd_read_char_blocking
    test al, al
    jz .sys_getkey_loop
    movzx eax, al
    jmp .syscall_return

.sys_clock_monotonic_ms:
    mov eax, [monotonic_ms]
    jmp .syscall_return

.sys_kbd_poll_key:
    call kbd_poll_char
    movzx eax, al
    jmp .syscall_return

.sys_get_random:
    test ecx, ecx
    jz .sys_get_random_zero
    cmp ecx, RANDOM_REQUEST_MAX
    ja .sys_get_random_range
    test ebx, ebx
    jz .sys_get_random_invalid
    mov edi, ebx
    call random_fill
    jc .sys_get_random_unavailable
    mov eax, ecx
    jmp .syscall_return
.sys_get_random_zero:
    xor eax, eax
    jmp .syscall_return
.sys_get_random_range:
    mov eax, SYS_ERR_RANGE
    jmp .syscall_return
.sys_get_random_invalid:
    mov eax, SYS_ERR_INVALID
    jmp .syscall_return
.sys_get_random_unavailable:
    mov eax, SYS_ERR_UNAVAILABLE
    jmp .syscall_return

.sys_net_get_info:
    mov edi, ebx
    call ne2k_get_info
    jmp .syscall_return

.sys_net_send_frame:
    mov esi, ebx
    call ne2k_send_frame
    jmp .syscall_return

.sys_net_recv_frame:
    mov edi, ebx
    call ne2k_recv_frame
    jmp .syscall_return

.sys_write:
    cmp ebx, 1
    je .sys_write_fd_ok
    cmp ebx, 2
    jne .sys_write_bad_fd
.sys_write_fd_ok:
    pusha
    mov esi, ecx            ; buffer pointer
    mov ecx, edx            ; length
    call vga_print_n
    popa
    mov eax, edx
    jmp .syscall_return
.sys_write_bad_fd:
    mov eax, SYS_ERR_BAD_FD
    jmp .syscall_return

.sys_open:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    test ebx, ebx
    jz near .sys_open_invalid
    mov [tmp_open_flags], ecx
    mov eax, ecx
    test eax, 0xFFFFFFE0
    jnz near .sys_open_invalid
    test eax, SYS_OPEN_READ | SYS_OPEN_WRITE
    jz near .sys_open_invalid
    test eax, SYS_OPEN_TRUNCATE
    jz .sys_open_check_append
    test eax, SYS_OPEN_WRITE
    jz near .sys_open_invalid
.sys_open_check_append:
    test eax, SYS_OPEN_APPEND
    jz .sys_open_flags_ok
    test eax, SYS_OPEN_WRITE
    jz near .sys_open_invalid
    test eax, SYS_OPEN_TRUNCATE
    jnz near .sys_open_invalid

.sys_open_flags_ok:
    mov esi, ebx

    mov ecx, 3

.sys_open_slot_loop:
    cmp ecx, 16
    jge near .sys_open_full
    mov eax, ecx
    shl eax, 4
    cmp dword [file_table + eax], 0
    je near .sys_open_found_slot
    inc ecx
    jmp near .sys_open_slot_loop

.sys_open_found_slot:
    mov [tmp_fd_slot], ecx

    call fs_lookup_path
    cmp eax, FS_ERR_NOT_FOUND
    je .sys_open_missing
    cmp eax, FS_OK
    jl near .sys_open_fs_fail
    jmp near .sys_open_exists

.sys_open_missing:
    test dword [tmp_open_flags], SYS_OPEN_CREATE
    jz near .sys_open_invalid

    push ebx
    mov esi, ebx
    call fs_create_file_path
    pop ebx
    cmp eax, 0
    jl near .sys_open_fs_fail

    push ebx
    mov esi, ebx
    call fs_lookup_path
    pop ebx
    cmp eax, FS_OK
    jl near .sys_open_fs_fail

.sys_open_exists:
    mov [tmp_open_inode], eax

    mov edi, BUF_INODE
    call fs_read_inode
    cmp eax, FS_OK
    jl near .sys_open_fs_fail
    cmp byte [BUF_INODE + INODE_TYPE_OFF], 1
    jne near .sys_open_invalid

    test dword [tmp_open_flags], SYS_OPEN_TRUNCATE
    jz .sys_open_skip_truncate

    push esi
    push edi
    call fs_begin_mutation
    pop edi
    pop esi
    cmp eax, FS_OK
    jl near .sys_open_fs_fail

    cmp dword [BUF_INODE + INODE_BLOCKS_OFF], 0
    je .sys_open_empty_inode

    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ecx, [BUF_INODE + INODE_BLOCKS_OFF]
    mov [tmp_chain_count], ecx
    mov ebx, ecx
    dec ebx
    call fs_fat_get_nth_block
    cmp eax, FS_OK
    jl near .sys_open_truncate_fail

    mov eax, [BUF_INODE + INODE_START_OFF]
    call fs_fat_read_entry
    cmp eax, FS_OK
    jl near .sys_open_truncate_fail
    mov [tmp_chain_next], eax

    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ebx, FS_FAT_EOC
    call fs_fat_write_entry
    cmp eax, FS_OK
    jl near .sys_open_truncate_fail

    mov dword [BUF_INODE + INODE_SIZE_OFF], 0
    mov dword [BUF_INODE + INODE_BLOCKS_OFF], 1
    mov eax, [tmp_open_inode]
    mov esi, BUF_INODE
    call fs_write_inode
    cmp eax, FS_OK
    jl .sys_open_restore_fat

    mov eax, [tmp_chain_next]
    cmp eax, FS_FAT_EOC
    je .sys_open_zero_first
    mov ecx, [tmp_chain_count]
    dec ecx
    call fs_fat_free_chain
    cmp eax, FS_OK
    jl near .sys_open_truncate_fail

.sys_open_zero_first:
    mov eax, [BUF_INODE + INODE_START_OFF]
    add eax, FS_DATA_START_LBA
    push edi
    push esi
    mov edi, BUF_SECTOR
    call zero_sector
    pop esi
    pop edi
    push esi
    mov esi, BUF_SECTOR
    call ata_write_sector_lba28
    pop esi
    jc near .sys_open_truncate_fail
    jmp .sys_open_truncate_done

.sys_open_empty_inode:
    mov dword [BUF_INODE + INODE_SIZE_OFF], 0
    mov eax, [tmp_open_inode]
    mov esi, BUF_INODE
    call fs_write_inode
    cmp eax, FS_OK
    jl near .sys_open_truncate_fail
    jmp .sys_open_truncate_done

.sys_open_restore_fat:
    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ebx, [tmp_chain_next]
    call fs_fat_write_entry
    jmp .sys_open_truncate_fail

.sys_open_truncate_done:
    mov eax, FS_OK
    call fs_complete_mutation
    cmp eax, FS_OK
    jl near .sys_open_fs_fail
    jmp .sys_open_skip_truncate

.sys_open_truncate_fail:
    mov eax, FS_ERR_IO
    call fs_complete_mutation
    jmp near .sys_open_fs_fail

.sys_open_skip_truncate:

    mov ecx, [tmp_fd_slot]
    shl ecx, 4
    mov dword [file_table + ecx], 1
    mov eax, [tmp_open_inode]
    mov dword [file_table + ecx + 4], eax
    xor eax, eax
    test dword [tmp_open_flags], SYS_OPEN_APPEND
    jz .sys_open_position_ready
    mov eax, [BUF_INODE + INODE_SIZE_OFF]
.sys_open_position_ready:
    mov dword [file_table + ecx + 8], eax
    mov eax, [tmp_open_flags]
    mov dword [file_table + ecx + 12], eax

    mov eax, [tmp_fd_slot]
    jmp near .sys_open_done

.sys_open_full:
    mov eax, SYS_ERR_BAD_FD
    jmp .sys_open_done
.sys_open_invalid:
    mov eax, SYS_ERR_INVALID
    jmp .sys_open_done
.sys_open_fs_fail:
    mov eax, SYS_ERR_IO

.sys_open_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    jmp .syscall_return

.sys_close:
    cmp ebx, 3
    jl near .sys_close_err
    cmp ebx, 16
    jge near .sys_close_err

    mov eax, ebx
    shl eax, 4
    cmp dword [file_table + eax], 1
    jne near .sys_close_err
    mov dword [file_table + eax], 0
    xor eax, eax
    jmp .syscall_return

.sys_close_err:
    mov eax, SYS_ERR_BAD_FD
    jmp .syscall_return

.sys_read_file:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov [tmp_rw_buffer], ecx
    mov [tmp_rw_request], edx

    cmp ebx, 3
    jl near .sys_rf_bad_fd
    cmp ebx, 16
    jge near .sys_rf_bad_fd

    mov eax, ebx
    shl eax, 4
    cmp dword [file_table + eax], 1
    jne near .sys_rf_bad_fd
    test dword [file_table + eax + 12], SYS_OPEN_READ
    jz near .sys_rf_access

    mov [tmp_fd_idx], eax

    mov eax, [file_table + eax + 4]
    mov edi, BUF_INODE
    call fs_read_inode
    cmp eax, FS_OK
    jl near .sys_rf_err

    mov eax, [tmp_fd_idx]
    mov esi, [file_table + eax + 8]
    mov edi, [tmp_rw_buffer]
    mov ecx, [tmp_rw_request]
    mov edx, [BUF_INODE + INODE_SIZE_OFF]

    cmp esi, edx
    jge near .sys_rf_eof

    sub edx, esi
    cmp ecx, edx
    jbe near .sys_rf_count_ok
    mov ecx, edx

.sys_rf_count_ok:
    mov [tmp_rw_count], ecx
    mov dword [tmp_rw_done], 0

.sys_rf_loop:
    cmp dword [tmp_rw_count], 0
    je near .sys_rf_done

    mov ebx, esi
    shr ebx, 9
    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ecx, [BUF_INODE + INODE_BLOCKS_OFF]
    call fs_fat_get_nth_block
    cmp eax, FS_OK
    jl near .sys_rf_err
    add eax, FS_DATA_START_LBA

    mov ebx, edi
    mov edi, BUF_SECTOR
    call ata_read_sector_lba28
    mov edi, ebx
    jc near .sys_rf_err

    mov ebx, esi
    and ebx, 511

    mov edx, 512
    sub edx, ebx
    cmp edx, [tmp_rw_count]
    jle near .sys_rf_copy_sz
    mov edx, [tmp_rw_count]

.sys_rf_copy_sz:
    push esi
    push edi
    push ecx

    mov esi, BUF_SECTOR
    add esi, ebx
    mov ecx, edx
    rep movsb

    pop ecx
    pop edi
    pop esi

    add edi, edx
    add esi, edx
    add [tmp_rw_done], edx
    sub [tmp_rw_count], edx
    jmp near .sys_rf_loop

.sys_rf_done:
    mov eax, [tmp_fd_idx]
    mov [file_table + eax + 8], esi
    mov eax, [tmp_rw_done]
    jmp near .sys_rf_exit

.sys_rf_eof:
    xor eax, eax
    jmp near .sys_rf_exit

.sys_rf_bad_fd:
    mov eax, SYS_ERR_BAD_FD
    jmp .sys_rf_exit
.sys_rf_access:
    mov eax, SYS_ERR_ACCESS
    jmp .sys_rf_exit
.sys_rf_err:
    mov eax, SYS_ERR_IO

.sys_rf_exit:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    jmp .syscall_return

.sys_write_file:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov [tmp_rw_buffer], ecx
    mov [tmp_rw_request], edx

    cmp ebx, 3
    jl near .sys_wf_bad_fd
    cmp ebx, 16
    jge near .sys_wf_bad_fd

    mov eax, ebx
    shl eax, 4
    cmp dword [file_table + eax], 1
    jne near .sys_wf_bad_fd
    test dword [file_table + eax + 12], SYS_OPEN_WRITE
    jz near .sys_wf_access

    mov [tmp_fd_idx], eax

    test edx, edx
    jz near .sys_wf_zero

    mov eax, [file_table + eax + 4]
    mov [tmp_open_inode], eax
    mov edi, BUF_INODE
    call fs_read_inode
    cmp eax, FS_OK
    jl near .sys_wf_err

    mov eax, [tmp_fd_idx]
    test dword [file_table + eax + 12], SYS_OPEN_APPEND
    jz .sys_wf_position_ready
    mov ecx, [BUF_INODE + INODE_SIZE_OFF]
    mov [file_table + eax + 8], ecx
.sys_wf_position_ready:
    mov ecx, [file_table + eax + 8]
    cmp ecx, FS_MAX_FILE_SIZE
    ja near .sys_wf_range
    mov eax, FS_MAX_FILE_SIZE
    sub eax, ecx
    cmp dword [tmp_rw_request], eax
    ja near .sys_wf_range

    push esi
    push edi
    call fs_begin_mutation
    pop edi
    pop esi
    cmp eax, FS_OK
    jl near .sys_wf_err

    cmp dword [BUF_INODE + INODE_BLOCKS_OFF], 0
    jne near .sys_wf_has_block

    call fs_fat_alloc_block
    cmp eax, FS_OK
    jl near .sys_wf_err
    mov [BUF_INODE + INODE_START_OFF], eax
    mov dword [BUF_INODE + INODE_BLOCKS_OFF], 1
    mov eax, [tmp_open_inode]
    push esi
    mov esi, BUF_INODE
    call fs_write_inode
    pop esi
    cmp eax, FS_OK
    jl .sys_wf_initial_rollback
    jmp .sys_wf_has_block

.sys_wf_initial_rollback:
    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ecx, 1
    call fs_fat_free_chain
    mov dword [BUF_INODE + INODE_START_OFF], 0
    mov dword [BUF_INODE + INODE_BLOCKS_OFF], 0
    jmp near .sys_wf_err

.sys_wf_has_block:
    mov eax, [tmp_fd_idx]
    mov esi, [file_table + eax + 8]
    mov edi, [tmp_rw_buffer]
    mov ecx, [tmp_rw_request]

    mov [tmp_rw_count], ecx
    mov dword [tmp_rw_done], 0

.sys_wf_loop:
    cmp dword [tmp_rw_count], 0
    je near .sys_wf_done

    mov eax, esi
    shr eax, 9
    cmp eax, FS_DATA_BLOCK_COUNT
    jae near .sys_wf_err
    mov [tmp_target_block], eax

.sys_wf_ensure_capacity:
    mov eax, [tmp_target_block]
    cmp eax, [BUF_INODE + INODE_BLOCKS_OFF]
    jb .sys_wf_have_capacity

    mov ecx, [BUF_INODE + INODE_BLOCKS_OFF]
    cmp ecx, 1
    jb near .sys_wf_err
    mov ebx, ecx
    dec ebx
    mov eax, [BUF_INODE + INODE_START_OFF]
    call fs_fat_get_nth_block
    cmp eax, FS_OK
    jl near .sys_wf_err
    mov [tmp_wf_chain_block], eax

    call fs_fat_alloc_block
    cmp eax, FS_OK
    jl near .sys_wf_err
    mov [tmp_wf_chain_next], eax

    mov ebx, eax
    mov eax, [tmp_wf_chain_block]
    call fs_fat_write_entry
    cmp eax, FS_OK
    jl .sys_wf_free_unlinked

    inc dword [BUF_INODE + INODE_BLOCKS_OFF]
    mov eax, [tmp_open_inode]
    push esi
    mov esi, BUF_INODE
    call fs_write_inode
    pop esi
    cmp eax, FS_OK
    jl .sys_wf_rollback_link
    jmp .sys_wf_ensure_capacity

.sys_wf_free_unlinked:
    mov eax, [tmp_wf_chain_block]
    mov ebx, FS_FAT_EOC
    call fs_fat_write_entry
    cmp eax, FS_OK
    jl near .sys_wf_err
    mov eax, [tmp_wf_chain_next]
    mov ecx, 1
    call fs_fat_free_chain
    jmp near .sys_wf_err

.sys_wf_rollback_link:
    dec dword [BUF_INODE + INODE_BLOCKS_OFF]
    mov eax, [tmp_wf_chain_block]
    mov ebx, FS_FAT_EOC
    call fs_fat_write_entry
    cmp eax, FS_OK
    jl near .sys_wf_err
    mov eax, [tmp_wf_chain_next]
    mov ecx, 1
    call fs_fat_free_chain
    jmp near .sys_wf_err

.sys_wf_have_capacity:
    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ebx, [tmp_target_block]
    mov ecx, [BUF_INODE + INODE_BLOCKS_OFF]
    call fs_fat_get_nth_block
    cmp eax, FS_OK
    jl near .sys_wf_err
    add eax, FS_DATA_START_LBA
    mov [tmp_sector_lba], eax

    mov ebx, edi
    mov edi, BUF_SECTOR
    call ata_read_sector_lba28
    mov edi, ebx
    jc near .sys_wf_io_err

    mov ebx, esi
    and ebx, 511

    mov edx, 512
    sub edx, ebx
    cmp edx, [tmp_rw_count]
    jle near .sys_wf_copy_sz
    mov edx, [tmp_rw_count]

.sys_wf_copy_sz:
    push esi
    push edi
    push ecx

    mov esi, edi
    mov edi, BUF_SECTOR
    add edi, ebx
    mov ecx, edx
    rep movsb

    pop ecx
    pop edi
    pop esi

    push eax
    push esi
    mov eax, [tmp_sector_lba]
    mov esi, BUF_SECTOR
    call ata_write_sector_lba28
    pop esi
    pop eax
    jc near .sys_wf_io_err

    add edi, edx
    add esi, edx
    add [tmp_rw_done], edx
    sub [tmp_rw_count], edx
    jmp near .sys_wf_loop

.sys_wf_done:
    mov eax, [tmp_fd_idx]
    mov [file_table + eax + 8], esi

    mov eax, [BUF_INODE + INODE_SIZE_OFF]
    cmp esi, eax
    jle .sys_wf_skip_size
    mov [BUF_INODE + INODE_SIZE_OFF], esi
.sys_wf_skip_size:
    mov eax, [tmp_open_inode]
    push esi
    mov esi, BUF_INODE
    call fs_write_inode
    pop esi
    cmp eax, FS_OK
    jl near .sys_wf_err

    mov eax, FS_OK
    call fs_complete_mutation
    cmp eax, FS_OK
    jl .sys_wf_complete_error
    mov eax, [tmp_rw_done]
    jmp near .sys_wf_exit

.sys_wf_zero:
    xor eax, eax
    jmp .sys_wf_exit
.sys_wf_bad_fd:
    mov eax, SYS_ERR_BAD_FD
    jmp .sys_wf_exit
.sys_wf_access:
    mov eax, SYS_ERR_ACCESS
    jmp .sys_wf_exit
.sys_wf_range:
    mov eax, SYS_ERR_RANGE
    jmp .sys_wf_exit
.sys_wf_io_err:
    mov eax, FS_ERR_IO
.sys_wf_err:
    cmp byte [fs_mutation_active], 1
    jne .sys_wf_map_error
    call fs_complete_mutation
.sys_wf_map_error:
    mov eax, SYS_ERR_IO
    jmp .sys_wf_exit

.sys_wf_complete_error:
    mov eax, SYS_ERR_IO

.sys_wf_exit:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    jmp .syscall_return

.sys_lseek:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    cmp ebx, 3
    jl near .sys_seek_bad_fd
    cmp ebx, 16
    jge near .sys_seek_bad_fd

    mov eax, ebx
    shl eax, 4
    cmp dword [file_table + eax], 1
    jne near .sys_seek_bad_fd

    mov esi, eax

    mov eax, [file_table + esi + 4]
    mov edi, BUF_INODE
    call fs_read_inode
    cmp eax, FS_OK
    jl near .sys_seek_io
    mov eax, [BUF_INODE + INODE_SIZE_OFF]

    cmp edx, 0
    je near .seek_set
    cmp edx, 1
    je near .seek_cur
    cmp edx, 2
    je near .seek_end
    jmp near .sys_seek_invalid

.seek_set:
    mov eax, ecx
    jmp near .seek_check

.seek_cur:
    mov eax, [file_table + esi + 8]
    add eax, ecx
    jo near .sys_seek_range
    jmp near .seek_check

.seek_end:
    add eax, ecx
    jo near .sys_seek_range

.seek_check:
    test eax, eax
    js near .sys_seek_range
    cmp eax, FS_MAX_FILE_SIZE
    ja near .sys_seek_range

.seek_ok:
    mov [file_table + esi + 8], eax
    jmp near .sys_seek_exit

.sys_seek_bad_fd:
    mov eax, SYS_ERR_BAD_FD
    jmp near .sys_seek_exit
.sys_seek_invalid:
    mov eax, SYS_ERR_INVALID
    jmp near .sys_seek_exit
.sys_seek_io:
    mov eax, SYS_ERR_IO
    jmp near .sys_seek_exit
.sys_seek_range:
    mov eax, SYS_ERR_RANGE

.sys_seek_exit:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    jmp .syscall_return

.sys_move_cursor:
    push ebx
    push ecx
    cmp ebx, 24
    jbe .mc_row_ok
    mov ebx, 24
.mc_row_ok:
    cmp ecx, 79
    jbe .mc_col_ok
    mov ecx, 79
.mc_col_ok:
    mov [cursor_row], ebx
    mov [cursor_col], ecx
    mov byte [cursor_auto_sync], 0
    call vga_sync_cursor
    pop ecx
    pop ebx
    xor eax, eax
    jmp .syscall_return

.sys_clear_screen:
    mov byte [cursor_auto_sync], 1
    call vga_clear
    xor eax, eax
    jmp .syscall_return

.sys_set_cursor_nosync:
    push ebx
    push ecx
    cmp ebx, 24
    jbe .scns_row_ok
    mov ebx, 24
.scns_row_ok:
    cmp ecx, 79
    jbe .scns_col_ok
    mov ecx, 79
.scns_col_ok:
    mov [cursor_row], ebx
    mov [cursor_col], ecx
    pop ecx
    pop ebx
    xor eax, eax
    jmp .syscall_return

.sys_save_screen:
    push esi
    push edi
    push ecx
    mov esi, VGA_BUFFER
    mov edi, saved_vga_buffer
    mov ecx, 4000
    rep movsb
    mov eax, [cursor_row]
    mov [saved_cursor_row], eax
    mov eax, [cursor_col]
    mov [saved_cursor_col], eax
    pop ecx
    pop edi
    pop esi
    xor eax, eax
    jmp .syscall_return

.sys_restore_screen:
    push esi
    push edi
    push ecx
    mov esi, saved_vga_buffer
    mov edi, VGA_BUFFER
    mov ecx, 4000
    rep movsb
    mov eax, [saved_cursor_row]
    mov [cursor_row], eax
    mov eax, [saved_cursor_col]
    mov [cursor_col], eax
    mov byte [cursor_auto_sync], 1
    call vga_sync_cursor
    pop ecx
    pop edi
    pop esi
    xor eax, eax
    jmp .syscall_return

.sys_get_cursor:
    mov eax, [cursor_row]
    imul eax, VGA_WIDTH
    add eax, [cursor_col]
    jmp .syscall_return

saved_cursor_row dd 0
saved_cursor_col dd 0
saved_vga_buffer times 4000 db 0

tmp_read_cnt dd 0
current_brk dd APP_HEAP_BASE
tmp_fd_slot dd 0
tmp_open_flags dd 0
tmp_open_inode dd 0
tmp_fd_idx dd 0
tmp_rw_buffer dd 0
tmp_rw_request dd 0
tmp_rw_count dd 0
tmp_rw_done dd 0
tmp_sector_lba dd 0
tmp_target_block dd 0
tmp_wf_chain_block dd 0
tmp_wf_chain_next dd 0
file_table times 256 db 0
