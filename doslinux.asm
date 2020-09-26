org 0x100

%define DOSLINUX_INT 0xe7

    ; get current default drive
    mov ah, 0x19
    int 0x21
    add al, 'a' ; interrupt returns drive index, not letter in AL
    mov [current_drive], al

    ; get current directory, returns pointer to ASCIZ string in DS:SI
    mov ah, 0x47
    xor dl, dl ; default drive
    mov si, current_dir_buffer
    int 0x21

    ; detect XMS (eg HIMEM.SYS) and bail if present
    mov ax, 0x4300
    int 0x2f
    cmp al, 0x80
    jne no_xms

    ; print error message if XMS found
    mov dx, xms_not_supported
    mov ah, 0x09
    int 0x21

    ; exit
    mov ah, 0x4c
    int 0x21

no_xms:
    ; detect already running instance of WSL
    call detect_dsl
    test ax, ax
    jz start_linux

run_command:
    ; doslinux is already running, prepare to run linux command

    ; flush disk first
    mov ah, 0x0d
    int 0x21

    ; invoke the run command syscall
    mov ah, 1
    mov dl, [current_drive]
    mov si, current_dir_buffer
    int DOSLINUX_INT

    ; replicate linux cursor position in BIOS
    call fix_cursor

    ; exit
    mov ah, 0x4c
    int 0x21

start_linux:
    ; open bzimage.com
    mov ax, 0x3d00
    mov dx, bzimage_path
    int 0x21

    ; check error
    mov dx, bzimage_open_err
    jc fatal

    ; store file handle
    mov [bzimage_handle], ax

    ; read first sector of bzimage
    mov ah, 0x3f
    mov dx, bzimage
    mov bx, [bzimage_handle]
    mov cx, 512
    int 0x21

    ; check error
    mov dx, bzimage_read_err
    jc fatal

    ; pull setup_sects value from header
    movzx ax, byte [k_setup_sects_b]
    shl ax, 9 ; multiply by 512 to get byte count from sector count
    mov [setup_bytes], ax

    ; read remaining setup code
    mov ah, 0x3f
    mov dx, bzimage + 512
    mov bx, [bzimage_handle]
    mov cx, [setup_bytes]
    int 0x21

    ; check error
    mov dx, bzimage_read_err
    jc fatal

    ; check magic header value
    mov eax, [k_header_magic_d]
    cmp eax, 0x53726448 ; 'HdrS'
    mov dx, not_kernel_err
    jne fatal

    ; pull syssize from header - count of 16 byte paras of system code after setup
    mov eax, [k_syssize_d]
    shl eax, 4 ; multiply by 16 to get bytes
    mov [sys_bytes], eax

    ; calculate sys_load_end pointer
    add eax, [sys_load_ptr]
    mov [sys_load_end], eax

    ; init unreal mode switching in prep for loading kernel to extended memory
    call init_unreal

