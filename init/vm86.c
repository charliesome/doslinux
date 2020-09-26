#include <bits/signal.h>
#include <bits/syscall.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/io.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kbd.h"
#include "panic.h"
#include "term.h"
#include "vm86.h"

typedef struct task {
    regs_t* regs;
    kbd_t kbd;
    bool pending_interrupt;
    uint8_t pending_interrupt_nr;
}
task_t;

enum rep_kind {
    NONE,
    REP,
};

enum bit_size {
    BITS16 = 0,
    BITS32 = 1,
};

static void*
linear(uint16_t segment, uint16_t offset)
{
    uint32_t seg32 = segment;
    uint32_t off32 = offset;
    uint32_t lin = (seg32 << 4) + off32;
    return (void*)lin;
}

static uint8_t
peek8(uint16_t segment, uint16_t offset)
{
    return *(uint8_t*)linear(segment, offset);
}

static void
poke8(uint16_t segment, uint16_t offset, uint8_t value)
{
    *(uint8_t*)linear(segment, offset) = value;
}

static void
poke16(uint16_t segment, uint16_t offset, uint16_t value)
{
    *(uint16_t*)linear(segment, offset) = value;
}

static void
poke32(uint16_t segment, uint16_t offset, uint32_t value)
{
    *(uint32_t*)linear(segment, offset) = value;
}

static uint8_t
peekip(regs_t* regs, uint16_t offset)
{
    return peek8(regs->cs.word.lo, regs->eip.word.lo + offset);
}

struct ivt_descr {
    uint16_t offset;
    uint16_t segment;
};

static struct ivt_descr* const IVT = 0;

static void
push16(regs_t* regs, uint16_t value)
{
    regs->esp.word.lo -= 2;
    poke16(regs->ss.word.lo, regs->esp.word.lo, value);
}

static void do_pending_int(task_t* task);

static void
do_int(task_t* task, uint8_t vector)
{
    push16(task->regs, task->regs->eflags.word.lo);
    push16(task->regs, task->regs->cs.word.lo);
    push16(task->regs, task->regs->eip.word.lo);
    struct ivt_descr* descr = &IVT[vector];
    task->regs->cs.word.lo = descr->segment;
    task->regs->eip.dword = descr->offset;
}

static void
do_software_int(task_t* task, uint8_t vector)
{
    do_int(task, vector);
}

static void
do_pending_int(task_t* task)
{
    if (task->pending_interrupt) {
        task->pending_interrupt = false;
        do_int(task, task->pending_interrupt_nr);
    }
}

static bool
is_port_whitelisted(uint16_t port)
{
    // ports whitelisted here are directly accessed by DOS rather than through
    // BIOS. we need to do something about them eventually, but for now just
    // let access succeed without intervention.

    // programmable interrupt controller
    if (port == 0x20 || port == 0x21 || port == 0xa0 || port == 0xa1) {
        return true;
    }

    // programmable interval timer
    if (port >= 0x40 && port <= 0x43) {
        // TODO should we handle timing ourselves? it might mess with linux
        return true;
    }

    // primary ATA
    if (port >= 0x1f0 && port <= 0x1f7) {
        return true;
    }

    // secondary ATA
    if (port >= 0x170 && port <= 0x177) {
        return true;
    }

    // VGA
    if (port >= 0x3b0 && port <= 0x3df) {
        return true;
    }

    // floppy disk
    if (port >= 0x3f0 && port <= 0x3f7) {
        return true;
    }

    // dunno what these are
    if (port >= 0x1ce && port <= 0x1cf) {
        return true;
    }

    if (port == 0x402) {
        return true;
    }

    if (port == 0x608) {
        return true;
    }

    return false;
}

static uint8_t
do_inb(task_t* task, uint16_t port)
{
    if (port >= KBD_PORT_LO && port <= KBD_PORT_HI) {
        return kbd_read_port(&task->kbd, port);
    }

    uint8_t value = inb(port);

    if (!is_port_whitelisted(port)) {
        printf("inb port %04x value %02x cs:ip %04x:%04x\r\n",
            port, value, task->regs->cs.word.lo, task->regs->eip.word.lo);
    }

    return value;
}

static uint16_t
do_inw(task_t* task, uint16_t port)
{
    uint16_t value = inw(port);

    if (!is_port_whitelisted(port)) {
        printf("inw port %04x value %04x cs:ip %04x:%04x\r\n",
            port, value, task->regs->cs.word.lo, task->regs->eip.word.lo);
    }

    return value;
}

static uint32_t
do_ind(task_t* task, uint16_t port)
{
    uint32_t value = inl(port);

    if (!is_port_whitelisted(port)) {
        printf("ind port %04x value %08x cs:ip %04x:%04x\r\n",
            port, value, task->regs->cs.word.lo, task->regs->eip.word.lo);
    }

    return value;
}

