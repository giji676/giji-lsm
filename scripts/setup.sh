#!/bin/bash
set -e

ROOT="$(git rev-parse --show-toplevel)"

"$ROOT/scripts/config.sh"
"$ROOT/scripts/initramfs.sh"

cd "$ROOT/linux"
make LLVM=1 -j"$(nproc)"
