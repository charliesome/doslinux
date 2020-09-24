CC = i386-linux-musl-gcc
CFLAGS = -m32 -static -Os -Wall -Wextra
NASM = nasm
STRIP = i386-linux-musl-strip

HDD_BASE = hdd.base.img
LINUX_BZIMAGE = linux-5.8.9/arch/x86/boot/bzImage
BUSYBOX_BIN = busybox-1.32.0/busybox_unstripped

.PHONY: all
all: hdd.img

.PHONY: clean
clean:
	rm -f hdd.img doslinux.com init/init init/*.o

hdd.img: $(HDD_BASE) doslinux.com init/init $(LINUX_IMAGE) $(BUSYBOX_BIN)
	cp $(HDD_BASE) hdd.img
	MTOOLSRC=mtoolsrc mmd C:/doslinux
	MTOOLSRC=mtoolsrc mcopy doslinux.com C:/doslinux/dsl.com
	MTOOLSRC=mtoolsrc mcopy init/init C:/doslinux/init
	MTOOLSRC=mtoolsrc mcopy $(LINUX_BZIMAGE) C:/doslinux/bzimage
	MTOOLSRC=mtoolsrc mcopy $(BUSYBOX_BIN) C:/doslinux/busybox
	MTOOLSRC=mtoolsrc mmd C:/doslinux/rootfs

doslinux.com: doslinux.asm
	$(NASM) -o $@ -f bin $<

init/init: init/init.o init/vm86.o init/panic.o init/kbd.o init/term.o
	$(CC) $(CFLAGS) -o $@ $^

init/%.o: init/%.c init/*.h
	$(CC) $(CFLAGS) -o $@ -c $<