static void
do_outb(task_t* task, uint16_t port, uint8_t value)
{
    if (port >= KBD_PORT_LO && port <= KBD_PORT_HI) {
        kbd_write_port(&task->kbd, port, value);
        return;
    }

    if (port == 0x20) {
        // ignore pic writes, they mess with stuff
        return;
    }

    if (!is_port_whitelisted(port)) {
        printf("outb port %04x value %02x cs:ip %04x:%04x\r\n",
            port, value, task->regs->cs.word.lo, task->regs->eip.word.lo);
    }

    outb(value, port);
}

static void
do_outw(task_t* task, uint16_t port, uint16_t value)
{
    if (!is_port_whitelisted(port)) {
        printf("outw port %04x value %04x cs:ip %04x:%04x\r\n",
            port, value, task->regs->cs.word.lo, task->regs->eip.word.lo);
    }

    outw(value, port);
}

static void
do_outd(task_t* task, uint16_t port, uint32_t value)
{
    if (!is_port_whitelisted(port)) {
        printf("outd port %04x value %08x cs:ip %04x:%04x\r\n",
            port, value, task->regs->cs.word.lo, task->regs->eip.word.lo);
    }

    outl(value, port);
}

static void
do_insb(task_t* task)
{
    poke8(task->regs->es16.word.lo, task->regs->edi.word.lo, do_inb(task, task->regs->edx.word.lo));
    task->regs->edi.word.lo += 1;
}

static void
do_insw(task_t* task)
{
    uint16_t value = do_inw(task, task->regs->edx.word.lo);
    poke16(task->regs->es16.word.lo, task->regs->edi.word.lo, value);
    task->regs->edi.word.lo += 2;
}

static void
do_insd(task_t* task)
{
    uint32_t value = do_ind(task, task->regs->edx.word.lo);
    poke32(task->regs->es16.word.lo, task->regs->edi.word.lo, value);
    task->regs->edi.word.lo += 4;
}

static uint32_t
rep_count(regs_t* regs, enum rep_kind rep_kind, enum bit_size operand, enum bit_size address)
{
    if (rep_kind == NONE) {
        return 1;
    } else {
        if (operand ^ address) {
            return regs->ecx.dword;
        } else {
            return regs->ecx.word.lo;
        }
    }
}

static void
emulate_insn(task_t* task)
{
    enum bit_size address = BITS16;
    enum bit_size operand = BITS16;
    enum rep_kind rep_kind = NONE;

prefix:
    switch (peekip(task->regs, 0)) {
        case 0x66:
            operand = BITS32;
            task->regs->eip.word.lo++;
            goto prefix;
        case 0x67:
            address = BITS32;
            task->regs->eip.word.lo++;
            goto prefix;
        case 0xf3:
            rep_kind = REP;
            task->regs->eip.word.lo++;
            goto prefix;

        // segment overrides, just ignore:
        case 0x2e: // cs
        case 0x3e: // ds
        case 0x26: // es
        case 0x36: // ss
        case 0x64: // fs
        case 0x65: // gs
            goto prefix;

        default:
            break;
    }

#define REPEAT(blk) do { \
        for (uint32_t count = rep_count(task->regs, rep_kind, operand, address); count; count--) { \
            blk \
        } \
        if (rep_kind != NONE) { \
            task->regs->ecx.dword = 0; \
        } \
    } while (0)

    switch (peekip(task->regs, 0)) {
    case 0x6c: {
        // INSB

        REPEAT({
            do_insb(task);
        });

        task->regs->eip.word.lo += 1;
        return;
    }
    case 0x6d: {
        // INSW

        REPEAT({
            if (operand == BITS32) {
                do_insd(task);
            } else {
                do_insw(task);
            }
        });

        task->regs->eip.word.lo += 1;
        return;
    }
    case 0xe4:
        // INB imm
        task->regs->eax.byte.lo = do_inb(task, peekip(task->regs, 1));
        task->regs->eip.word.lo += 2;
        return;
    case 0xe5:
        // INW imm
        if (operand == BITS32) {
            task->regs->eax.dword = do_ind(task, peekip(task->regs, 1));
        } else {
            task->regs->eax.word.lo = do_inw(task, peekip(task->regs, 1));
        }
        task->regs->eip.word.lo += 2;
        return;
    case 0xe6:
        // OUTB imm
        do_outb(task, peekip(task->regs, 1), task->regs->eax.byte.lo);
        task->regs->eip.word.lo += 2;
        return;
    case 0xe7:
        // OUTW imm
        if (operand == BITS32) {
            do_outd(task, peekip(task->regs, 1), task->regs->eax.dword);
        } else {
            do_outw(task, peekip(task->regs, 1), task->regs->eax.word.lo);
        }
        task->regs->eip.word.lo += 2;
        return;
    case 0xec:
        // INB DX
        task->regs->eax.byte.lo = do_inb(task, task->regs->edx.word.lo);
        task->regs->eip.word.lo += 1;
        return;
    case 0xed:
        // INW DX
        if (operand == BITS32) {
            task->regs->eax.dword = do_ind(task, task->regs->edx.word.lo);
        } else {
            task->regs->eax.word.lo = do_inw(task, task->regs->edx.word.lo);
        }
        task->regs->eip.word.lo += 1;
        return;
    case 0xee:
        // OUTB DX
        do_outb(task, task->regs->edx.word.lo, task->regs->eax.byte.lo);
        task->regs->eip.word.lo += 1;
        return;
    case 0xef:
        // OUTW DX
        if (operand == BITS32) {
            do_outd(task, task->regs->edx.word.lo, task->regs->eax.dword);
        } else {
            do_outw(task, task->regs->edx.word.lo, task->regs->eax.word.lo);
        }
        task->regs->eip.word.lo += 1;
        return;
    case 0xf4: {
        // HLT

        if (!(task->regs->eflags.word.lo & FLAG_INTERRUPT)) {
            panic("8086 task halted CPU with interrupts disabled");
        }

        // just no-op on HLT for now

        task->regs->eip.word.lo += 1;

        return;
    }
    default:
        printf("[%04x:%04x] unknown instruction in gpf: %02x\n",
            task->regs->cs.word.lo,
            task->regs->eip.word.lo,
            peekip(task->regs, 0));
        fatal();
    }

    panic("unhandled GPF");
}

