#include "rtl8139.h"

/* Register layout and DMA ownership follow the RTL8139(A/B) programming
 * guide. Packet buffers must remain resident and physically contiguous.
 * https://www.cs.usfca.edu/~cruse/cs326f04/RTL8139_ProgrammersGuide.pdf */
enum {
    PCI_ADDRESS = 0xcf8,
    PCI_DATA = 0xcfc,
    RX_RING_SIZE = 8192,
    RX_PADDING = 16 + 2048,
    FRAME_MIN = 60,
    FRAME_MAX = 1514,
    TX_BUFFER_SIZE = 1536,
    TX_DESCRIPTOR_COUNT = 4,
    RESET_POLL_LIMIT = 100000,
    RX_DISCARD_LIMIT = 16,

    REG_TX_STATUS = 0x10,
    REG_TX_ADDRESS = 0x20,
    REG_RX_ADDRESS = 0x30,
    REG_COMMAND = 0x37,
    REG_RX_READ = 0x38,
    REG_INTERRUPT_MASK = 0x3c,
    REG_INTERRUPT_STATUS = 0x3e,
    REG_TX_CONFIG = 0x40,
    REG_RX_CONFIG = 0x44,
    REG_RX_MISSED = 0x4c,
    REG_CONFIG_LOCK = 0x50,
    REG_CONFIG1 = 0x52,
    REG_MEDIA_STATUS = 0x58,

    COMMAND_RESET = 0x10,
    COMMAND_RX = 0x08,
    COMMAND_TX = 0x04,
    COMMAND_RX_EMPTY = 0x01,
    INTERRUPT_RX_OK = 0x0001,
    INTERRUPT_RX_ERROR = 0x0002,
    INTERRUPT_TX_OK = 0x0004,
    INTERRUPT_TX_ERROR = 0x0008,
    INTERRUPT_RX_OVERFLOW = 0x0010,
    INTERRUPT_RX_FIFO = 0x0040,
    INTERRUPT_PCI_ERROR = 0x8000,
    RX_OK = 0x0001,
    RX_ERRORS = 0x003e,
    TX_HOST_OWNS = 0x00002000,
    TX_UNDERRUN = 0x00004000,
    TX_OK = 0x00008000,
    TX_COLLISION = 0x20000000,
    TX_ABORT = 0x40000000,
    TX_CARRIER = 0x80000000U,

    /* Standard interframe gap, 1024-byte DMA bursts, hardware CRC. */
    TX_CONFIGURATION = (3U << 24) | (6U << 8),
    /* 512-byte FIFO threshold and 1024-byte DMA bursts. Bit 7 makes the
     * final packet continue into padding instead of wrapping its bytes.
     * Accept only this MAC and broadcast frames, with an 8 KiB ring. */
    RX_CONFIGURATION = (4U << 13) | (6U << 8) | (1U << 7) | 0x0a
};

static volatile u8 rx_buffer[RX_RING_SIZE + RX_PADDING]
    __attribute__((aligned(256)));
static u8 tx_buffers[TX_DESCRIPTOR_COUNT][TX_BUFFER_SIZE]
    __attribute__((aligned(256)));
static u8 tx_pending[TX_DESCRIPTOR_COUNT];
static u8 hardware_mac[6];
static u16 io_base;
static u8 ready;
static u32 rx_offset;
static u32 tx_next;
static u32 received_packets;
static u32 transmitted_packets;
static u32 errors;

