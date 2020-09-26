#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "panic.h"

__attribute__((noreturn)) void
halt()
{
    if (getpid() == 1) {
        // if init dies the kernel will panic
        while (1) {
            sleep(1);
        }
    } else {
        exit(EXIT_FAILURE);
    }
}

__attribute__((noreturn)) void
fatal(const char* msg)
{
    perror(msg);
    halt();
}

__attribute__((noreturn)) void
panic(const char* msg)
{
    printf("panic: %s\n", msg);
    halt();
}