.sys_load_loop:
    mov ah, 0x3f
    mov dx, readbuf
    mov bx, [bzimage_handle]
    mov cx, READBUF_SIZE
    int 0x21

    ; check error
    mov dx, bzimage_read_err
    jc fatal

    ; do unreal copy
    mov si, readbuf
    mov edi, [sys_load_ptr]
    mov ecx, READBUF_SIZE
    call copy_unreal

    ; advance load pointer
    add dword [sys_load_ptr], READBUF_SIZE

    ; loop around again if more to read
    mov eax, [sys_load_ptr]
    cmp eax, [sys_load_end]
    jb .sys_load_loop

    ; finished reading kernel, set obligatory kernel params:

    ; use our current video mode for kernel vidmode parameter
    mov [k_vidmode_w], word 0

    ; we are not a registered bootloader
    mov byte [k_type_of_loader_b], 0xff

    ; set load flags
    %define LOADED_HIGH_FLAG 0x01
    %define CAN_USE_HEAP_FLAG 0x80
    mov byte [k_loadflags_b], LOADED_HIGH_FLAG | CAN_USE_HEAP_FLAG

    ; no ramdisk
    mov dword [k_ramdisk_size_d], 0
    mov dword [k_ramdisk_image_d], 0

    ; set heap end pointer - TODO is this correct?
    %define HEAP_END 0xe000
    mov word [k_heap_end_ptr_w], HEAP_END

    ; copy cmd line into place
    mov si, cmdline
    mov di, bzimage + HEAP_END
    mov cx, cmdline.end - cmdline
    rep movsb
    ; now calculate linear address for pointer
    mov ax, ds
    movzx eax, ax
    shl eax, 4
    mov ebx, bzimage + HEAP_END
    add ebx, eax
    mov [k_cmd_line_ptr_d], ebx

    ; set kernel boot params relevant to relocation
    mov dword [k_code32_start_d], kernel_base

    ; write CS:IP of vm86_return into somewhere init can grab it from
    call enter_unreal
    push es
    mov ax, 0x08
    mov es, ax

    a32 mov [es:0x100000], word vm86_return

    mov ax, cs
    a32 mov [es:0x100002], word ax

    pushf
    pop ax
    a32 mov [es:0x100004], word ax

    a32 mov [es:0x100006], sp

    mov ax, ss
    a32 mov [es:0x100008], word ax

    call exit_unreal
    pop es

    ; print initializing message right before starting kernel
    mov dx, initializing
    mov ah, 0x09
    int 0x21

    ; calculate kernel segment
    mov ax, ds
    movzx eax, ax
    shl eax, 4
    add eax, bzimage
    shr eax, 4

    ; disable interrupts and setup segments/stack
    cli
    mov ss, ax
    mov sp, HEAP_END
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; enter kernel
    ; see https://www.kernel.org/doc/html/latest/x86/boot.html#running-the-kernel

    ; kernel code seg is + 0x20
    add ax, 0x20

    ; since the segment is dynamic we need an indirect jump
    mov bx, sp
    sub bx, 4
    mov [bx], word 0
    mov [bx + 2], ax
    jmp far [bx]

vm86_return:
    xchg bx, bx

    ; now DSL is running we can run the originally invoked command
    jmp run_command

;
; helper subroutines
;

; print error message and exit
fatal:
    mov ah, 0x09
    int 0x21
    mov ah, 0x4c
    int 0x21

; initialize unreal mode switching
init_unreal:
    ; setup gdt offset in gdtr
    mov eax, ds
    shl eax, 4
    add eax, gdt
    mov [gdtr.offset], eax

    ret

; copy from low to extended memory (> 1 MiB)
; DS:ESI - source (far pointer)
; EDI - destination (linear address)
; ECX - byte count
; clobbers EAX
copy_unreal:
    ; zero high bits of ESI
    movzx eax, si
    mov esi, eax

    ; save ds/es and load 32 bit segment selectors
    call enter_unreal
    push es
    mov ax, 0x08
    mov es, ax

    ; copy 4 bytes at a time:
    add ecx, 3
    shr ecx, 2

    ; do the copy, using 32 bit address override
    a32 rep movsd

    ; restore ds/es
    call exit_unreal
    pop es

    ret

enter_unreal:
    ; load gdt to prepare to enter protected mode
    cli
    lgdt [gdtr]

    ; enable protected mode
    mov eax, cr0
    or al, 1
    mov cr0, eax

    ret

exit_unreal:
    ; disable protected mode
    mov eax, cr0
    and al, ~1
    mov cr0, eax

    ret

; returns line VGA cursor is on in AL, 0 indexed
; clobbers DX and BX
cursor_line:
    ; read cursor pos into bx from vga
    mov al, 0x0f
    mov dx, 0x3d4
    out dx, al

    mov dx, 0x3d5
    in al, dx
    mov bl, al

    mov al, 0x0e
    mov dx, 0x3d4
    out dx, al

    mov dx, 0x3d5
    in al, dx
    mov bh, al

    ; calculate line
    mov ax, bx
    xor dx, dx
    add ax, 79 ; for round-up division
    mov bx, 80
    div bx

    xor ah, ah

    ret

; replicate VGA cursor pos in BIOS
fix_cursor:
    call cursor_line

    mov dh, al ; line number
    xor dl, dl ; column
    xor bh, bh ; page number (?)
    mov ah, 0x02 ; set cursor pos
    int 0x10

    ret


