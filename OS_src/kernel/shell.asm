; ----------------------------
; Shell
; ----------------------------
; IN: ESI = command line buffer
shell_dispatch:
    call parse_line

    mov eax, [tok_cmd]
    test eax, eax
    jz .done

    mov esi, [tok_cmd]
    mov edi, cmd_help
    call str_eq_ci
    cmp al, 1
    je near .help

    mov esi, [tok_cmd]
    mov edi, cmd_ls
    call str_eq_ci
    cmp al, 1
    je near .ls

    mov esi, [tok_cmd]
    mov edi, cmd_pwd
    call str_eq_ci
    cmp al, 1
    je near .pwd

    mov esi, [tok_cmd]
    mov edi, cmd_cd
    call str_eq_ci
    cmp al, 1
    je near .cd

    mov esi, [tok_cmd]
    mov edi, cmd_mkdir
    call str_eq_ci
    cmp al, 1
    je near .mkdir

    mov esi, [tok_cmd]
    mov edi, cmd_touch
    call str_eq_ci
    cmp al, 1
    je near .touch

    mov esi, [tok_cmd]
    mov edi, cmd_cat
    call str_eq_ci
    cmp al, 1
    je near .cat

    mov esi, [tok_cmd]
    mov edi, cmd_edit
    call str_eq_ci
    cmp al, 1
    je near .edit

    mov esi, [tok_cmd]
    mov edi, cmd_rm
    call str_eq_ci
    cmp al, 1
    je near .rm

    mov esi, [tok_cmd]
    mov edi, cmd_mv
    call str_eq_ci
    cmp al, 1
    je near .mv

    mov esi, [tok_cmd]
    mov edi, cmd_format
    call str_eq_ci
    cmp al, 1
    je near .format

    mov esi, [tok_cmd]
    mov edi, cmd_run
    call str_eq_ci
    cmp al, 1
    je near .run

    mov esi, [tok_cmd]
    mov edi, cmd_vedit
    call str_eq_ci
    cmp al, 1
    je near .vedit_cmd

    mov esi, msg_unknown
    call vga_print
    jmp .done

.help:
    mov esi, msg_help
    call vga_print
    jmp .done

.ls:
    call fs_list_cwd
    cmp eax, FS_OK
    jl .fs_error
    jmp .done

.pwd:
    mov esi, cwd_path
    call vga_print
    call vga_newline
    jmp .done

.cd:
    mov esi, [tok_arg1]
    test esi, esi
    jz .cd_usage
    call fs_change_dir_path
    cmp eax, FS_OK
    je .done
    jmp .fs_error
.cd_usage:
    mov esi, msg_usage_cd
    call vga_print
    jmp .done

.mkdir:
    mov esi, [tok_arg1]
    test esi, esi
    jz .mkdir_usage
    call fs_create_dir_path
    cmp eax, FS_OK
    je .mkdir_ok
    jmp .fs_error
.mkdir_ok:
    mov esi, msg_mkdir_ok
    call vga_print
    mov esi, [tok_arg1]
    call vga_print
    call vga_newline
    jmp .done
.mkdir_usage:
    mov esi, msg_usage_mkdir
    call vga_print
    jmp .done

.touch:
    mov esi, [tok_arg1]
    test esi, esi
    jz .touch_usage
    call fs_create_file_path
    cmp eax, FS_OK
    jge .touch_ok
    jmp .fs_error
.touch_ok:
    mov esi, msg_touch_ok
    call vga_print
    mov esi, [tok_arg1]
    call vga_print
    call vga_newline
    jmp .done
.touch_usage:
    mov esi, msg_usage_touch
    call vga_print
    jmp .done

.cat:
    mov esi, [tok_arg1]
    test esi, esi
    jz .cat_usage
    call shell_cat
    jmp .done
.cat_usage:
    mov esi, msg_usage_cat
    call vga_print
    jmp .done

