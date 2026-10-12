; OSp's first sector. BIOS loads this at physical 0x7c00.
; The rest of the image contains our kernel, starting at floppy sector 2.
bits 16
org 0x7c00

%ifndef KERNEL_SECTORS
    %error "The build must provide KERNEL_SECTORS"
%endif
%if KERNEL_SECTORS < 1 || KERNEL_SECTORS > 64
    %error "Kernel must occupy between 1 and 64 sectors"
%endif

    jmp 0x0000:start

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    cld
    mov [boot_drive], dl
    sti

    ; The loader deliberately uses 1.44 MiB floppy geometry.
    cmp dl, 2
    jae wrong_drive

    mov ax, 0x0003             ; BIOS establishes 80x25 VGA text mode.
    int 0x10

next_sector:
    mov byte [retries], 3
read_sector:
    mov ax, 0x1000
    mov es, ax
    mov bx, [read_offset]
    mov ch, [cylinder]
    mov cl, [sector]
    mov dh, [head]
    mov dl, [boot_drive]
    mov ax, 0x0201             ; One sector avoids track/DMA boundary issues.
    int 0x13
    jnc sector_loaded

    xor ax, ax                ; Reset the disk before another attempt.
    mov dl, [boot_drive]
    int 0x13
    dec byte [retries]
    jnz read_sector
    mov si, disk_error
    jmp fatal

sector_loaded:
    add word [read_offset], 512
    dec word [sectors_left]
    jz enter_kernel
    inc byte [sector]
    cmp byte [sector], 19
    jb next_sector
    mov byte [sector], 1
    xor byte [head], 1
    jnz next_sector
    inc byte [cylinder]
    jmp next_sector

enter_kernel:
    cli
    ; Fast A20 gate. All current kernel storage is below 1 MiB, too.
    in al, 0x92
    or al, 2
    and al, 0xfe               ; Do not activate the fast-reset bit.
    out 0x92, al
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1                 ; CR0.PE: enter protected mode.
    mov cr0, eax
    jmp dword 0x08:protected_mode

wrong_drive:
    mov si, drive_error
fatal:
    lodsb
    test al, al
    jz stopped
    out 0xe9, al              ; Emulator debug output; harmless on our target.
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    jmp fatal
stopped:
    cli
    hlt
    jmp stopped

align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff      ; Selector 0x08: flat, 32-bit executable segment.
    dq 0x00cf92000000ffff      ; Selector 0x10: flat, writable data segment.
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt - 1
    dd gdt

bits 32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x80000
    jmp 0x08:0x10000

bits 16
boot_drive:   db 0
retries:      db 0
cylinder:     db 0
head:         db 0
sector:       db 2
read_offset:  dw 0
sectors_left: dw KERNEL_SECTORS
disk_error:   db 'OSp: disk read failed.', 0
drive_error:  db 'OSp: boot as a floppy image.', 0

times 510 - ($ - $$) db 0
dw 0xaa55
