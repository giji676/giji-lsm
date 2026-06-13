#!/bin/bash
set -e

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT/linux"

make LLVM=1 allnoconfig

scripts/config --enable CONFIG_64BIT
scripts/config --enable CONFIG_BLK_DEV_INITRD
scripts/config --enable CONFIG_DEVTMPFS
scripts/config --enable CONFIG_DEVTMPFS_MOUNT
scripts/config --enable CONFIG_TTY
scripts/config --enable CONFIG_SERIAL_8250
scripts/config --enable CONFIG_SERIAL_8250_CONSOLE
scripts/config --enable CONFIG_BINFMT_ELF
scripts/config --enable CONFIG_BINFMT_SCRIPT
scripts/config --enable CONFIG_MODULES
scripts/config --enable CONFIG_MODULE_UNLOAD
scripts/config --enable CONFIG_PRINTK
scripts/config --enable CONFIG_RUST
scripts/config --enable CONFIG_SAMPLES
scripts/config --enable CONFIG_SAMPLES_RUST
scripts/config --enable CONFIG_SECURITY
scripts/config --enable CONFIG_SECURITY_GIJI

make LLVM=1 olddefconfig