void
vm86_interrupt(task_t* task, uint8_t vector)
{
    if (task->regs->eflags.word.lo & FLAG_INTERRUPT) {
        // printf("Dispatching interrupt %04x\r\n", vector);
        do_int(task, vector);
    } else {
        printf("Setting pending interrupt %04x\r\n", vector);
        task->pending_interrupt = true;
        task->pending_interrupt_nr = vector;
    }
}

void
vm86_gpf(task_t* task)
{
    emulate_insn(task);

    // FIXME something is setting NT, IOPL=3, and a reserved bit in EFLAGS
    // not sure what's happening, but this causes things to break and clearing
    // these bits seems to work around it for now ¯\_(ツ)_/¯
    task->regs->eflags.dword &= ~(0xf << 12);
}

static void
do_syscall(task_t* task)
{
    uint8_t ah = task->regs->eax.byte.hi;

    switch (ah) {
        case 0: {
            // presence test
            task->regs->eax.word.lo = 1;
            break;
        }
        case 1: {
            // run command

            // first acquire ownership of the terminal
            term_acquire();

            uint32_t prog_base = (uint32_t)task->regs->cs.word.lo << 4;

            // extract command to execute out of PSP
            uint8_t* psp = (uint8_t*)prog_base;
            size_t cmdline_len = psp[0x80];
            uint8_t* cmdline_raw = psp + 0x81;

            char cmdline[256] = { 0 };
            memcpy(cmdline, cmdline_raw, cmdline_len);

            // extract current DOS drive from DL and path from SI
            char current_dos_drive = task->regs->edx.byte.lo;

            if (current_dos_drive >= 'a' && current_dos_drive <= 'z') {
                const char* current_dos_path = linear(task->regs->cs.word.lo, task->regs->esi.word.lo);

                char linux_dir[70] = "/mnt/";
                char* linux_dir_ptr = linux_dir + 5;

                *linux_dir_ptr++ = current_dos_drive;
                *linux_dir_ptr++ = '/';

                for (size_t i = 0; i < 64; i++) {
                    if (current_dos_path[i] == 0) {
                        break;
                    }

                    if (current_dos_path[i] == '\\') {
                        *linux_dir_ptr++ = '/';
                    } else {
                        *linux_dir_ptr++ = current_dos_path[i];
                    }
                }

                *linux_dir_ptr++ = 0;

                int rc = chdir(linux_dir);

                if (rc < 0) {
                    perror("warn: cannot chdir");
                }
            }

            // execute the command
            pid_t child = fork();

            if (child < 0) {
                perror("fork");
                break;
            }

            if (child == 0) {
                char sh[] = "sh";
                char opt_c[] = "-c";
                char* argv[] = { sh, opt_c, cmdline, NULL };

                char path[] = "PATH=/usr/bin:/usr/sbin:/bin:/sbin";
                char* envp[] = { path, NULL };

                execve("/bin/busybox", argv, envp);
            }

            while (1) {
                int wstat;
                int rc = waitpid(child, &wstat, 0);

                if (rc < 0) {
                    perror("waitpid");
                    break;
                }

                if (WIFEXITED(wstat)) {
                    break;
                }
            }

            // yield terminal ownership back to DOS
            term_yield_to_dos();
        }
        default: {
            break;
        }
    }
}

