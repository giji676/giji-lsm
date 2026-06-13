#!/bin/bash

ROOT="$(git rev-parse --show-toplevel)"

qemu-system-x86_64 \
    -kernel "$ROOT/linux/arch/x86/boot/bzImage" \
    -initrd "$ROOT/initramfs.cpio.gz" \
    -append "console=ttyS0 nokaslr" \
    -nographic \
    -m 512M