.edit:
    mov esi, [tok_arg1]
    test esi, esi
    jz .edit_usage
    call shell_edit
    jmp .done
.edit_usage:
    mov esi, msg_usage_edit
    call vga_print
    jmp .done

.rm:
    mov esi, [tok_arg1]
    test esi, esi
    jz .rm_usage
    call fs_remove_path
    cmp eax, FS_OK
    je .rm_ok
    cmp eax, FS_ERR_PROTECTED
    je .rm_deny
    jmp .fs_error
.rm_ok:
    mov esi, msg_rm_ok
    call vga_print
    jmp .done
.rm_usage:
    mov esi, msg_usage_rm
    call vga_print
    jmp .done

.mv:
    mov esi, [tok_arg1]
    test esi, esi
    jz .mv_usage
    mov edi, [tok_arg2]
    test edi, edi
    jz .mv_usage
    call fs_rename_path
    cmp eax, FS_OK
    je .mv_ok
    cmp eax, FS_ERR_INVALID
    je .mv_invalid
    jmp .fs_error
.mv_ok:
    mov esi, msg_mv_ok
    call vga_print
    jmp .done
.mv_usage:
    mov esi, msg_usage_mv
    call vga_print
    jmp .done

.format:
    call fs_format
    cmp eax, FS_OK
    jl .fs_error
    jmp .done

.run:
    mov esi, [tok_arg1]
    test esi, esi
    jz .run_usage
    mov edi, [tok_arg2]
    call shell_run
    jmp .done
.run_usage:
    mov esi, msg_usage_run
    call vga_print
    jmp .done

.vedit_cmd:
    mov esi, str_vedit_bin_path
    mov edi, [tok_arg1]
    call shell_run
    jmp .done

.exists:
    mov esi, msg_exists
    call vga_print
    jmp .done

.no_inode:
    mov esi, msg_no_inode
    call vga_print
    jmp .done

.no_data:
    mov esi, msg_no_data
    call vga_print
    jmp .done

.not_file:
    mov esi, msg_not_file
    call vga_print
    jmp .done

.not_empty:
    mov esi, msg_not_empty
    call vga_print
    jmp .done

.not_dir:
    mov esi, msg_not_dir
    call vga_print
    jmp .done

.rm_deny:
    mov esi, msg_rm_deny
    call vga_print
    jmp .done

.invalid_path:
    mov esi, msg_invalid_path
    call vga_print
    jmp .done

.mv_invalid:
    mov esi, msg_mv_invalid
    call vga_print
    jmp .done

.fs_error:
    call shell_print_fs_error
    jmp .done

.done:
    ret

; IN: EAX=filesystem error
shell_print_fs_error:
    push esi
    cmp eax, FS_ERR_NOT_FOUND
    je .not_found
    cmp eax, FS_ERR_EXISTS
    je .exists
    cmp eax, FS_ERR_NOT_DIR
    je .not_dir
    cmp eax, FS_ERR_NOT_EMPTY
    je .not_empty
    cmp eax, FS_ERR_INVALID
    je .invalid
    cmp eax, FS_ERR_NO_INODE
    je .no_inode
    cmp eax, FS_ERR_NO_DATA
    je .no_data
    cmp eax, FS_ERR_IO
    je .io
    cmp eax, FS_ERR_PROTECTED
    je .protected
    cmp eax, FS_ERR_PATH_TOO_LONG
    je .too_long
    mov esi, msg_fs_corrupt
    jmp .print
.not_found:
    mov esi, msg_not_found
    jmp .print
.exists:
    mov esi, msg_exists
    jmp .print
.not_dir:
    mov esi, msg_not_dir
    jmp .print
.not_empty:
    mov esi, msg_not_empty
    jmp .print
.invalid:
    mov esi, msg_invalid_path
    jmp .print
.no_inode:
    mov esi, msg_no_inode
    jmp .print
.no_data:
    mov esi, msg_no_data
    jmp .print
.io:
    mov esi, msg_fs_io
    jmp .print
