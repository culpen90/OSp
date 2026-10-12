#ifndef OSP_KERNEL_H
#define OSP_KERNEL_H

/* The kernel uses no hosted headers or runtime. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

void kernel_main(void);
void interrupt_dispatch(u32 vector, u32 error);

#endif