; detects running DSL instance
; returns 1 if running in AX, 0 otherwise
detect_dsl:
    ; push flags and disable interrupts before we do anything dodgy
    pushf
    cli

    ; set fs to zero to access IVT
    push fs
    xor ax, ax
    mov fs, ax

    ; save previous handler for doslinux interrupt
    mov ax, [fs:DOSLINUX_INT * 4]
    push ax
    mov ax, [fs:DOSLINUX_INT * 4 + 2]
    push ax

    ; set up dummy interrupt handler so we don't crash or invoke any
    ; unintended behaviour when calling the doslinux interrupt
    mov [fs:DOSLINUX_INT * 4], word .dummy_handler
    mov ax, cs
    mov [fs:DOSLINUX_INT * 4 + 2], ax

    ; hit it
    xor ax, ax
    int DOSLINUX_INT
    mov [.is_running], ax

    ; restore previous interrupt handler
    pop ax
    mov [fs:DOSLINUX_INT * 4 + 2], ax
    pop ax
    mov [fs:DOSLINUX_INT * 4], ax

    ; restore previous fs
    pop fs

    ; restore flags
    popf

    ; test ax
    mov ax, [.is_running]

    ret

.is_running dw 0

.dummy_handler:
    mov [.is_running], word 0
    iret

;
; RO data
;

xms_not_supported db "Extended memory manager detected (maybe HIMEM.SYS?) - cannot start DOS Subsystem for Linux", 13, 10, "$"
bzimage_path db "C:\doslinux\bzimage", 0
bzimage_open_err db "Could not open bzImage", 13, 10, "$"
bzimage_read_err db "Could not read bzImage", 13, 10, "$"
not_kernel_err db "bzImage is not a Linux kernel", 13, 10, "$"
initializing db "Starting DOS Subsystem for Linux, please wait...$"
newline db 13, 10, "$"

; reserve entire low memory region
cmdline: db "quiet init=/doslinux/init root=/dev/sda1 "

            ; kernel should scroll in software rather than hardware, DOS/BIOS
            ; only scrolls in software:
         db "nomodeset no-scroll "

            ; mark phys memory from 0x100000-0x110000 as reserved, we may
            ; use high memory area in DOS
         db "memmap=64K$0x100000 "

         db 0
    .end:

gdt:
    ; entry 0x00 : null
    dq 0

    ; data entry
    dw 0xffff       ; limit 0:15
    .data_base_0_w:
    dw 0x0000       ; base 0:15
    .data_base_16_b:
    db 0x00         ; base 16:23
    db 0b10010010   ; access byte - data
    db 0xcf         ; flags/(limit 16:19). 4 KB granularity + 32 bit mode flags
    .data_base_24_b:
    db 0x00         ; base 24:31
.end:

;
; variables
;

align 4

bzimage_handle: dw 0
setup_bytes: dw 0
heap_end: dw 0

align 4
sys_bytes: dd 0
sys_load_ptr: dd kernel_base
sys_load_end: dd 0

current_drive: db 0
current_dir_buffer: times 64 db 0

align 4
gdtr:
    dw gdt.end - gdt - 1
.offset:
    dd 0

;
; constants
;

kernel_base equ 0x200000

; kernel real mode header fields
; see https://www.kernel.org/doc/html/latest/x86/boot.html#the-real-mode-kernel-header
k_setup_sects_b         equ bzimage + 0x1f1
k_syssize_d             equ bzimage + 0x1f4
k_header_magic_d        equ bzimage + 0x202

; obligatory fields - we must supply this information to kernel
k_vidmode_w             equ bzimage + 0x1fa
k_type_of_loader_b      equ bzimage + 0x210
k_loadflags_b           equ bzimage + 0x211
k_setup_move_size_w     equ bzimage + 0x212
k_ramdisk_image_d       equ bzimage + 0x218
k_ramdisk_size_d        equ bzimage + 0x21c
k_heap_end_ptr_w        equ bzimage + 0x224
k_ext_loader_type_b     equ bzimage + 0x227
k_cmd_line_ptr_d        equ bzimage + 0x228

; reloc fields - we must supply if kernel is relocated
k_code32_start_d        equ bzimage + 0x214
k_kernel_alignment_d    equ bzimage + 0x230
k_relocatable_kernel_b  equ bzimage + 0x234
k_min_alignment_b       equ bzimage + 0x235
k_pref_address_q        equ bzimage + 0x258


align 16
progend:

READBUF_SIZE equ 4096

readbuf equ progend
bzimage equ progend + READBUF_SIZE