.protected:
    mov esi, msg_rm_deny
    jmp .print
.too_long:
    mov esi, msg_path_too_long
.print:
    call vga_print
    pop esi
    ret

; IN: ESI = line buffer, modifies it by replacing spaces with 0
parse_line:
    mov dword [tok_cmd], 0
    mov dword [tok_arg1], 0
    mov dword [tok_arg2], 0

    call skip_spaces
    cmp byte [esi], 0
    je .done
    mov [tok_cmd], esi
    call cut_token

    call skip_spaces
    cmp byte [esi], 0
    je .done
    mov [tok_arg1], esi
    call cut_token

    call skip_spaces
    cmp byte [esi], 0
    je .done
    mov [tok_arg2], esi
    call cut_token

.done:
    ret

; IN/OUT: ESI
skip_spaces:
.loop:
    cmp byte [esi], ' '
    jne .done
    inc esi
    jmp .loop
.done:
    ret

; IN/OUT: ESI. If token ended by space, writes 0 and advances.
cut_token:
.loop:
    mov al, [esi]
    cmp al, 0
    je .done
    cmp al, ' '
    je .split
    inc esi
    jmp .loop
.split:
    mov byte [esi], 0
    inc esi
.done:
    ret

; ----------------------------
; File handlers
; ----------------------------
; IN: ESI=file name
shell_cat:
    push esi
    call fs_lookup_path
    cmp eax, FS_OK
    jl .fs_error

    mov edi, BUF_INODE
    call fs_read_inode
    cmp eax, FS_OK
    jl .fs_error
    cmp byte [BUF_INODE + INODE_TYPE_OFF], 1
    jne .not_file

    mov eax, [BUF_INODE + INODE_SIZE_OFF]
    cmp eax, 0
    je .empty
    cmp eax, FS_MAX_FILE_SIZE
    ja .corrupt
    mov [tmp_cat_remaining], eax

    add eax, 511
    jc .corrupt
    shr eax, 9
    cmp eax, [BUF_INODE + INODE_BLOCKS_OFF]
    jne .corrupt
    mov [tmp_cat_blocks], eax

    mov eax, [BUF_INODE + INODE_START_OFF]
    mov [tmp_cat_block], eax
    mov ecx, [tmp_cat_blocks]
    mov ebx, ecx
    dec ebx
    call fs_fat_get_nth_block
    cmp eax, FS_OK
    jl .fs_error

.stream:
    mov eax, [tmp_cat_block]
    add eax, FS_DATA_START_LBA
    mov edi, BUF_TEXT
    call ata_read_sector_lba28
    jc .io

    mov ecx, [tmp_cat_remaining]
    cmp ecx, 512
    jbe .print
    mov ecx, 512
.print:
    sub [tmp_cat_remaining], ecx
    mov esi, BUF_TEXT
    call vga_print_n

    dec dword [tmp_cat_blocks]
    cmp dword [tmp_cat_blocks], 0
    je .stream_done
    mov eax, [tmp_cat_block]
    call fs_fat_read_entry
    cmp eax, FS_OK
    jl .fs_error
    cmp eax, FS_FAT_EOC
    je .corrupt
    mov [tmp_cat_block], eax
    jmp .stream

.stream_done:
    cmp dword [tmp_cat_remaining], 0
    jne .corrupt
    call vga_newline
    pop esi
    ret

.empty:
    mov esi, msg_empty
    call vga_print
    pop esi
    ret

.not_file:
    mov esi, msg_not_file
    call vga_print
    pop esi
    ret

.io:
    mov eax, FS_ERR_IO
    jmp .fs_error
.corrupt:
    mov eax, FS_ERR_CORRUPT
.fs_error:
    call shell_print_fs_error
    pop esi
    ret

; IN: ESI=file name
shell_edit:
    push esi
    call fs_lookup_path
    cmp eax, FS_ERR_NOT_FOUND
    jne .have_inode

    pop esi
    push esi
    call fs_create_file_path
    cmp eax, 0
    jl .create_fail

    pop esi
    push esi
    call fs_lookup_path
    cmp eax, FS_OK
    jl .create_fail