static volatile sig_atomic_t
received_keyboard_input = 0;

static void
on_sigio(int sig, siginfo_t* info, void* context)
{
    (void)sig;
    (void)context;

    if (info->si_fd == STDIN_FILENO && (info->si_band & POLLIN)) {
        // data to be read on stdin
        received_keyboard_input = 1;
    }
}

static void
setup_sigio()
{
    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_sigio;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGIO, &sa, NULL)) {
        perror("sigaction SIGIO");
        fatal();
    }
}

__attribute__((noreturn)) void
vm86_run(struct vm86_init init_params)
{
    // need elevated port privileges for init
    iopl(3);

    struct vm86plus_struct vm86 = { 0 };
    vm86.regs.cs = init_params.cs;
    vm86.regs.eip = init_params.ip;
    vm86.regs.eflags = init_params.flags;
    vm86.regs.esp = init_params.sp;
    vm86.regs.ss = init_params.ss;
    vm86.regs.ds = init_params.ss;
    vm86.regs.es = init_params.ss;
    vm86.regs.fs = init_params.ss;

    // cannot emulate 386 as we rely on port I/O trapping
    vm86.cpu_type = 2;

    // make sure we always trap DOSLINUX_INT
    vm86.int_revectored.__map[DOSLINUX_INT >> 5] |= 1 << (DOSLINUX_INT & 0x1f);

    task_t task = { 0 };
    task.regs = (void*)&vm86.regs;
    kbd_init(&task.kbd);

    setup_sigio();
    term_init();
    term_yield_to_dos();

    while (1) {
        // set IOPL=0 before returning to DOS so we can intercept port I/O
        iopl(0);

        int rc = syscall(SYS_vm86, VM86_ENTER, &vm86);

        // and then reenable it for the supervisor
        iopl(3);

        switch (VM86_TYPE(rc)) {
            case VM86_SIGNAL: {
                if (received_keyboard_input) {
                    // even if we race with a second signal here, we should
                    // always catch the input in the read call anyway
                    received_keyboard_input = 0;

                    while (1) {
                        char scancode;
                        ssize_t nread = read(STDIN_FILENO, &scancode, 1);

                        if (nread < 0 && errno == EAGAIN) {
                            break;
                        }

                        if (nread == 0) {
                            // eof? what to do...
                            break;
                        }

                        kbd_send_input(&task.kbd, scancode);

                        // IRQ #1 is ivec 9 - TODO handle PIC remapping
                        vm86_interrupt(&task, 0x09);
                    }
                }
                break;
            }
            case VM86_UNKNOWN: {
                vm86_gpf(&task);
                break;
            }
            case VM86_INTx: {
                uint8_t vector = VM86_ARG(rc);
                uint8_t ah = task.regs->eax.byte.hi;
                uint16_t ax = task.regs->eax.word.lo;

                if (vector == 0xe7) {
                    // doslinux syscall
                    do_syscall(&task);
                    break;
                }

                if (vector == 0x16) {
                    // BIOS keyboard services
                    do_software_int(&task, VM86_ARG(rc));
                    break;
                }

                if (vector == 0x15 && ah == 0x4f) {
                    // keyboard intercept
                    do_software_int(&task, VM86_ARG(rc));
                    break;
                }

                if (vector == 0x15 && ax == 0x5305) {
                    // APM cpu idle
                    break;
                }

                if (vector == 0x13 && ah == 0x02) {
                    // disk - read sectors into memory
                    do_software_int(&task, VM86_ARG(rc));
                    break;
                }

                if (vector == 0x1a && ah <= 0x0f) {
                    // BIOS time services
                    do_software_int(&task, VM86_ARG(rc));
                    break;
                }

                // log all non-whitelisted software interrupts
                printf("VM86_INTx: %02x AX=%04x CS:IP=%04x:%04x\r\n",
                    VM86_ARG(rc), task.regs->eax.word.lo, task.regs->cs.word.lo, task.regs->eip.word.lo);

                do_software_int(&task, VM86_ARG(rc));
                break;
            }
            case VM86_STI: {
                printf("VM86_STI\r\n");
                do_pending_int(&task);
                break;
            }
            case VM86_PICRETURN: {
                // this can only happen if vm86plus_info_struct.force_return_for_pic
                // is set, and this field is never set by us, set by the kernel, or
                // even set by dosemu it seems. this just shouldn't happen.
                panic("VM86_PICRETURN should never occur");
                break;
            }
            case VM86_TRAP: {
                // ignore
                break;
            }
            default: {
                printf("unknown vm86 return code: %d\n", rc);
                break;
            }
        }
    }
}
