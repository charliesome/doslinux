# DOS Subsystem for Linux

A WSL alternative for users who prefer an MS-DOS environment.

_Note:  This project runs Linux under DOS, it does not run DOS under Linux.  This project follows the naming convention of WSL (Windows Subsystem for Linux)._

![](https://charlie.su/screenshot-ff6abe24cd4858.png)

## Videos

* [Basic commands](https://charlie.su/doslinux_demo-f5c2031c25d47a.mp4)

* [Two way filesystem access](https://charlie.su/doslinux_two_way_filesystem-35aadae02ad5ec.mp4)

## Building

* You will need a cross toolchain targeting `i386-linux-musl` on `PATH`.

  https://github.com/richfelker/musl-cross-make is a tool that can build one for you with minimal hassle. Set `TARGET` to `i386-linux-musl`.

* Build the prequisites (Linux and Busybox) by running `J=xxx script/build-prereq`, replacing `xxx` with the desired build parallelism.

* You will need a hard drive image `hdd.base.img` with an installed copy of MS-DOS on the first partition.

* Run `make`

  This will produce a new hard drive image `hdd.img` with DOS Subsystem for Linux installed. Invoke `C:\doslinux\dsl <command>` to run Linux commands. `C:\doslinux` can also be placed on your DOS `PATH` for greater convenience.