static inline void out8(u16 port, u8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void out16(u16 port, u16 value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void out32(u16 port, u32 value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline u8 in8(u16 port)
{
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline u16 in16(u16 port)
{
    u16 value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline u32 in32(u16 port)
{
    u32 value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

/* Calls may come from the PIT handler or the shell on this single CPU. */
static inline u32 interrupt_save(void)
{
    u32 flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void interrupt_restore(u32 flags)
{
    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

static u32 pci_read(u32 device, u32 offset)
{
    out32(PCI_ADDRESS, 0x80000000U | device | (offset & 0xfc));
    return in32(PCI_DATA);
}

static int find_device(void)
{
    for (u32 bus = 0; bus < 256; ++bus) {
        for (u32 slot = 0; slot < 32; ++slot) {
            for (u32 function = 0; function < 8; ++function) {
                u32 device = (bus << 16) | (slot << 11) | (function << 8);
                if (pci_read(device, 0) != 0x813910ecU)
                    continue;
                for (u32 bar_index = 0; bar_index < 6; ++bar_index) {
                    u32 bar = pci_read(device, 0x10 + bar_index * 4);
                    u32 port = bar & ~3U;
                    /* Ports above 0xff00 cannot address the whole device. */
                    if (!(bar & 1) || port == 0 || port > 0xff00)
                        continue;
                    u16 command = (u16)pci_read(device, 4);
                    out32(PCI_ADDRESS, 0x80000000U | device | 4);
                    /* A word write leaves PCI status's W1C bits alone. */
                    out16(PCI_DATA, (u16)(command | 0x0005));
                    if ((pci_read(device, 4) & 0x0005) != 0x0005)
                        return 0;
                    io_base = (u16)port;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void reap_transmits(void)
{
    for (u32 slot = 0; slot < TX_DESCRIPTOR_COUNT; ++slot) {
        if (!tx_pending[slot])
            continue;
        u32 status = in32((u16)(io_base + REG_TX_STATUS + slot * 4));
        /* OWN alone only says the DMA has completed. Keep the descriptor
         * until transmission ends, as required by the round-robin queue. */
        if (!(status & TX_HOST_OWNS) ||
            !(status & (TX_OK | TX_UNDERRUN | TX_ABORT | TX_COLLISION)))
            continue;
        /* Carrier-lost can be asserted spuriously at 100 Mbps. TOK is
         * authoritative unless the descriptor records a major TX error. */
        if ((status & TX_OK) && !(status & (TX_ABORT | TX_COLLISION))) {
            ++transmitted_packets;
        } else {
            ++errors;
        }
        if (status & TX_ABORT)
            out32((u16)(io_base + REG_TX_CONFIG), TX_CONFIGURATION | 1);
        tx_pending[slot] = 0;
    }
}

static void reset_receiver(void)
{
    /* Only stop RX: queued transmits and their buffers remain intact.
     * Updating CAPR before acknowledging overflow lets RX DMA resume. */
    out8((u16)(io_base + REG_COMMAND), COMMAND_TX);
    rx_offset = 0;
    out32((u16)(io_base + REG_RX_ADDRESS), (u32)rx_buffer);
    out16((u16)(io_base + REG_RX_READ), (u16)(rx_offset - 16));
    out16((u16)(io_base + REG_INTERRUPT_STATUS),
          INTERRUPT_RX_OK | INTERRUPT_RX_ERROR |
          INTERRUPT_RX_OVERFLOW | INTERRUPT_RX_FIFO);
    out8((u16)(io_base + REG_COMMAND), COMMAND_RX | COMMAND_TX);
    out32((u16)(io_base + REG_RX_CONFIG), RX_CONFIGURATION);
    out32((u16)(io_base + REG_RX_MISSED), 0);
}

int rtl8139_initialize(void)
{
    u32 flags = interrupt_save();
    if (io_base) {
        out16((u16)(io_base + REG_INTERRUPT_MASK), 0);
        out8((u16)(io_base + REG_COMMAND), 0);
    }
    ready = 0;
    io_base = 0;
    received_packets = 0;
    transmitted_packets = 0;
    errors = 0;
    rx_offset = 0;
    tx_next = 0;
    for (u32 slot = 0; slot < TX_DESCRIPTOR_COUNT; ++slot)
        tx_pending[slot] = 0;
    for (u32 i = 0; i < 6; ++i)
        hardware_mac[i] = 0;

    if (!find_device()) {
        interrupt_restore(flags);
        return 0;
    }
    out16((u16)(io_base + REG_INTERRUPT_MASK), 0);
    out8((u16)(io_base + REG_CONFIG_LOCK), 0xc0);
    out8((u16)(io_base + REG_CONFIG1),
         (u8)(in8((u16)(io_base + REG_CONFIG1)) & ~3U));
    out8((u16)(io_base + REG_CONFIG_LOCK), 0);
    out8((u16)(io_base + REG_COMMAND), COMMAND_RESET);
    u32 attempt;
    for (attempt = 0; attempt < RESET_POLL_LIMIT; ++attempt) {
        if (!(in8((u16)(io_base + REG_COMMAND)) & COMMAND_RESET))
            break;
    }
    if (attempt == RESET_POLL_LIMIT) {
        ++errors;
        interrupt_restore(flags);
        return 0;
    }

    u8 all_zero = 1;
    u8 all_ones = 1;
    for (u32 i = 0; i < 6; ++i) {
        hardware_mac[i] = in8((u16)(io_base + i));
        if (hardware_mac[i] != 0)
            all_zero = 0;
        if (hardware_mac[i] != 0xff)
            all_ones = 0;
    }
    if (all_zero || all_ones || (hardware_mac[0] & 1)) {
        ++errors;
        interrupt_restore(flags);
        return 0;
    }
    for (u32 i = 0; i < sizeof(rx_buffer); ++i)
        rx_buffer[i] = 0;
    out32((u16)(io_base + REG_RX_ADDRESS), (u32)rx_buffer);
    for (u32 slot = 0; slot < TX_DESCRIPTOR_COUNT; ++slot)
        out32((u16)(io_base + REG_TX_ADDRESS + slot * 4),
              (u32)tx_buffers[slot]);
    out16((u16)(io_base + REG_RX_READ), (u16)(0U - 16));
    out16((u16)(io_base + REG_INTERRUPT_STATUS), 0xffff);
    out16((u16)(io_base + REG_INTERRUPT_MASK), 0);
    out8((u16)(io_base + REG_COMMAND), COMMAND_RX | COMMAND_TX);
    out32((u16)(io_base + REG_TX_CONFIG), TX_CONFIGURATION);
    out32((u16)(io_base + REG_RX_CONFIG), RX_CONFIGURATION);
    out32((u16)(io_base + REG_RX_MISSED), 0);
    ready = 1;
    interrupt_restore(flags);
    return 1;
}

int rtl8139_present(void)
{
    return io_base != 0;
}

int rtl8139_link_up(void)
{
    return ready && !(in8((u16)(io_base + REG_MEDIA_STATUS)) & 0x04);
}

const u8 *rtl8139_mac(void)
{
    return hardware_mac;
}

int rtl8139_send(const u8 *frame, u32 length)
{
    if (!frame || length < 14 || length > FRAME_MAX)
        return 0;
    u32 flags = interrupt_save();
    if (!ready || !rtl8139_link_up()) {
        interrupt_restore(flags);
        return 0;
    }
    reap_transmits();
    u32 slot = tx_next;
    if (tx_pending[slot] ||
        !(in32((u16)(io_base + REG_TX_STATUS + slot * 4)) & TX_HOST_OWNS)) {
        interrupt_restore(flags);
        return 0;
    }
    for (u32 i = 0; i < length; ++i)
        tx_buffers[slot][i] = frame[i];
    /* Pad in our DMA buffer so short frames never expose adjacent memory. */
    for (u32 i = length; i < FRAME_MIN; ++i)
        tx_buffers[slot][i] = 0;
    if (length < FRAME_MIN)
        length = FRAME_MIN;
    tx_pending[slot] = 1;
    tx_next = (slot + 1) & (TX_DESCRIPTOR_COUNT - 1);
    /* OWN is cleared by this write; the device supplies the trailing CRC.
     * A whole-frame threshold avoids underruns with these small buffers. */
    out32((u16)(io_base + REG_TX_STATUS + slot * 4), (48U << 16) | length);
    interrupt_restore(flags);
    return 1;
}

u32 rtl8139_receive(u8 *frame, u32 capacity)
{
    u32 flags = interrupt_save();
    if (!ready) {
        interrupt_restore(flags);
        return 0;
    }
    reap_transmits();
    u16 events = in16((u16)(io_base + REG_INTERRUPT_STATUS));
    if (events & INTERRUPT_PCI_ERROR) {
        ++errors;
        ready = 0;
        out8((u16)(io_base + REG_COMMAND), 0);
        out16((u16)(io_base + REG_INTERRUPT_STATUS), events);
        interrupt_restore(flags);
        return 0;
    }
    if (events & (INTERRUPT_RX_OVERFLOW | INTERRUPT_RX_FIFO)) {
        ++errors;
        reset_receiver();
        interrupt_restore(flags);
        return 0;
    }
    /* Link-change and TX status events do not require a NIC interrupt. */
    out16((u16)(io_base + REG_INTERRUPT_STATUS),
          (u16)(events & ~(INTERRUPT_RX_OK | INTERRUPT_RX_ERROR)));

    for (u32 attempt = 0; attempt < RX_DISCARD_LIMIT; ++attempt) {
        if (in8((u16)(io_base + REG_COMMAND)) & COMMAND_RX_EMPTY)
            break;
        u16 status = (u16)(rx_buffer[rx_offset] |
                            ((u16)rx_buffer[rx_offset + 1] << 8));
        u16 length = (u16)(rx_buffer[rx_offset + 2] |
                            ((u16)rx_buffer[rx_offset + 3] << 8));
        /* 0xfff0 marks a packet whose DMA has not finished on RTL8139.
         * Leave its ring position alone and retry on the next PIT tick. */
        if (length == 0xfff0)
            break;
        if (length < 4 || length > FRAME_MAX + 4) {
            ++errors;
            reset_receiver();
            break;
        }
        u32 payload_length = length - 4;
        int valid = (status & RX_OK) && !(status & RX_ERRORS) &&
                    payload_length >= FRAME_MIN && frame &&
                    payload_length <= capacity;
        if (valid) {
            for (u32 i = 0; i < payload_length; ++i)
                frame[i] = rx_buffer[rx_offset + 4 + i];
        } else {
            ++errors;
        }
        rx_offset = (rx_offset + length + 4 + 3) & ~3U;
        rx_offset &= RX_RING_SIZE - 1;
        out16((u16)(io_base + REG_RX_READ), (u16)(rx_offset - 16));
        out16((u16)(io_base + REG_INTERRUPT_STATUS),
              INTERRUPT_RX_OK | INTERRUPT_RX_ERROR);
        if (valid) {
            ++received_packets;
            interrupt_restore(flags);
            return payload_length;
        }
    }
    interrupt_restore(flags);
    return 0;
}

u32 rtl8139_rx_count(void)
{
    return received_packets;
}

u32 rtl8139_tx_count(void)
{
    return transmitted_packets;
}

u32 rtl8139_error_count(void)
{
    return errors;
}
