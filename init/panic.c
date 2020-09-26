#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "panic.h"

// if init dies the kernel will panic
__attribute__((noreturn)) void
halt()
{
    while (1) {
        sleep(1);
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
