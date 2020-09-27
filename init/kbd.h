#ifndef KBD_H
#define KBD_H

#include <stdint.h>

#include "vm86.h"

#define KBD_IRQ 1
#define KBD_BUFFER_SIZE 16
#define KBD_PORT_LO 0x60
#define KBD_PORT_HI 0x64

typedef struct kbd {
    uint16_t keybuff[KBD_BUFFER_SIZE];
    size_t keybuff_len;
}
kbd_t;

void
kbd_init(kbd_t* kbd);

void
kbd_send_input(kbd_t* kbd, uint8_t scancode);

void
kbd_int(kbd_t* kbd, regs_t* regs);

#endif