.have_inode:
    cmp eax, FS_OK
    jl .lookup_fail
    mov [tmp_inode_idx], eax
    mov edi, BUF_INODE
    call fs_read_inode
    cmp eax, FS_OK
    jl .lookup_fail
    cmp byte [BUF_INODE + INODE_TYPE_OFF], 1
    jne .not_file

    mov esi, msg_edit_prompt
    call vga_print

    mov edi, BUF_TEXT
    mov ecx, 510
    call kbd_read_text
    mov [tmp_edit_length], eax

    call fs_begin_mutation
    cmp eax, FS_OK
    jl .lookup_fail
    mov byte [tmp_edit_detached], 0
    mov byte [tmp_edit_allocated], 0
    mov dword [tmp_edit_tail], FS_FAT_EOC

    cmp dword [BUF_INODE + INODE_BLOCKS_OFF], 0
    jne .validate_chain

    call fs_fat_alloc_block
    cmp eax, FS_OK
    jl .lookup_fail
    mov [BUF_INODE + INODE_START_OFF], eax
    mov dword [BUF_INODE + INODE_BLOCKS_OFF], 1
    mov byte [tmp_edit_allocated], 1
    jmp .have_block

.validate_chain:
    mov ecx, [BUF_INODE + INODE_BLOCKS_OFF]
    mov [tmp_edit_old_blocks], ecx
    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ebx, ecx
    dec ebx
    call fs_fat_get_nth_block
    cmp eax, FS_OK
    jl .lookup_fail

    cmp dword [tmp_edit_old_blocks], 1
    je .have_block
    mov eax, [BUF_INODE + INODE_START_OFF]
    call fs_fat_read_entry
    cmp eax, FS_OK
    jl .lookup_fail
    cmp eax, FS_FAT_EOC
    je .corrupt
    mov [tmp_edit_tail], eax

    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ebx, FS_FAT_EOC
    call fs_fat_write_entry
    cmp eax, FS_OK
    jl .lookup_fail
    mov byte [tmp_edit_detached], 1
    mov dword [BUF_INODE + INODE_BLOCKS_OFF], 1

.have_block:
    mov eax, [BUF_INODE + INODE_START_OFF]
    cmp eax, 2
    jl .bad_block
    mov edx, FS_DATA_BLOCK_COUNT
    cmp eax, edx
    jge .bad_block

    mov edi, BUF_SECTOR
    call zero_sector

    mov esi, BUF_TEXT
    mov edi, BUF_SECTOR
    mov ecx, [tmp_edit_length]
    inc ecx
    call copy_bytes

    mov eax, [BUF_INODE + INODE_START_OFF]
    add eax, FS_DATA_START_LBA
    mov esi, BUF_SECTOR
    call ata_write_sector_lba28
    jc .write_io_fail

    mov eax, [tmp_edit_length]
    mov [BUF_INODE + INODE_SIZE_OFF], eax
    mov dword [BUF_INODE + INODE_BLOCKS_OFF], 1

    mov eax, [tmp_inode_idx]
    mov esi, BUF_INODE
    call fs_write_inode
    cmp eax, FS_OK
    jl .rollback_fail

    mov byte [tmp_edit_allocated], 0
    cmp byte [tmp_edit_detached], 0
    je .saved
    mov byte [tmp_edit_detached], 0
    mov eax, [tmp_edit_tail]
    mov ecx, [tmp_edit_old_blocks]
    dec ecx
    call fs_fat_free_chain
    cmp eax, FS_OK
    jl .lookup_fail

.saved:
    mov eax, FS_OK
    call fs_complete_mutation
    cmp eax, FS_OK
    jl .lookup_fail
    mov esi, msg_edit_ok
    call vga_print
    pop esi
    ret

