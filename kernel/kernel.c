#include "kernel.h"
#include "network.h"

_Static_assert(sizeof(u8) == 1, "u8 must be one byte");
_Static_assert(sizeof(u16) == 2, "u16 must be two bytes");
_Static_assert(sizeof(u32) == 4, "u32 must be four bytes");

enum {
    SCREEN_WIDTH = 80,
    SCREEN_HEIGHT = 25,
    INPUT_LIMIT = 127,
    KEY_QUEUE_SIZE = 256,
    TIMER_HZ = 100,
    PIC_COMMAND = 0x20,
    PIC_DATA = 0x21,
    PIC_SLAVE_COMMAND = 0xa0,
    PIC_SLAVE_DATA = 0xa1
};

/* These entry points preserve the interrupted registers and pass cdecl args. */
extern void (*isr_stub_table[32])(void);
extern void irq0_stub(void);
extern void irq1_stub(void);
extern void unexpected_stub(void);

struct idt_gate {
    u16 address_low;
    u16 selector;
    u8 reserved;
    u8 flags;
    u16 address_high;
} __attribute__((packed));

struct idt_pointer {
    u16 limit;
    u32 address;
} __attribute__((packed));

static struct idt_gate idt[256];
static volatile u16 *const screen = (volatile u16 *)0xb8000;
static u32 cursor_row;
static u32 cursor_column;
static u8 console_color = 0x0f;
static volatile u32 timer_ticks;

/* IRQ1 is the sole producer; the shell is the sole consumer. */
static volatile u8 key_queue[KEY_QUEUE_SIZE];
static volatile u32 queue_head;
static volatile u32 queue_tail;
static u8 left_shift;
static u8 right_shift;
static u8 left_control;
static u8 right_control;
static u8 caps_lock;
static u8 extended_key;
static u8 pause_bytes;

