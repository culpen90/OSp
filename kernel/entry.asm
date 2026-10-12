BITS 32

section .text.entry
global _start
extern kernel_main
extern __bss_start
extern __bss_end

; The BIOS loader enters here with flat code selector 0x08 and interrupts off.
_start:
    cli
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x80000
    xor ebp, ebp
    cld

    ; BSS is not stored in the disk image. Clear every byte before entering C.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; ESP is 16-byte aligned immediately before the call, as required by C.
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
