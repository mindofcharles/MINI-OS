; ----------------------------
; ATA PIO (LBA28)
; ----------------------------
; IN: EAX=lba, EDI=destination buffer (512 bytes)
; OUT: CF clear on success, set on failure
ata_read_sector_lba28:
    push eax
    push ebx
    push ecx
    push edx
    push edi

    mov ebx, eax
    cmp ebx, 0x0FFFFFFF
    ja .fail
    call ata_wait_not_busy
    jc .fail

    mov dx, ATA_SECTOR_COUNT
    mov al, 1
    out dx, al

    mov dx, ATA_LBA_LOW
    mov al, bl
    out dx, al

    mov dx, ATA_LBA_MID
    mov al, bh
    out dx, al

    shr ebx, 16
    mov dx, ATA_LBA_HIGH
    mov al, bl
    out dx, al

    mov dx, ATA_DRIVE_HEAD
    mov al, bh
    and al, 0x0F
    or al, 0xE0
    out dx, al

    mov dx, ATA_COMMAND_STATUS
    mov al, ATA_CMD_READ
    out dx, al

    call ata_wait_drq
    jc .fail

    mov dx, ATA_DATA_PORT
    mov ecx, 256
    rep insw
    call ata_wait_not_busy
    jc .fail
    clc
    jmp .done

.fail:
    mov byte [fs_io_poisoned], 1
    stc
.done:
    pop edi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; IN: EAX=lba, ESI=source buffer (512 bytes)
; OUT: CF clear on success, set on failure
ata_write_sector_lba28:
    push eax
    push ebx
    push ecx
    push edx
    push esi

    cmp byte [fs_io_poisoned], 0
    jne .fail
    mov ebx, eax
    cmp ebx, 0x0FFFFFFF
    ja .fail
    call ata_wait_not_busy
    jc .fail

    mov dx, ATA_SECTOR_COUNT
    mov al, 1
    out dx, al

    mov dx, ATA_LBA_LOW
    mov al, bl
    out dx, al

    mov dx, ATA_LBA_MID
    mov al, bh
    out dx, al

    shr ebx, 16
    mov dx, ATA_LBA_HIGH
    mov al, bl
    out dx, al

    mov dx, ATA_DRIVE_HEAD
    mov al, bh
    and al, 0x0F
    or al, 0xE0
    out dx, al

    mov dx, ATA_COMMAND_STATUS
    mov al, ATA_CMD_WRITE
    out dx, al

    call ata_wait_drq
    jc .fail

    mov dx, ATA_DATA_PORT
    mov ecx, 256
    rep outsw

    mov dx, ATA_COMMAND_STATUS
    mov al, 0xE7
    out dx, al
    call ata_wait_not_busy
    jc .fail
    cmp byte [fs_mutation_active], 1
    jne .success
    mov byte [fs_mutation_touched], 1
.success:
    clc
    jmp .done

.fail:
    mov byte [fs_io_poisoned], 1
    stc
.done:
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

ata_wait_not_busy:
    push ecx
    push edx
    mov ecx, 0x100000
.wait:
    mov dx, ATA_COMMAND_STATUS
    in al, dx
    test al, 0x21
    jnz .fail
    test al, 0x80
    jz .ready
    loop .wait
    jmp .fail
.ready:
    clc
    jmp .done
.fail:
    stc
.done:
    pop edx
    pop ecx
    ret

ata_wait_drq:
    push ecx
    push edx
    mov ecx, 0x100000
.wait:
    mov dx, ATA_COMMAND_STATUS
    in al, dx
    test al, 0x21
    jnz .fail
    test al, 0x80
    jnz .cont
    test al, 0x08
    jnz .ready
.cont:
    loop .wait
    jmp .fail
.ready:
    clc
    jmp .done
.fail:
    stc
.done:
    pop edx
    pop ecx
    ret

; ----------------------------
; Keyboard (polling)
; ----------------------------
; IN: EDI = buffer, ECX = max length
; OUT: EAX = length (excluding terminator)
kbd_read_line:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    xor ebx, ebx
.loop:
    call kbd_read_char_blocking
    cmp al, 13
    je .enter
    cmp al, 8
    je .backspace
    cmp al, 0
    je .loop

    cmp ebx, ecx
    jge .loop

    mov [edi + ebx], al
    push eax
    call vga_putc
    pop eax
    inc ebx
    jmp .loop

