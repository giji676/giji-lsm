# Setup
## Cloning the repo properly with submodules
`git clone --recurse-submodules https://github.com/giji676/giji-lsm.git`

`./scripts/submodule_setup.sh`

## Setting up the environment
Mainly adapted from https://medium.com/@sumant1122/writing-your-first-rust-linux-kernel-module-from-zero-to-insmod-d49669eb6737

### System dependencies
```bash
sudo apt update && sudo apt install -y \
  git curl build-essential flex bison \
  libssl-dev libelf-dev libncurses-dev \
  qemu-system-x86 cpio busybox-static \
  python3 python3-pip clang llvm lld \
  bc pahole zstd
```

### Rust install
```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
# Choose option 1 (default install)

source $HOME/.cargo/env

rustc --version
cargo --version

rustup component add rust-src rustfmt clippy
cargo install bindgen-cli
```

### Setting up qemu
`sudo apt install qemu-system-x86`

## Build
### Verify Rust is detected by the kernel build system
`linux$ make LLVM=1 rustavailable` - should show `Rust is available!`

### setup
`$ ./scripts/setup.sh`

## Compiling the kernel
`linux$ make LLVM=1 -j$(nproc)`

## Running the kernel
`$ ./run.sh`

# Viewing changes made
To view the full changes made to add the LSM run: </br>
`linux$ git diff origin/master` - for full diff </br>
`linux$ git diff origin/master --names-only` - for only the names of the files affected
