#!/bin/bash
set -e

cd "linux"
make LLVM=1 -j"$(nproc)"
