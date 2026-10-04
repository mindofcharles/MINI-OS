[bits 32]

RDRAND_CPUID_BIT equ 1 << 30
CPUID_ID_BIT equ 1 << 21
RDRAND_RETRY_LIMIT equ 10
RANDOM_REQUEST_MAX equ 1024

random_available db 0

; Detect CPUID before querying the RDRAND feature bit. This keeps the kernel
; valid on processors old enough not to implement CPUID at all.
random_initialize:
    pushad
    mov byte [random_available], 0
    pushfd
    pop eax
    mov edx, eax
    xor eax, CPUID_ID_BIT
    push eax
    popfd
    pushfd
    pop eax
    push edx
    popfd
    xor eax, edx
    test eax, CPUID_ID_BIT
    jz .done

    xor eax, eax
    cpuid
    cmp eax, 1
    jb .done

    mov eax, 1
    cpuid
    test ecx, RDRAND_CPUID_BIT
    jz .done
    mov byte [random_available], 1
.done:
    popad
    ret

; IN: EDI=destination, ECX=length from 1 through RANDOM_REQUEST_MAX
; OUT: CF clear after a complete fill; CF set after clearing the destination
random_fill:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov ebx, edi
    mov esi, ecx
    cmp byte [random_available], 1
    jne .failed

.word_loop:
    test ecx, ecx
    jz .success
    mov edx, RDRAND_RETRY_LIMIT
.retry:
    rdrand eax
    jc .sample_ready
    dec edx
    jnz .retry
    jmp .failed

.sample_ready:
    cmp ecx, 4
    jb .tail
    mov [edi], eax
    add edi, 4
    sub ecx, 4
    jmp .word_loop

.tail:
    mov [edi], al
    inc edi
    shr eax, 8
    dec ecx
    jnz .tail

.success:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    clc
    ret

.failed:
    mov edi, ebx
    mov ecx, esi
    xor eax, eax
    rep stosb
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    stc
    ret