.backspace:
    cmp ebx, 0
    je .loop
    dec ebx
    mov byte [edi + ebx], 0
    call vga_backspace
    jmp .loop

.enter:
    mov byte [edi + ebx], 0
    call vga_newline
    mov eax, ebx

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret

; IN: EDI = buffer, ECX = max length
; OUT: EAX = length (excluding terminator)
; Behavior: Enter inserts newline, ESC finishes editing.
kbd_read_text:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    xor ebx, ebx
.loop:
    call kbd_read_char_blocking
    cmp al, 27
    je .finish
    cmp al, 13
    je .enter
    cmp al, 8
    je .backspace
    cmp al, 0
    je .loop

    cmp ebx, ecx
    jge .loop
    mov [edi + ebx], al
    push eax
    call vga_putc
    pop eax
    inc ebx
    jmp .loop

.enter:
    cmp ebx, ecx
    jge .loop
    mov byte [edi + ebx], 10
    inc ebx
    call vga_newline
    jmp .loop

.backspace:
    cmp ebx, 0
    je .loop
    dec ebx
    cmp byte [edi + ebx], 10
    je .bs_newline
    mov byte [edi + ebx], 0
    call vga_backspace
    jmp .loop

.bs_newline:
    mov byte [edi + ebx], 0
    jmp .loop

.finish:
    mov byte [edi + ebx], 0
    call vga_newline
    mov eax, ebx

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret

kbd_shift_state db 0
kbd_ctrl_state db 0

; OUT: AL = ASCII char, 0 if unsupported/key release
kbd_read_char_blocking:
.wait_key:
    mov dx, KBD_STATUS_PORT
    in al, dx
    test al, 1
    jz .wait_key

    jmp kbd_read_pending_char

; OUT: AL = ASCII char, 0 if no raw byte is pending or it is unsupported
kbd_poll_char:
    mov dx, KBD_STATUS_PORT
    in al, dx
    test al, 1
    jnz kbd_read_pending_char
    xor al, al
    ret

kbd_read_pending_char:

    mov dx, KBD_DATA_PORT
    in al, dx

    ; Check Shift make/break codes
    cmp al, 0x2A        ; Left Shift press
    je .shift_on
    cmp al, 0x36        ; Right Shift press
    je .shift_on
    cmp al, 0xAA        ; Left Shift release
    je .shift_off
    cmp al, 0xB6        ; Right Shift release
    je .shift_off

    ; Check Ctrl make/break codes
    cmp al, 0x1D        ; Ctrl press
    je .ctrl_on
    cmp al, 0x9D        ; Ctrl release
    je .ctrl_off

    cmp al, 0x80
    jae .unsupported    ; Ignore other key releases (>= 0x80)

    ; Check Ctrl combinations
    cmp byte [kbd_ctrl_state], 1
    jne .not_ctrl_combo
    cmp al, 0x2E        ; Ctrl + C
    je .ctrl_c
    cmp al, 0x1F        ; Ctrl + S
    je .ctrl_s
    cmp al, 0x10        ; Ctrl + Q
    je .ctrl_q

.not_ctrl_combo:
    ; Arrow keys
    cmp al, 0x48        ; Up Arrow
    je .arrow_up
    cmp al, 0x50        ; Down Arrow
    je .arrow_down
    cmp al, 0x4B        ; Left Arrow
    je .arrow_left
    cmp al, 0x4D        ; Right Arrow
    je .arrow_right

    ; Special keys
    cmp al, 0x1C        ; Enter
    je .enter
    cmp al, 0x01        ; Esc
    je .esc
    cmp al, 0x0E        ; Backspace
    je .backspace
    cmp al, 0x53        ; Delete
    je .backspace
    cmp al, 0x39        ; Space
    je .space

    ; Number & Symbol row (0x02 to 0x0D)
    cmp al, 0x02
    jb .check_other_symbols
    cmp al, 0x0D
    jbe .num_symbol_row

