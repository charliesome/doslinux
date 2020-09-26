#define _GNU_SOURCE
#include <sys/io.h>
#include <bits/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
// #include <sys/vm86.h>

#include "vm86.h"
#include "panic.h"

#define CHECKED(expr) { if ((rc = (expr)) < 0) { goto out; } }

int copy_file(const char* src, const char* dst) {
    int rc, src_fd = -1, dst_fd = -1;
    struct stat st;

    CHECKED(src_fd = open(src, O_RDONLY | O_CLOEXEC));
    CHECKED(fstat(src_fd, &st));
    CHECKED(dst_fd = open(dst, O_WRONLY | O_CREAT | O_CLOEXEC | O_TRUNC, st.st_mode));

    size_t len = st.st_size;

    while (len > 0) {
        ssize_t copied = 0;
        CHECKED(copied = copy_file_range(src_fd, NULL, dst_fd, NULL, len, 0));
        len -= copied;
    }

    rc = 0;

out:
    if (src_fd >= 0) {
        close(src_fd);
    }

    if (dst_fd >= 0) {
        close(dst_fd);
    }

    return rc;
}

int install_busybox() {
    pid_t install_pid = fork();

    if (install_pid < 0) {
        return install_pid;
    }

    if (install_pid == 0) {
        char busybox[] = "/bin/busybox";
        char install[] = "--install";
        char* argv[] = { busybox, install, NULL };
        execve(busybox, argv, NULL);
        perror("execve");
        exit(errno);
    }

    while (1) {
        int rc, wstat;

        if ((rc = waitpid(install_pid, &wstat, 0)) < 0) {
            return rc;
        }

        if (WIFEXITED(wstat)) {
            return WEXITSTATUS(wstat);
        }
    }
}

void initialize() {
    // remount hard drive as rw and sync
    // sync is needed so that linux does not defer disk writes and MS-DOS can
    // see them immediately
    if (mount("", "/", "vfat", MS_REMOUNT | MS_SYNCHRONOUS, NULL)) {
        fatal("remount root");
    }

    // setup ramdisk for root partition
    // TODO - maybe a persistent ext4 fs on a loop device?

    if (mount("", "/doslinux/rootfs", "ramfs", 0, NULL)) {
        fatal("mount ramfs");
    }

    // setup procfs

    if (mkdir("/doslinux/rootfs/proc", 0755)) {
        fatal("mkdir /proc");
    }

    if (mount("", "/doslinux/rootfs/proc", "proc", 0, NULL)) {
        fatal("mount /proc");
    }

    // setup bin and copy busybox into place

    if (mkdir("/doslinux/rootfs/bin", 0755)) {
        fatal("mkdir /bin");
    }

    if (copy_file("/doslinux/busybox", "/doslinux/rootfs/bin/busybox")) {
        fatal("copy busybox");
    }

    // setup mnt and pivot root

    if (mkdir("/doslinux/rootfs/mnt", 0755)) {
        fatal("mkdir /mnt");
    }

    if (mkdir("/doslinux/rootfs/mnt/c", 0755)) {
        fatal("mkdir /mnt/c");
    }

    if (syscall(SYS_pivot_root, "/doslinux/rootfs", "/doslinux/rootfs/mnt/c")) {
        fatal("pivot_root");
    }

    // setup remaining bin dirs and install busybox

    if (mkdir("/sbin", 0755)) {
        fatal("mkdir /sbin");
    }

    if (mkdir("/usr", 0755)) {
        fatal("mkdir /usr");
    }

    if (mkdir("/usr/bin", 0755)) {
        fatal("mkdir /usr/bin");
    }

    if (mkdir("/usr/sbin", 0755)) {
        fatal("mkdir /usr/sbin");
    }

    if (install_busybox()) {
        fatal("install busybox");
    }

    // setup /dev

    if (mkdir("/dev", 0755)) {
        fatal("mkdir /dev");
    }

    if (mknod("/dev/mem", S_IFCHR | 0600, makedev(1, 1))) {
        fatal("mknod mem");
    }

    if (mknod("/dev/ttyS0", S_IFCHR | 0600, makedev(4, 64))) {
        fatal("mknod ttyS0");
    }
}

int run_vmm() {
    // open /dev/mem for mapping
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) {
        perror("open mem");
        return -1;
    }

    // map the entire first MiB of memory in :)
    if (mmap(0, 0x110000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memfd, 0) == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    vm86_init_t* dos = (void*)0x100000;
    vm86_run(*dos);
}

int run_console() {
    int rc, fd;

    CHECKED(fd = open("/dev/ttyS0", O_RDWR));

    CHECKED(dup2(fd, 0));
    CHECKED(dup2(fd, 1));
    CHECKED(dup2(fd, 2));

    CHECKED(close(fd));

    char sh[] = "sh";
    char* argv[] = { sh, NULL };

    char path[] = "PATH=/usr/bin:/usr/sbin:/bin:/sbin";
    char* envp[] = { path, NULL };
    rc = execve("/bin/busybox", argv, envp);

out:
    return rc;
}

int main() {
    initialize();

    printf(" ok\n");

    pid_t rc;

    // fork control shell
    rc = fork();

    if (rc < 0) {
        perror("fork");
    }

    if (rc == 0) {
        run_console();
        perror("run console");
    }

    // fork vmm
    rc = fork();

    if (rc < 0) {
        perror("fork");
    }

    if (rc == 0) {
        run_vmm();
        perror("run vmm");
    }

    while (1) {
        int wstat;
        int rc = wait(&wstat);
        if (rc < 0) {
            fatal("wait");
        }
    }
}
