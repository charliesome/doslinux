#ifndef PANIC_H
#define PANIC_H

// prints errno based message and panics
__attribute__((noreturn)) void
fatal(const char* msg);

// panics with a simple message, no errno
__attribute__((noreturn)) void
panic(const char* msg);

// halts machine, no message
__attribute__((noreturn)) void
halt();

#endif
