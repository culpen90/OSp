# OSp architecture

## Boot flow

1. BIOS loads the first image sector at physical address `0x7c00`.
2. `boot/boot.asm` normalizes the code segment, establishes a stack, remembers
   the boot drive, and selects VGA mode 3.
3. It reads the kernel from sector 2 into `0x10000`. Reads use one sector at a
   time, three attempts per sector, and 1.44 MiB floppy geometry: 18 sectors per
   track and two heads. The builder calculates the sector count. A failed read
   produces a boot error and stops the CPU.
4. The loader enables the A20 gate, loads its global descriptor table, sets
   `CR0.PE`, and jumps into a flat 32-bit code segment. Code selector is `0x08`;
   data selector is `0x10`.
5. `kernel/entry.asm` sets the stack at `0x80000`, zeroes BSS, and calls
   `kernel_main()` with interrupts disabled. The linker puts this entry point
   at the start of the flat kernel binary.
6. The C kernel sets up VGA, its interrupt descriptor table, PIC, and PIT,
   then enables interrupts, initializes Ethernet, starts DHCP, and enters the shell.

The loaded kernel is limited to 64 sectors (32 KiB). This keeps disk reads
within a single 64 KiB DMA window. The build rejects an oversized kernel instead
of creating an image with truncated code. BSS is zeroed at runtime and does not
consume disk sectors. The linker separately checks that runtime kernel storage
ends before `0x70000`, leaving room below the stack.

## Memory

All segments have base zero. Paging is disabled, so the kernel's addresses are
physical addresses. Everything runs at privilege level zero.

| Address | Use |
| --- | --- |
| `0x7c00`–`0x7dff` | Boot sector, including its GDT |
| `0x10000` onward | Kernel text, constants, initialized data, then BSS |
| `0x70000`–`0x7ffff` | Space kept clear for the downward-growing kernel stack |
| `0x80000` | Initial stack pointer |
| `0xb8000`–`0xb8f9f` | 80×25 VGA text cells, character and attribute bytes |

There is no allocator yet. Kernel buffers and the IDT use static storage. The
memory layout relies on the emulated PC's conventional memory; a future memory
manager should collect the BIOS E820 map before leaving real mode.

## Interrupts

The IDT contains 256 present 32-bit interrupt gates. The first 32 point to CPU
exception stubs. The PIC is remapped to vectors 32–47; IRQ0 uses vector 32 and
IRQ1 uses vector 33. Only those two IRQs are unmasked. The PIT runs at
approximately 100 Hz.

The assembly entry points normalize the vector and CPU error code, preserve
general registers and data segments, clear the direction flag, align the stack,
and call `interrupt_dispatch(vector, error)`. Returning handlers restore the
original context and use `iretd`. Timer and keyboard handlers acknowledge the
master PIC. An exception prints its vector, name, and error code, disables
interrupts, and stops the CPU.

Keyboard IRQs translate PS/2 set-1 scancodes into characters. The target inherits
the emulated BIOS's keyboard/controller initialization and translation setting.
The handler tracks Shift, Caps Lock, control, extended scancodes, and Pause
sequences. A 256-byte ring buffer separates interrupt input from the shell;
when full, it drops new characters and preserves unread input. The consumer
checks the queue with interrupts disabled and uses `sti; hlt` when empty, avoiding
a lost interrupt between checking and sleeping.

The timer count is a 32-bit value. `uptime` reports whole seconds; the counter
wraps after approximately 497 days. It is a boot-relative clock, not wall time.

## Console and shell

VGA output uses two-byte cells directly. The kernel handles newline, row
wrapping, scrolling, cursor position, and deleting characters across a row
boundary. Changing colors updates existing cells as well as subsequent output.
VGA blinking is disabled so all 16 background colors are available.

The console mirrors emitted bytes to port `0xe9`, which QEMU can capture as a
debug transcript. Screen clearing, recoloring, and scrolling change video
memory without adding fictitious transcript lines. The tests check both the
transcript and actual VGA memory.