.check_other_symbols:
    cmp al, 0x29        ; ` / ~
    je .tilde_key
    cmp al, 0x1A        ; [ / {
    je .lbrace_key
    cmp al, 0x1B        ; ] / }
    je .rbrace_key
    cmp al, 0x2B        ; \ / |
    je .backslash_key
    cmp al, 0x27        ; ; / :
    je .colon_key
    cmp al, 0x28        ; ' / "
    je .quote_key
    cmp al, 0x33        ; , / <
    je .comma_key
    cmp al, 0x34        ; . / >
    je .dot_key
    cmp al, 0x35        ; / / ?
    je .slash_key

    call kbd_map_letter
    test al, al
    jnz .done_char

.unsupported:
    xor al, al
    ret

.shift_on:
    mov byte [kbd_shift_state], 1
    xor al, al
    ret

.shift_off:
    mov byte [kbd_shift_state], 0
    xor al, al
    ret

.ctrl_on:
    mov byte [kbd_ctrl_state], 1
    xor al, al
    ret

.ctrl_off:
    mov byte [kbd_ctrl_state], 0
    xor al, al
    ret

.ctrl_c:
    mov al, 3
    ret

.ctrl_s:
    mov al, 19
    ret

.ctrl_q:
    mov al, 17
    ret

.arrow_up:
    mov al, 11
    ret

.arrow_down:
    mov al, 12
    ret

.arrow_left:
    mov al, 14
    ret

.arrow_right:
    mov al, 15
    ret

.enter:

    mov al, 13
    ret

.esc:
    mov al, 27
    ret

.backspace:
    mov al, 8
    ret

.space:
    mov al, ' '
    ret

.num_symbol_row:
    cmp byte [kbd_shift_state], 1
    je .num_symbol_shifted
    cmp al, 0x0B
    jbe .num_1_0
    cmp al, 0x0C
    je .minus_plain
    mov al, '='
    ret
.num_1_0:
    cmp al, 0x0B
    je .zero_plain
    add al, '1' - 0x02
    ret
.zero_plain:
    mov al, '0'
    ret
.minus_plain:
    mov al, '-'
    ret

.num_symbol_shifted:
    cmp al, 0x02
    je .shift_excl
    cmp al, 0x03
    je .shift_at
    cmp al, 0x04
    je .shift_hash
    cmp al, 0x05
    je .shift_dollar
    cmp al, 0x06
    je .shift_percent
    cmp al, 0x07
    je .shift_caret
    cmp al, 0x08
    je .shift_amp
    cmp al, 0x09
    je .shift_star
    cmp al, 0x0A
    je .shift_lparen
    cmp al, 0x0B
    je .shift_rparen
    cmp al, 0x0C
    je .shift_under
    mov al, '+'
    ret

.shift_excl:    mov al, '!'
                ret
.shift_at:      mov al, '@'
                ret
.shift_hash:    mov al, '#'
                ret
.shift_dollar:  mov al, '$'
                ret
.shift_percent: mov al, '%'
                ret
.shift_caret:   mov al, '^'
                ret
.shift_amp:     mov al, '&'
                ret
.shift_star:    mov al, '*'
                ret
.shift_lparen:  mov al, '('
                ret
.shift_rparen:  mov al, ')'
                ret
.shift_under:   mov al, '_'
                ret

.tilde_key:
    cmp byte [kbd_shift_state], 1
    je .ret_tilde
    mov al, '`'
    ret
.ret_tilde:     mov al, '~'
                ret

.lbrace_key:
    cmp byte [kbd_shift_state], 1
    je .ret_lbrace
    mov al, '['
    ret
.ret_lbrace:    mov al, '{'
                ret

.rbrace_key:
    cmp byte [kbd_shift_state], 1
    je .ret_rbrace
    mov al, ']'
    ret
.ret_rbrace:    mov al, '}'
                ret

.backslash_key:
    cmp byte [kbd_shift_state], 1
    je .ret_pipe
    mov al, '\'
    ret
.ret_pipe:      mov al, '|'
                ret

.colon_key:
    cmp byte [kbd_shift_state], 1
    je .ret_colon
    mov al, ';'
    ret