.create_fail:
    call shell_print_fs_error
    pop esi
    ret

.not_file:
    mov esi, msg_not_file
    call vga_print
    pop esi
    ret

.bad_block:
    mov eax, FS_ERR_CORRUPT
    jmp .rollback_fail

.write_io_fail:
    mov eax, FS_ERR_IO
.rollback_fail:
    mov [tmp_edit_error], eax
    cmp byte [tmp_edit_detached], 0
    je .rollback_allocated
    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ebx, [tmp_edit_tail]
    call fs_fat_write_entry
    mov byte [tmp_edit_detached], 0
.rollback_allocated:
    cmp byte [tmp_edit_allocated], 0
    je .rollback_done
    mov eax, [BUF_INODE + INODE_START_OFF]
    mov ecx, 1
    call fs_fat_free_chain
    mov byte [tmp_edit_allocated], 0
.rollback_done:
    mov eax, [tmp_edit_error]
    jmp .lookup_fail

.corrupt:
    mov eax, FS_ERR_CORRUPT
.lookup_fail:
    cmp byte [fs_mutation_active], 1
    jne .print_lookup_error
    call fs_complete_mutation
.print_lookup_error:
    call shell_print_fs_error
    pop esi
    ret

; ----------------------------
; Executable Runner
; ----------------------------
; IN: ESI = file path, EDI = optional arg2
shell_run:
    push esi
    push edi

    mov [tmp_run_path], esi
    mov [tmp_run_arg], edi

    mov ecx, APP_ARG0_CAP
    call string_fits_capacity
    test eax, eax
    jz near shell_run_arg_too_long
    mov esi, [tmp_run_arg]
    test esi, esi
    jz .args_valid
    mov ecx, APP_ARG1_CAP
    call string_fits_capacity
    test eax, eax
    jz near shell_run_arg_too_long
.args_valid:
    mov esi, [tmp_run_path]

    call fs_lookup_path
    cmp eax, FS_OK
    jl near shell_run_fs_error

    mov [tmp_inode_idx], eax
    mov edi, BUF_INODE
    call fs_read_inode
    cmp eax, FS_OK
    jl near shell_run_fs_error

    cmp byte [BUF_INODE + INODE_TYPE_OFF], 1
    jne near shell_run_not_file

    mov eax, [BUF_INODE + INODE_SIZE_OFF]
    cmp eax, 0
    je near shell_run_empty
    cmp eax, APP_IMAGE_SIZE
    ja near shell_run_too_large
    mov [tmp_run_size], eax

    add eax, 511
    jc near shell_run_too_large
    shr eax, 9
    cmp eax, [BUF_INODE + INODE_BLOCKS_OFF]
    jne near shell_run_bad_image
    cmp eax, APP_IMAGE_MAX_BLOCKS
    ja near shell_run_too_large
    mov [tmp_run_blocks], eax

    mov eax, [BUF_INODE + INODE_START_OFF]
    mov [tmp_run_block], eax
    mov ecx, [tmp_run_blocks]
    mov ebx, ecx
    dec ebx
    call fs_fat_get_nth_block
    cmp eax, FS_OK
    jl near shell_run_fs_error

    call platform_prepare_app_memory
    mov edi, APP_IMAGE_BASE
    mov ecx, APP_IMAGE_SIZE
    call zero_buffer
    mov dword [tmp_run_index], 0

shell_run_load_loop:
    mov ebx, [tmp_run_index]
    cmp ebx, [tmp_run_blocks]
    jge near shell_run_load_done

    mov eax, [tmp_run_block]
    add eax, FS_DATA_START_LBA

    mov edi, ebx
    shl edi, 9
    add edi, APP_IMAGE_BASE

    call ata_read_sector_lba28
    jc near shell_run_io_error

    inc dword [tmp_run_index]
    mov ebx, [tmp_run_index]
    cmp ebx, [tmp_run_blocks]
    jge near shell_run_load_done

    mov eax, [tmp_run_block]
    call fs_fat_read_entry
    cmp eax, FS_OK
    jl near shell_run_fs_error
    cmp eax, FS_FAT_EOC
    je near shell_run_bad_image
    mov [tmp_run_block], eax
    jmp near shell_run_load_loop