The shell uses a 128-byte stack buffer, including its trailing zero. It parses
one command at a time. There are no programs outside the kernel yet; each
command is a kernel function or branch in `execute_command()`.

## Build contract

NASM assembles the boot sector as a flat binary and the kernel entry points as
ELF32 objects. Clang targets `i686-none-elf` with freestanding compilation, no
PIC, no stack protector, no builtin library calls, and no SIMD or floating-point
instructions. Size optimization keeps code within the loader's disk budget.
LLD links at `0x10000`; Objcopy produces the flat kernel image. No host C library
is linked. A final link with unresolved runtime helpers fails the build.

Python prepends the signed 512-byte boot sector and zero-pads the image to
1,474,560 bytes. The image has no filesystem or partition table. Attach it as a
floppy drive, as `make run` does.

## Networking

`kernel/rtl8139.c` scans PCI configuration space using ports `0xcf8` and
`0xcfc` for Realtek vendor `0x10ec`, device `0x8139`. It enables I/O space and
bus mastering, obtains the I/O BAR, resets the adapter, reads its MAC address,
and programs static receive and transmit DMA buffers. Addresses are physical
because paging is disabled. The driver uses the RTL8139's original register
interface; C+ descriptor mode and other NIC models are unsupported.

The NIC interrupts remain masked. `network_poll()` drains a bounded number of
frames while the shell waits for keyboard input and while network commands
wait for replies. The PIT wakes the idle CPU every 10 ms. Protocol work stays
outside interrupt handlers, so neither network receive parsing nor DNS/HTTP
output runs inside keyboard or timer IRQs. Buffers are fixed-size BSS storage;
packet lengths are checked before reading headers or payloads.

`kernel/network.c` builds and parses Ethernet frames, ARP, IPv4, ICMP, UDP,
DHCP, DNS A records, and TCP. DHCP configuration comes from received packets,
not assumptions about the emulator's address range. Static configuration is
available for networks without DHCP. On-link destinations use ARP directly;
other destinations use the configured gateway. IP and transport checksums,
protocol lengths, and transaction identities are checked on receive. IPv4
fragment reassembly is unsupported, so fragmented datagrams are discarded.

The command API supports one operation at a time, with retries and bounded
deadlines. The TCP client has a small receive window and emits HTTP/1.0 GET
requests with `Connection: close`. This is a starter Internet client, without
TLS, a web browser, IPv6, or general-purpose socket/process APIs. See
[RFC 2131](https://www.rfc-editor.org/rfc/rfc2131) for DHCP and
[RFC 9293](https://www.rfc-editor.org/rfc/rfc9293) for TCP.
The client sends its small request as one segment; peer MSS negotiation and
receive-window flow control are expansion points. Such peers can time out
rather than complete a request. Response display is capped at 4 KiB.

The kernel never invokes the host's network APIs. QEMU's `-nic` backend
transports the guest's Ethernet packets and represents the attached network;
the default user backend also acts as a router and NAT. An existing bridged
TAP backend connects the same guest driver and stack to a LAN. Actual absence
of a host OS requires booting on supported physical hardware, which is not
yet verified. The guest stack is the same in both cases. See
[QEMU network emulation](https://www.qemu.org/docs/master/system/devices/net.html)
and the
[Realtek programming guide](https://www.cs.usfca.edu/~cruse/cs326f04/RTL8139_ProgrammersGuide.pdf).

## Expansion paths

Useful next changes, in dependency order:

1. Capture a BIOS memory map and introduce a physical-page allocator.
2. Add paging, a kernel heap, and explicit ownership of allocated memory.
3. Replace BIOS-only loading with a larger second-stage loader as the kernel
   outgrows the current 32 KiB disk budget.
4. Implement block-device I/O and a filesystem.
5. Add tasks, scheduling, user-mode segments, and system calls.

Keep interrupts bounded as the system grows. Leave allocation, command
execution, and slow I/O outside the keyboard and timer handlers. Extend the
emulator tests alongside changes to boot, memory, or interrupt behavior.
