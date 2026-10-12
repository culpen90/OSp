BITS 32

section .text
extern interrupt_dispatch
global irq0_stub
global irq1_stub
global unexpected_stub

; Give every exception the same stack layout: vector, error, EIP, CS, EFLAGS.
; All handlers run at CPL 0; a privilege-changing stack frame is not needed.
%macro ISR 1
isr_%1:
%if %1 != 8 && %1 != 10 && %1 != 11 && %1 != 12 && %1 != 13 && %1 != 14 && %1 != 17 && %1 != 21 && %1 != 29 && %1 != 30
    push dword 0
%endif
    push dword %1
    jmp interrupt_common
%endmacro

%assign vector 0
%rep 32
    ISR vector
%assign vector vector + 1
%endrep

irq0_stub:
    push dword 0
    push dword 32
    jmp interrupt_common

irq1_stub:
    push dword 0
    push dword 33
    jmp interrupt_common

unexpected_stub:
    push dword 0
    push dword 255
    jmp interrupt_common

interrupt_common:
    pushad

    ; Save segments as full dwords so frame offsets are independent of CPU
    ; behavior for 32-bit PUSH of a segment register.
    xor eax, eax
    mov ax, ds
    push eax
    mov ax, es
    push eax
    mov ax, fs
    push eax
    mov ax, gs
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld

    ; The saved frame has GS/FS/ES/DS (16 bytes), PUSHAD (32 bytes), then
    ; vector and error. EBP is preserved by cdecl and lets us restore the
    ; interrupted stack exactly even when an IRQ arrived at an unaligned ESP.
    mov ebp, esp
    and esp, -16
    sub esp, 8
    push dword [ebp + 52]         ; second argument: CPU error code
    push dword [ebp + 48]         ; first argument: vector
    call interrupt_dispatch
    mov esp, ebp

    pop eax
    mov gs, ax
    pop eax
    mov fs, ax
    pop eax
    mov es, ax
    pop eax
    mov ds, ax
    popad
    add esp, 8
    iretd

section .rodata
align 4
global isr_stub_table
isr_stub_table:
%assign vector 0
%rep 32
    dd isr_ %+ vector
%assign vector vector + 1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