.ret_colon:     mov al, ':'
                ret

.quote_key:
    cmp byte [kbd_shift_state], 1
    je .ret_dquote
    mov al, "'"
    ret
.ret_dquote:    mov al, '"'
                ret

.comma_key:
    cmp byte [kbd_shift_state], 1
    je .ret_less
    mov al, ','
    ret
.ret_less:      mov al, '<'
                ret

.dot_key:
    cmp byte [kbd_shift_state], 1
    je .ret_greater
    mov al, '.'
    ret
.ret_greater:   mov al, '>'
                ret

.slash_key:
    cmp byte [kbd_shift_state], 1
    je .ret_question
    mov al, '/'
    ret
.ret_question:  mov al, '?'
                ret

.done_char:
    ret

kbd_map_letter:
    mov ah, [kbd_shift_state]
    cmp al, 0x1E
    je .a
    cmp al, 0x30
    je .b
    cmp al, 0x2E
    je .c
    cmp al, 0x20
    je .d
    cmp al, 0x12
    je .e
    cmp al, 0x21
    je .f
    cmp al, 0x22
    je .g
    cmp al, 0x23
    je .h
    cmp al, 0x17
    je .i
    cmp al, 0x24
    je .j
    cmp al, 0x25
    je .k
    cmp al, 0x26
    je .l
    cmp al, 0x32
    je .m
    cmp al, 0x31
    je .n
    cmp al, 0x18
    je .o
    cmp al, 0x19
    je .p
    cmp al, 0x10
    je .q
    cmp al, 0x13
    je .r
    cmp al, 0x1F
    je .s
    cmp al, 0x14
    je .t
    cmp al, 0x16
    je .u
    cmp al, 0x2F
    je .v
    cmp al, 0x11
    je .w
    cmp al, 0x2D
    je .x
    cmp al, 0x15
    je .y
    cmp al, 0x2C
    je .z
    xor al, al
    ret

.a: mov al, 'a'
    jmp .apply_shift
.b: mov al, 'b'
    jmp .apply_shift
.c: mov al, 'c'
    jmp .apply_shift
.d: mov al, 'd'
    jmp .apply_shift
.e: mov al, 'e'
    jmp .apply_shift
.f: mov al, 'f'
    jmp .apply_shift
.g: mov al, 'g'
    jmp .apply_shift
.h: mov al, 'h'
    jmp .apply_shift
.i: mov al, 'i'
    jmp .apply_shift
.j: mov al, 'j'
    jmp .apply_shift
.k: mov al, 'k'
    jmp .apply_shift
.l: mov al, 'l'
    jmp .apply_shift
.m: mov al, 'm'
    jmp .apply_shift
.n: mov al, 'n'
    jmp .apply_shift
.o: mov al, 'o'
    jmp .apply_shift
.p: mov al, 'p'
    jmp .apply_shift
.q: mov al, 'q'
    jmp .apply_shift
.r: mov al, 'r'
    jmp .apply_shift
.s: mov al, 's'
    jmp .apply_shift
.t: mov al, 't'
    jmp .apply_shift
.u: mov al, 'u'
    jmp .apply_shift
.v: mov al, 'v'
    jmp .apply_shift
.w: mov al, 'w'
    jmp .apply_shift
.x: mov al, 'x'
    jmp .apply_shift
.y: mov al, 'y'
    jmp .apply_shift
.z: mov al, 'z'
    jmp .apply_shift

.apply_shift:
    test ah, ah
    jz .no_shift
    sub al, 32
.no_shift:
    ret

; ----------------------------
; VGA text console
; ----------------------------
vga_clear:
    push eax
    push ecx
    push edi

    mov eax, (VGA_ATTR << 8) | ' '
    mov edi, VGA_BUFFER
    mov ecx, VGA_WIDTH * VGA_HEIGHT
    rep stosw

    mov dword [cursor_row], 0
    mov dword [cursor_col], 0
    call vga_sync_cursor

    pop edi
    pop ecx
    pop eax
    ret

; IN: AL=character
vga_putc:
    push eax
    push ebx
    push edx
    push edi

%ifdef ENABLE_DEBUGCON
    mov dx, 0xE9
    out dx, al
