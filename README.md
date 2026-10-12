# OSp

OSp is an OS starter built from scratch: an original BIOS bootloader, a
freestanding 32-bit x86 kernel, and an interactive shell. It boots directly in
QEMU. The operating-system code uses no existing kernel, bootloader, C library,
or operating-system framework.

The kernel drives the VGA text screen, handles PS/2 keyboard interrupts, and
keeps time using the PC timer. It includes scrolling, a hardware cursor, line
editing, screen colors, exception diagnostics, reboot, and halt.
It also drives an RTL8139 Ethernet adapter and implements its own IPv4
network stack: ARP, DHCP, ICMP, UDP/DNS, and a small TCP/HTTP client.

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
The network tests use a local Ethernet fixture and need no Internet connection.
To also verify external DNS and fetch a real Internet page:

```sh
make test-network-live
```

This saves `build/network-live-console.log` and `build/osp-network-screen.png`.

## Shell

| Command | Action |
| --- | --- |
| `help` | List commands and editing keys |
| `about` | Describe the OS |
| `clear` | Clear the screen |
| `echo <text>` | Print text |
| `color <foreground> <background>` | Set two different colors, each a hex digit `0`–`f` |
| `uptime` | Show seconds since boot |
| `net` or `ifconfig` | Show adapter, link, IP configuration, and packet counters |
| `dhcp` | Obtain a network address from a DHCP server |
| `ifconfig <ip> <mask> <gateway> <dns>` | Set static IPv4 configuration |
| `ping <ip or hostname>` | Send an ICMP echo request |
| `nslookup <hostname>` | Resolve an IPv4 DNS address |
| `http <http://host[:port]/path>` | Fetch and display a plain HTTP response |
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

## Internet access

`make run` provides an emulated RTL8139 adapter connected to QEMU's user
network. OSp discovers the PCI device, programs its DMA buffers, and requests
an IP address automatically at boot. Give DHCP a few seconds, then try:

```text
osp> net
osp> ping 10.0.2.2
osp> nslookup example.com
osp> http http://example.com/
```

If automatic configuration fails, `dhcp` retries it. Static configuration is
also available, for example on the default QEMU network:

```text
osp> ifconfig 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3
```

All Ethernet and Internet protocol code runs inside OSp, without host socket
APIs, a host browser, or a host-side HTTP/DNS helper. QEMU still uses the host's
network connection to transport the virtual NIC's packets. A VM cannot reach
the physical network without an emulator backend or an assigned physical
adapter. The default user backend supplies a virtual router, DHCP, DNS
forwarding, and NAT; external ping may be blocked even when DNS and HTTP work.
See [QEMU networking](https://www.qemu.org/docs/master/system/devices/net.html).

To connect the guest to a LAN without QEMU's user-mode NAT, use an already
configured, bridged TAP device on a Linux host:

```sh
make run NET='tap,model=rtl8139,ifname=tap0,script=no,downscript=no'
```

That TAP device must already belong to a working network bridge and be
accessible to QEMU. The LAN's DHCP server supplies the guest's configuration.
Use `make run NET=none` to boot without an adapter.

The driver uses ordinary PCI configuration and RTL8139 port I/O and DMA. A
BIOS-compatible x86 PC with a supported RTL8139 adapter is the intended path
to operating without any host OS. Physical hardware remains unverified;
modern Ethernet adapters, Wi-Fi, Apple Silicon hardware, and UEFI boot require
additional drivers or boot support. The current Internet client supports IPv4
and plain HTTP, with one active request at a time. HTTPS/TLS and IPv6 are not
implemented.

The TCP client sends each small HTTP request in one segment and does not yet
negotiate the peer's MSS or implement receive-window flow control. Peers with
unusually small or closed receive windows can time out. The displayed response
is limited to 4 KiB, and requests have a bounded connection deadline.

## Start extending it

- `boot/boot.asm`: the 512-byte boot sector, disk reads, GDT, and protected-mode transition.
- `kernel/entry.asm`: stack setup and zeroing kernel runtime storage before C starts.
- `kernel/interrupts.asm`: exception and IRQ entry points, saved registers, and interrupt return.
- `kernel/kernel.c`: screen, keyboard, timer, interrupt setup, and command shell.
- `kernel/kernel.h`: kernel types and entry-point declarations.
- `kernel/rtl8139.c`: PCI discovery and Ethernet DMA driver.
- `kernel/network.c`: IP configuration and Internet protocols.
- `kernel/linker.ld`: physical memory layout.
- `tools/build.py`: the source-to-image build.
- `tools/test.py`: automated tests against the running image.
- `tools/test_network.py`: packet-level network tests against the running image.

Add a command in `execute_command()` in `kernel/kernel.c`, add its description
to `help`, and run `make test`. See [the architecture guide](docs/architecture.md)
for boot flow, memory, interrupt handling, and paths toward memory management,
a filesystem, and programs.

For a debugger, `make debug` starts QEMU paused with a GDB server on port 1234.
Load `build/kernel.elf` in an x86-capable GDB, run `target remote :1234`, set a
breakpoint at `kernel_main`, and continue.

## Current target and verification

The target is QEMU's BIOS PC machine with a floppy image, VGA text mode, and a
PS/2 keyboard and an optional PCI RTL8139 Ethernet adapter. Apple Silicon hosts
run it through QEMU's x86 CPU emulation.
BIOS firmware performs the initial disk reads and selects text mode; after the
protected-mode transition, the kernel handles its own input, output, and
interrupts. The assembler, compiler, linker, emulator, and BIOS are external
tools and firmware; all bootloader and kernel sources are in this repository.

`make test` verifies the image layout, actual VGA text and colors, CR0 protected
mode, shell commands, Shift symbols, editing across wrapped lines, input
bounds, scrolling, timer progress, reboot, halt, and a CPU exception diagnostic.
It also checks disk loading through the loader's full 64-sector capacity across
head and cylinder boundaries, using a second VM with 16 MiB RAM.
The networking suite checks actual Ethernet packets from the guest, including
DHCP, ARP, ICMP, DNS compression, TCP checksums and retransmission, segmented
HTTP responses, malformed input, receive-buffer wrapping, timeouts, link
recovery, and boot without an adapter.

Physical machines and UEFI boot are unverified. The current design has no
paging, dynamic allocation, filesystem, or user processes. These
are expansion points rather than hidden dependencies.

Hardware and emulator references:
[Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
[QEMU invocation](https://www.qemu.org/docs/master/system/qemu-manpage.html),
and [QEMU monitor](https://www.qemu.org/docs/master/system/monitor.html).
