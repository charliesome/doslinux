# DOS Subsystem for Linux

A WSL alternative for users who prefer an MS-DOS environment. DOS Subsystem for Linux integrates a real Linux environment into MS-DOS systems, allowing users to make use of both DOS and Linux applications from the DOS command prompt.

![](https://charlie.su/recording-ac5e9af596a918.gif)

## Building

* You will need a cross toolchain targeting `i386-linux-musl` on `PATH`.

  https://github.com/richfelker/musl-cross-make is a tool that can build one for you with minimal hassle. Set `TARGET` to `i386-linux-musl`.

* Build the prequisites (Linux and Busybox) by running `J=xxx script/build-prereq`, replacing `xxx` with the desired build parallelism.

* You will need a hard drive image `hdd.base.img` with an installed copy of MS-DOS on the first partition.

* Run `make`

  This will produce a new hard drive image `hdd.img` with DOS Subsystem for Linux installed. Invoke `C:\doslinux\dsl <command>` to run Linux commands. `C:\doslinux` can also be placed on your DOS `PATH` for greater convenience.
