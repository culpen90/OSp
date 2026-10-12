#ifndef OSP_RTL8139_H
#define OSP_RTL8139_H

#include "kernel.h"

/* Polling driver for the RTL8139's original (non-C+) descriptor mode.
 * The kernel's flat, identity-mapped memory provides physical DMA addresses.
 * send() queues an Ethernet frame without an FCS; receive() removes the FCS.
 * Both calls are bounded and nonblocking. A zero result means no packet/slot.
 * Counters survive receive recovery, but are cleared by initialize(). */
int rtl8139_initialize(void);
int rtl8139_present(void);
int rtl8139_link_up(void);
const u8 *rtl8139_mac(void);
int rtl8139_send(const u8 *frame, u32 length);
u32 rtl8139_receive(u8 *frame, u32 capacity);
u32 rtl8139_rx_count(void);
u32 rtl8139_tx_count(void);
u32 rtl8139_error_count(void);

#endif
