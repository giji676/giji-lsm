#!/bin/bash
set -e

ROOT="$(git rev-parse --show-toplevel)"

rm -rf "$ROOT/initramfs"
rm -f "$ROOT/initramfs.cpio.gz"

mkdir -p "$ROOT/initramfs"/{bin,sbin,dev,proc,sys,tmp}

gcc -g -static test.c -o tests

cp tests initramfs/bin/
chmod +x initramfs/bin/tests

# Copy the busybox
cp /usr/bin/busybox "$ROOT/initramfs/bin/"

# Create symlinks for commands our init script needs
cd "$ROOT/initramfs/bin"
ln -s busybox sh
ln -s busybox ls
ln -s busybox mount
cd "$ROOT/"

# Create the init script (PID 1, the first process the kernel runs)
cat > "$ROOT/initramfs/init" <<'EOF'
#!/bin/sh

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

echo "=== Rust Kernel Boot Success ==="
echo "Kernel: $(uname -r)"
exec /bin/sh
EOF

chmod +x "$ROOT/initramfs/init"

# Package it as a cpio archive
cd "$ROOT/initramfs"
find . | cpio -H newc -o | gzip > "$ROOT/initramfs.cpio.gz"
cd ..