%endif

    cmp al, 10
    je near .newline
    cmp al, 8
    je near .backspace

    mov ebx, [cursor_row]
    imul ebx, VGA_WIDTH
    add ebx, [cursor_col]
    shl ebx, 1
    mov edi, VGA_BUFFER
    add edi, ebx

    mov ah, VGA_ATTR
    mov [edi], ax

    mov edx, [cursor_col]
    inc edx
    cmp edx, VGA_WIDTH
    jl near .store_col
    mov edx, 0
    mov ebx, [cursor_row]
    inc ebx
    cmp ebx, VGA_HEIGHT
    jl near .store_row
    call vga_scroll_up
    mov ebx, VGA_HEIGHT - 1
.store_row:
    mov [cursor_row], ebx
.store_col:
    mov [cursor_col], edx
    jmp near .done

.newline:
    call vga_newline
    jmp near .done

.backspace:
    call vga_backspace

.done:
    cmp byte [cursor_auto_sync], 1
    jne .skip_sync
    call vga_sync_cursor

.skip_sync:
    pop edi
    pop edx
    pop ebx
    pop eax
    ret

vga_backspace:
    push eax
    push ebx
    push edi

    mov ebx, [cursor_col]
    cmp ebx, 0
    jne .move_left

    mov eax, [cursor_row]
    cmp eax, 0
    je .done
    dec eax
    mov [cursor_row], eax
    mov ebx, VGA_WIDTH - 1
    jmp .erase

.move_left:
    dec ebx
.erase:
    mov [cursor_col], ebx

    mov eax, [cursor_row]
    imul eax, VGA_WIDTH
    add eax, ebx
    shl eax, 1
    mov edi, VGA_BUFFER
    add edi, eax
    mov word [edi], (VGA_ATTR << 8) | ' '

.done:
    call vga_sync_cursor
    pop edi
    pop ebx
    pop eax
    ret


vga_newline:
    push eax
    mov dword [cursor_col], 0
    mov eax, [cursor_row]
    inc eax
    cmp eax, VGA_HEIGHT
    jl .set_row
    call vga_scroll_up
    mov eax, VGA_HEIGHT - 1
.set_row:
    mov [cursor_row], eax
    call vga_sync_cursor
    pop eax
    ret

vga_scroll_up:
    push eax
    push ecx
    push esi
    push edi

    ; Move rows 1..24 to rows 0..23.
    mov esi, VGA_BUFFER + (VGA_WIDTH * 2)
    mov edi, VGA_BUFFER
    mov ecx, VGA_WIDTH * (VGA_HEIGHT - 1)
    rep movsw

    ; Clear last row.
    mov eax, (VGA_ATTR << 8) | ' '
    mov ecx, VGA_WIDTH
    rep stosw

    pop edi
    pop esi
    pop ecx
    pop eax
    ret

vga_sync_cursor:
    push eax
    push ebx
    push edx

    mov eax, [cursor_row]
    imul eax, VGA_WIDTH
    add eax, [cursor_col]
    mov ebx, eax

    mov dx, 0x3D4
    mov al, 0x0F
    out dx, al
    mov dx, 0x3D5
    mov al, bl
    out dx, al

    mov dx, 0x3D4
    mov al, 0x0E
    out dx, al
    mov dx, 0x3D5
    mov al, bh
    out dx, al

    pop edx
    pop ebx
    pop eax
    ret

; IN: ESI=zero-terminated string
vga_print:
    push eax
.loop:
    lodsb
    test al, al
    jz .done
    call vga_putc
    jmp .loop
.done:
    pop eax
    ret

; IN: ESI=string, ECX=length
vga_print_n:
    push eax
.loop:
    cmp ecx, 0
    je .done
    lodsb
    call vga_putc
    dec ecx
    jmp .loop
.done:
    pop eax
    ret

; IN: ESI=fixed-length name field (max 27), stops at 0
vga_print_name:
    push eax
    push ecx
    mov ecx, INODE_NAME_LEN
.loop:
    lodsb
    test al, al
    jz .done
    call vga_putc
    loop .loop
.done:
    pop ecx
    pop eax
    ret

cursor_auto_sync db 1
