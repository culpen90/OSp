# OSp

OSp is an OS starter built from scratch: an original BIOS bootloader, a
freestanding 32-bit x86 kernel, and an interactive shell. It boots directly in
QEMU. The operating-system code uses no existing kernel, bootloader, C library,
or operating-system framework.

The kernel drives the VGA text screen, handles PS/2 keyboard interrupts, and
keeps time using the PC timer. It includes scrolling, a hardware cursor, line
editing, screen colors, exception diagnostics, reboot, and halt.

## Build and run

Required tools: Python 3, Make, NASM, Clang, LLD (`ld.lld`), LLVM Objcopy
(`llvm-objcopy`), and QEMU (`qemu-system-i386`).

```sh
make run
```

This builds `build/osp.img` and opens the OS in a QEMU window. Click inside the
window and type `help`. Close the QEMU window to leave the emulator. If QEMU
captures the pointer, Ctrl+Alt+G releases it; macOS uses Control+Option+G.

To install the tools on macOS:

```sh
brew install nasm qemu llvm
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

On Debian or Ubuntu:

```sh
sudo apt install make python3 nasm clang lld llvm qemu-system-x86
```

Build without opening an emulator, or run the automated emulator checks:

```sh
make
make test
```

The build creates the boot sector, kernel ELF with debug symbols, flat kernel
binary, linker map, a 1.44 MiB bootable floppy image, and a JSON manifest with an
image checksum. `make test` also saves `build/osp-screen.png` and
`build/test-console.log`. Tool paths can be overridden with `NASM`, `CLANG`,
`LD_LLD`, and `OBJCOPY` environment variables; Make accepts `QEMU` and `PYTHON`.

## Shell

| Command | Action |
| --- | --- |
| `help` | List commands and editing keys |
| `about` | Describe the OS |
| `clear` | Clear the screen |
| `echo <text>` | Print text |
| `color <foreground> <background>` | Set two different colors, each a hex digit `0`–`f` |
| `uptime` | Show seconds since boot |
| `reboot` | Restart the emulated machine |
| `halt` | Stop the CPU until the machine is restarted |

Command names are lowercase. The keyboard uses a US layout; Shift and Caps Lock
work. Backspace deletes one character, and Ctrl+U clears the input line. Each
line holds up to 127 characters; extra characters are ignored until you edit or
submit it. Arrow keys and command history are not implemented yet.

For example:

```text
osp> echo Hello from my OS
Hello from my OS
osp> color a 0
Colors updated.
osp> uptime
Uptime: 12 seconds.
```

## Start extending it

- `boot/boot.asm`: the 512-byte boot sector, disk reads, GDT, and protected-mode transition.
- `kernel/entry.asm`: stack setup and zeroing kernel runtime storage before C starts.
- `kernel/interrupts.asm`: exception and IRQ entry points, saved registers, and interrupt return.
- `kernel/kernel.c`: screen, keyboard, timer, interrupt setup, and command shell.
- `kernel/kernel.h`: kernel types and entry-point declarations.
- `kernel/linker.ld`: physical memory layout.
- `tools/build.py`: the source-to-image build.
- `tools/test.py`: automated tests against the running image.

Add a command in `execute_command()` in `kernel/kernel.c`, add its description
to `help`, and run `make test`. See [the architecture guide](docs/architecture.md)
for boot flow, memory, interrupt handling, and paths toward memory management,
a filesystem, and programs.

For a debugger, `make debug` starts QEMU paused with a GDB server on port 1234.
Load `build/kernel.elf` in an x86-capable GDB, run `target remote :1234`, set a
breakpoint at `kernel_main`, and continue.

## Current target and verification

The target is QEMU's BIOS PC machine with a floppy image, VGA text mode, and a
PS/2 keyboard. Apple Silicon hosts run it through QEMU's x86 CPU emulation.
BIOS firmware performs the initial disk reads and selects text mode; after the
protected-mode transition, the kernel handles its own input, output, and
interrupts. The assembler, compiler, linker, emulator, and BIOS are external
tools and firmware; all bootloader and kernel sources are in this repository.

`make test` verifies the image layout, actual VGA text and colors, CR0 protected
mode, shell commands, Shift symbols, editing across wrapped lines, input
bounds, scrolling, timer progress, reboot, halt, and a CPU exception diagnostic.
It also checks disk loading through the loader's full 64-sector capacity across
head and cylinder boundaries, using a second VM with 16 MiB RAM.

Physical machines and UEFI boot are unverified. The current design has no
paging, dynamic allocation, filesystem, user processes, or networking. These
are expansion points rather than hidden dependencies.

Hardware and emulator references:
[Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
[QEMU invocation](https://www.qemu.org/docs/master/system/qemu-manpage.html),
and [QEMU monitor](https://www.qemu.org/docs/master/system/monitor.html).