shell_run_load_done:
    ; A sector read also copies bytes beyond the logical end of a partial
    ; final block. Clear from the exact file end so trailing BSS is always
    ; zero even when those on-disk padding bytes contain old data.
    mov edi, APP_IMAGE_BASE
    add edi, [tmp_run_size]
    mov ecx, APP_IMAGE_SIZE
    sub ecx, [tmp_run_size]
    call zero_buffer

    pop edi                       ; tok_arg2
    pop esi                       ; tok_arg1

    ; The capacities were validated before loading the executable.
    mov edi, APP_ARG0_ADDR
    call copy_string

    ; Check tok_arg2
    mov edi, [tmp_run_arg]
    test edi, edi
    jz near .shell_run_argc1

    ; Copy argv[1] into the shared argument block.
    mov esi, edi
    mov edi, APP_ARG1_ADDR
    call copy_string

    ; Build the argv pointer array in the shared argument block.
    mov dword [APP_ARGV_ADDR], APP_ARG0_ADDR
    mov dword [APP_ARGV_ADDR + 4], APP_ARG1_ADDR
    mov dword [APP_ARGV_ADDR + 8], 0
    mov ecx, 2                    ; argc = 2
    jmp near .shell_run_setup_stack

.shell_run_argc1:
    mov dword [APP_ARGV_ADDR], APP_ARG0_ADDR
    mov dword [APP_ARGV_ADDR + 4], 0
    mov ecx, 1                    ; argc = 1

.shell_run_setup_stack:
    mov esi, msg_app_start
    call vga_print

    ; Save kernel stack
    mov [saved_kernel_esp], esp

    ; Switch to the bounded application stack.
    mov esp, APP_STACK_TOP

    ; Push arguments. The following call supplies [esp+0]=return address,
    ; leaving [esp+4]=argc and [esp+8]=argv as expected by crt0.
    push dword APP_ARGV_ADDR     ; argv pointer array
    push ecx                     ; argc

    ; Call the application at the shared image base.
    mov eax, APP_IMAGE_BASE
    call eax

return_to_shell:
    mov esp, [saved_kernel_esp]
    cld
    sti
    mov dword [current_brk], APP_HEAP_BASE
    call platform_verify_guards
    test eax, eax
    jnz .guards_ok
    mov esi, msg_layout_fatal
    call vga_print
    jmp kernel_layout_halt
.guards_ok:
    mov esi, msg_app_finished
    call vga_print
    ret

; IN: ESI=string, ECX=destination capacity including the terminator
; OUT: EAX=1 if the terminator is within capacity, otherwise 0
string_fits_capacity:
    push ecx
    push esi
.check:
    test ecx, ecx
    jz .no
    cmp byte [esi], 0
    je .yes
    inc esi
    dec ecx
    jmp .check
.yes:
    mov eax, 1
    jmp .done
.no:
    xor eax, eax
.done:
    pop esi
    pop ecx
    ret

shell_run_empty:
    mov esi, msg_empty
    call vga_print
    jmp shell_run_cleanup

shell_run_not_file:
    mov esi, msg_not_file
    call vga_print
    jmp shell_run_cleanup

shell_run_too_large:
    mov esi, msg_run_too_large
    call vga_print
    jmp shell_run_cleanup

shell_run_bad_image:
    mov esi, msg_run_bad_image
    call vga_print
    jmp shell_run_cleanup

shell_run_arg_too_long:
    mov esi, msg_run_arg_long
    call vga_print
    jmp shell_run_cleanup

shell_run_io_error:
    mov eax, FS_ERR_IO

shell_run_fs_error:
    call shell_print_fs_error
shell_run_cleanup:
    pop edi
    pop esi
    ret