static inline void out_byte(u16 port, u8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 in_byte(u16 port)
{
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void)
{
    out_byte(0x80, 0);
}

static void update_cursor(void)
{
    u16 position = (u16)(cursor_row * SCREEN_WIDTH + cursor_column);
    out_byte(0x3d4, 0x0f);
    out_byte(0x3d5, (u8)position);
    out_byte(0x3d4, 0x0e);
    out_byte(0x3d5, (u8)(position >> 8));
}

static u16 screen_cell(u8 character)
{
    return (u16)((u16)console_color << 8) | character;
}

static void scroll_if_needed(void)
{
    if (cursor_row < SCREEN_HEIGHT)
        return;

    for (u32 i = 0; i < SCREEN_WIDTH * (SCREEN_HEIGHT - 1); ++i)
        screen[i] = screen[i + SCREEN_WIDTH];
    for (u32 i = SCREEN_WIDTH * (SCREEN_HEIGHT - 1);
         i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i)
        screen[i] = screen_cell(' ');
    cursor_row = SCREEN_HEIGHT - 1;
}

static void console_clear(void)
{
    for (u32 i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i)
        screen[i] = screen_cell(' ');
    cursor_row = 0;
    cursor_column = 0;
    update_cursor();
}

static void console_character(char character)
{
    /* Port E9 provides an observable transcript without a serial driver. */
    out_byte(0xe9, (u8)character);

    if (character == '\n') {
        cursor_column = 0;
        ++cursor_row;
    } else if (character == '\r') {
        cursor_column = 0;
    } else if (character >= ' ') {
        screen[cursor_row * SCREEN_WIDTH + cursor_column] =
            screen_cell((u8)character);
        ++cursor_column;
        if (cursor_column == SCREEN_WIDTH) {
            cursor_column = 0;
            ++cursor_row;
        }
    }
    scroll_if_needed();
    update_cursor();
}

static void console_write(const char *text)
{
    while (*text)
        console_character(*text++);
}

static void console_delete(void)
{
    if (cursor_column != 0) {
        --cursor_column;
    } else if (cursor_row != 0) {
        --cursor_row;
        cursor_column = SCREEN_WIDTH - 1;
    } else {
        return;
    }
    screen[cursor_row * SCREEN_WIDTH + cursor_column] = screen_cell(' ');
    update_cursor();
    out_byte(0xe9, '\b');
    out_byte(0xe9, ' ');
    out_byte(0xe9, '\b');
}

static void console_decimal(u32 value)
{
    char digits[10];
    u32 count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
        console_character(digits[--count]);
}

/* The network stack runs in the shell context, with timer/keyboard IRQs active. */
void net_write(const char *text) { console_write(text); }
void net_putc(char character) { console_character(character); }
void net_number(u32 value) { console_decimal(value); }
u32 net_ticks(void) { return timer_ticks; }
void net_idle(void) { __asm__ volatile("sti; hlt" : : : "memory"); }

static void console_hex(u32 value)
{
    static const char digits[] = "0123456789abcdef";
    console_write("0x");
    for (u32 i = 8; i != 0; --i)
        console_character(digits[(value >> ((i - 1) * 4)) & 0xf]);
}

static void enable_full_background_colors(void)
{
    /* VGA's attribute register uses a flip-flop reset by a status read. */
    (void)in_byte(0x3da);
    out_byte(0x3c0, 0x30);
    u8 mode = in_byte(0x3c1);
    (void)in_byte(0x3da);
    out_byte(0x3c0, 0x30);
    out_byte(0x3c0, (u8)(mode & ~0x08));
    (void)in_byte(0x3da);
    out_byte(0x3c0, 0x20);
}

static void console_set_color(u8 foreground, u8 background)
{
    console_color = (u8)((background << 4) | foreground);
    for (u32 i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i)
        screen[i] = (u16)(screen[i] & 0xff) | (u16)(console_color << 8);
}

static void idt_set(u32 vector, void (*entry)(void))
{
    u32 address = (u32)entry;
    idt[vector].address_low = (u16)address;
    idt[vector].selector = 0x08;
    idt[vector].reserved = 0;
    idt[vector].flags = 0x8e; /* Present, ring 0, 32-bit interrupt gate. */
    idt[vector].address_high = (u16)(address >> 16);
}

static void interrupts_initialize(void)
{
    for (u32 i = 0; i < 256; ++i)
        idt_set(i, unexpected_stub);
    for (u32 i = 0; i < 32; ++i)
        idt_set(i, isr_stub_table[i]);
    idt_set(32, irq0_stub);
    idt_set(33, irq1_stub);

    struct idt_pointer descriptor = {
        (u16)(sizeof(idt) - 1), (u32)idt
    };
    __asm__ volatile("lidt %0" : : "m"(descriptor) : "memory");

    /* Move the legacy PIC IRQs away from the CPU exception vectors. */
    out_byte(PIC_COMMAND, 0x11);
    io_wait();
    out_byte(PIC_SLAVE_COMMAND, 0x11);
    io_wait();
    out_byte(PIC_DATA, 32);
    io_wait();
    out_byte(PIC_SLAVE_DATA, 40);
    io_wait();
    out_byte(PIC_DATA, 0x04);
    io_wait();
    out_byte(PIC_SLAVE_DATA, 0x02);
    io_wait();
    out_byte(PIC_DATA, 0x01);
    io_wait();
    out_byte(PIC_SLAVE_DATA, 0x01);
    io_wait();
    out_byte(PIC_DATA, 0xfc); /* Only timer and keyboard are unmasked. */
    out_byte(PIC_SLAVE_DATA, 0xff);

    u16 divisor = (u16)(1193182 / TIMER_HZ);
    out_byte(0x43, 0x36); /* PIT channel 0, square wave, low/high divisor. */
    out_byte(0x40, (u8)divisor);
    out_byte(0x40, (u8)(divisor >> 8));
}

static void queue_character(u8 character)
{
    u32 next = (queue_head + 1) & (KEY_QUEUE_SIZE - 1);
    if (next == queue_tail)
        return; /* Keep unread input intact when the queue is full. */
    key_queue[queue_head] = character;
    queue_head = next;
}

static void keyboard_interrupt(void)
{
    u8 status = in_byte(0x64);
    if ((status & 1) == 0)
        return;
    u8 scan = in_byte(0x60);
    if (status & 0xe0)
        return; /* Ignore mouse bytes and controller parity/timeout errors. */

    if (pause_bytes != 0) {
        --pause_bytes;
        return;
    }
    if (scan == 0xe1) {
        pause_bytes = 5;
        return;
    }
    if (scan == 0xe0) {
        extended_key = 1;
        return;
    }

    u8 released = (u8)(scan & 0x80);
    u8 key = (u8)(scan & 0x7f);
    if (extended_key) {
        extended_key = 0;
        if (key == 0x1d)
            right_control = (u8)!released;
        else if (!released && key == 0x1c)
            queue_character('\n');
        return;
    }

    if (key == 0x2a) {
        left_shift = (u8)!released;
        return;
    }
    if (key == 0x36) {
        right_shift = (u8)!released;
        return;
    }
    if (key == 0x1d) {
        left_control = (u8)!released;
        return;
    }
    if (released)
        return;
    if (key == 0x3a) {
        caps_lock ^= 1;
        return;
    }
    if (key == 0x0e) {
        queue_character('\b');
        return;
    }
    if (key == 0x1c) {
        queue_character('\n');
        return;
    }

    /* PS/2 set 1 positions, using a US keyboard layout. */
    static const char plain[128] = {
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
        [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
        [0x0a] = '9', [0x0b] = '0', [0x0c] = '-', [0x0d] = '=',
        [0x0f] = ' ', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
        [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u',
        [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1a] = '[',
        [0x1b] = ']', [0x1e] = 'a', [0x1f] = 's', [0x20] = 'd',
        [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j',
        [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'',
        [0x29] = '`', [0x2b] = '\\', [0x2c] = 'z', [0x2d] = 'x',
        [0x2e] = 'c', [0x2f] = 'v', [0x30] = 'b', [0x31] = 'n',
        [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
        [0x37] = '*', [0x39] = ' ', [0x4a] = '-', [0x4e] = '+'
    };
    static const char shifted[128] = {
        [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
        [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
        [0x0a] = '(', [0x0b] = ')', [0x0c] = '_', [0x0d] = '+',
        [0x1a] = '{', [0x1b] = '}', [0x27] = ':', [0x28] = '"',
        [0x29] = '~', [0x2b] = '|', [0x33] = '<', [0x34] = '>',
        [0x35] = '?'
    };
    char character = plain[key];
    if (!character)
        return;

    u8 shift = (u8)(left_shift || right_shift);
    if (character >= 'a' && character <= 'z') {
        if (left_control || right_control) {
            if (character == 'u')
                queue_character(0x15);
            return;
        }
        if (shift ^ caps_lock)
            character = (char)(character - 'a' + 'A');
    } else if (shift && shifted[key]) {
        character = shifted[key];
    }
    queue_character((u8)character);
}

__attribute__((noreturn)) static void stop_cpu(void)
{
    __asm__ volatile("cli" : : : "memory");
    for (;;)
        __asm__ volatile("hlt");
}

void interrupt_dispatch(u32 vector, u32 error)
{
    if (vector == 32) {
        ++timer_ticks;
        out_byte(PIC_COMMAND, 0x20);
        return;
    }
    if (vector == 33) {
        keyboard_interrupt();
        out_byte(PIC_COMMAND, 0x20);
        return;
    }

    static const char *const exceptions[32] = {
        "divide error", "debug", "non-maskable interrupt", "breakpoint",
        "overflow", "bound range", "invalid opcode", "device unavailable",
        "double fault", "reserved", "invalid TSS", "segment unavailable",
        "stack fault", "general protection", "page fault", "reserved",
        "x87 floating point", "alignment check", "machine check", "SIMD",
        "virtualization", "control protection", "reserved", "reserved",
        "reserved", "reserved", "reserved", "reserved",
        "hypervisor injection", "VMM communication", "security", "reserved"
    };
    console_set_color(15, 4);
    console_write("\nOSp stopped: interrupt ");
    console_decimal(vector);
    if (vector < 32) {
        console_write(" (");
        console_write(exceptions[vector]);
        console_character(')');
    }
    console_write(", error=");
    console_hex(error);
    console_character('\n');
    stop_cpu();
}

static u8 next_character(void)
{
    for (;;) {
        network_poll();
        __asm__ volatile("cli" : : : "memory");
        if (queue_tail != queue_head) {
            u8 character = key_queue[queue_tail];
            queue_tail = (queue_tail + 1) & (KEY_QUEUE_SIZE - 1);
            __asm__ volatile("sti" : : : "memory");
            return character;
        }
        /* STI delays interrupts until HLT: no wakeup can be lost here. */
        __asm__ volatile("sti; hlt" : : : "memory");
    }
}

static int same_text(const char *left, const char *right)
{
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static int is_space(char character)
{
    return character == ' ' || character == '\t';
}

static char *skip_spaces(char *text)
{
    while (is_space(*text))
        ++text;
    return text;
}

static int hex_digit(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static int command_takes_no_arguments(const char *command)
{
    static const char *const commands[] = {
        "help", "about", "clear", "uptime", "reboot", "halt", "net", "dhcp"
    };
    for (u32 i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (same_text(command, commands[i]))
            return 1;
    return 0;
}

static void command_color(char *arguments)
{
    int foreground = hex_digit(*arguments);
    if (foreground < 0 || !is_space(arguments[1])) {
        console_write("Usage: color <foreground 0-f> <background 0-f>\n");
        return;
    }
    arguments = skip_spaces(arguments + 1);
    int background = hex_digit(*arguments);
    if (background < 0 || *skip_spaces(arguments + 1) != '\0') {
        console_write("Usage: color <foreground 0-f> <background 0-f>\n");
        return;
    }
    if (foreground == background) {
        console_write("Choose different foreground and background colors.\n");
        return;
    }
    console_set_color((u8)foreground, (u8)background);
    console_write("Colors updated.\n");
}

__attribute__((noreturn)) static void command_reboot(void)
{
    console_write("Restarting OSp...\n");
    __asm__ volatile("cli" : : : "memory");
    for (u32 attempt = 0; attempt < 100000; ++attempt) {
        if ((in_byte(0x64) & 2) == 0) {
            out_byte(0x64, 0xfe);
            break;
        }
    }
    /* If the controller cannot reset, an empty IDT forces a CPU reset. */
    struct idt_pointer empty_idt = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(empty_idt) : "memory");
    stop_cpu();
}

static void execute_command(char *line)
{
    char *command = skip_spaces(line);
    if (!*command)
        return;
    char *arguments = command;
    while (*arguments && !is_space(*arguments))
        ++arguments;
    if (*arguments) {
        *arguments++ = '\0';
        arguments = skip_spaces(arguments);
    }

    if (same_text(command, "echo")) {
        console_write(arguments);
        console_character('\n');
    } else if (same_text(command, "color")) {
        command_color(arguments);
    } else if (same_text(command, "ifconfig")) {
        if (*arguments)
            network_configure(arguments);
        else
            network_status();
    } else if (same_text(command, "ping")) {
        network_ping(arguments);
    } else if (same_text(command, "nslookup")) {
        network_dns(arguments);
    } else if (same_text(command, "http")) {
        network_http(arguments);
    } else if (*arguments && command_takes_no_arguments(command)) {
        console_write("That command does not take arguments. Type help.\n");
    } else if (same_text(command, "help")) {
        console_write("Commands:\n"
                      "  help                Show this list\n"
                      "  about               Describe OSp\n"
                      "  clear               Clear the screen\n"
                      "  echo <text>         Print text\n"
                      "  color <fg> <bg>     Set colors, each 0-f\n"
                      "  uptime              Show time since boot\n"
                      "  net                 Show Ethernet and IP status\n"
                      "  dhcp                Obtain an address automatically\n"
                      "  ifconfig <ip> <mask> <gateway> <dns>   Set static IPv4\n"
                      "  ping <ip or host>   Send an ICMP echo\n"
                      "  nslookup <host>     Resolve a DNS name\n"
                      "  http <http://url>   Fetch a plain HTTP page\n"
                      "  reboot              Restart the machine\n"
                      "  halt                Stop the CPU\n"
                      "Editing: Backspace removes a character; Ctrl+U clears a line.\n");
    } else if (same_text(command, "about")) {
        console_write("OSp is an OS starter with an original BIOS bootloader and kernel.\n"
                      "32-bit x86, VGA text console, timer interrupts, and PS/2 keyboard.\n"
                      "Own RTL8139 Ethernet driver and IPv4 network stack.\n"
                      "The freestanding kernel runs directly on the CPU.\n");
    } else if (same_text(command, "clear")) {
        console_clear();
    } else if (same_text(command, "uptime")) {
        u32 ticks = timer_ticks;
        console_write("Uptime: ");
        console_decimal(ticks / TIMER_HZ);
        console_write(" seconds.\n");
    } else if (same_text(command, "net")) {
        network_status();
    } else if (same_text(command, "dhcp")) {
        network_dhcp();
    } else if (same_text(command, "reboot")) {
        command_reboot();
    } else if (same_text(command, "halt")) {
        console_write("OSp halted. Restart the machine to continue.\n");
        stop_cpu();
    } else {
        console_write("Unknown command: ");
        console_write(command);
        console_write(". Type help.\n");
    }
}

void kernel_main(void)
{
    __asm__ volatile("cli" : : : "memory");
    enable_full_background_colors();
    console_clear();
    interrupts_initialize();
    console_write("OSp\n"
                  "An OS starter built from an original BIOS bootloader and kernel.\n"
                  "Type help to explore the commands.\n\n");
    __asm__ volatile("sti" : : : "memory");
    network_initialize();

    char line[INPUT_LIMIT + 1];
    for (;;) {
        u32 length = 0;
        console_write("osp> ");
        for (;;) {
            u8 character = next_character();
            if (character == '\n') {
                console_character('\n');
                line[length] = '\0';
                execute_command(line);
                break;
            }
            if (character == '\b') {
                if (length != 0) {
                    --length;
                    console_delete();
                }
            } else if (character == 0x15) {
                while (length != 0) {
                    --length;
                    console_delete();
                }
            } else if (character >= ' ' && character <= '~' && length < INPUT_LIMIT) {
                line[length++] = (char)character;
                console_character((char)character);
            }
        }
    }
}
