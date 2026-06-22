# General Information
(summary of https://docs.kernel.org/rust/general-information.html)
## Code documentation
## Abstractions vs. bindings
### Bindings
`bindgen` is used to generate Rust bindings for C. This is done by including a C header from `include/` in `rust/bindings/bindings_helper.h`. `bindgen` then automatically generates the bindings in the `*_generated.rs` file in the `rust/bindings/` folder. For C features that bindgen cannot generate (e.g. inline functions or complex macros), small C wrapper functions can be added under `rust/helpers/`.

### Abstractions
Abstractions wrap the auto-generated, unsafe bindings in sound, ergonomic, and as-safe-as-possible Rust APIs. They are located in `rust/kernel/`. These abstractions are what Rust kernel modules should use when interacting with kernel functionality, rather than accessing the bindings directly. The goal is to encapsulate all direct interaction with the C kernel APIs inside carefully reviewed abstractions. As a result, Drivers and other end-user kernel components ("leaf" modules) should use the abstractions rather than interacting with the bindings directly.

#### Key properties of abstractions:
- Soundness: if the abstraction is implemented correctly, safe Rust code using it cannot cause undefined behaviour.
- Ergonomics: the abstraction should feel like idiomatic Rust, for example: using constructors/destructors instead of manual init/free, returning Result<T> instead of integer error codes, using ownership  instead of explicit cleanup.

Together, bindings provide access to the C kernel APIs, while abstractions
provide the safe, idiomatic interface that Rust kernel code is expected to use.

### Bindings and abstractions usage diagram
(taken from https://docs.kernel.org/rust/general-information.html)

```
                                                rust/bindings/
                                               (rust/helpers/)

                                                   include/ -----+ <-+
                                                                 |   |
  drivers/              rust/kernel/              +----------+ <-+   |
    fs/                                           | bindgen  |       |
   .../            +-------------------+          +----------+ --+   |
                   |    Abstractions   |                         |   |
+---------+        | +------+ +------+ |          +----------+   |   |
| my_foo  | -----> | | foo  | | bar  | | -------> | Bindings | <-+   |
| driver  |  Safe  | | sub- | | sub- | |  Unsafe  |          |       |
+---------+        | |system| |system| |          | bindings | <-----+
     |             | +------+ +------+ |          |  crate   |       |
     |             |   kernel crate    |          +----------+       |
     |             +-------------------+                             |
     |                                                               |
     +------------------# FORBIDDEN #--------------------------------+
 ```

# Coding Guidelines
# Testing
