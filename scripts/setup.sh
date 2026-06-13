#!/bin/bash
set -e

ROOT="$(git rev-parse --show-toplevel)"

chmod +x "$ROOT/scripts/config.sh"
chmod +x "$ROOT/scripts/initramfs.sh"
chmod +x "$ROOT/run.sh"

"$ROOT/scripts/config.sh"
"$ROOT/scripts/initramfs.sh"

cd "$ROOT/linux"
make LLVM=1 -j"$(nproc)"
