#ifndef VM86_H
#define VM86_H

#include <asm/vm86.h>
#include <stdbool.h>
#include <stdint.h>

#define DOSLINUX_INT 0xe7

#define FLAG_INTERRUPT              (1 << 9)
#define FLAG_VM8086                 (1 << 17)

typedef union reg32 {
    uint32_t dword;
    struct {
        uint16_t lo;
        uint16_t hi;
    } word;
    struct {
        uint8_t lo;
        uint8_t hi;
        uint8_t res1;
        uint8_t res2;
    } byte;
}
reg32_t;

// this is like vm86_regs, but all registers are of union type reg32_t
typedef struct regs {
    reg32_t ebx;
    reg32_t ecx;
    reg32_t edx;
    reg32_t esi;
    reg32_t edi;
    reg32_t ebp;
    reg32_t eax;
    reg32_t __null_ds;
    reg32_t __null_es;
    reg32_t __null_fs;
    reg32_t __null_gs;
    reg32_t orig_eax;
    reg32_t eip;
    reg32_t cs;
    reg32_t eflags;
    reg32_t esp;
    reg32_t ss;

    reg32_t es16;
    reg32_t ds16;
    reg32_t fs16;
    reg32_t gs16;
}
regs_t;

typedef struct vm86_init {
    uint16_t ip;
    uint16_t cs;
    uint16_t flags;
    uint16_t sp;
    uint16_t ss;
} __attribute__((packed))
vm86_init_t;

__attribute__((noreturn)) void
vm86_run(vm86_init_t init_params);

#endif
