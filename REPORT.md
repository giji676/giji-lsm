# Project Objective
The goal of this project was to identify a Linux Security Module (LSM) within the Linux kernel and reimplement part of it in Rust while preserving its behaviour and improving memory safety. Rust provides stronger memory safety guarantees through its ownership model, borrow checker, and type system, eliminating many classes of memory errors at compile time.

The challenge with writing an LSM in Rust is that it must work with the C code that the rest of the kernel is written in. This includes registering a module written in Rust with the LSM framework, or implementing some of the functionality from existing C modules in Rust. Both require tight coupling between Rust and C, including calling C APIs, accessing C data structures, and sharing ownership of kernel objects. This interoperability requires `unsafe` code when interacting with C APIs and raw pointers. These sections cannot be fully verified by the compiler, placing responsibility for memory safety on the programmer.

# Linux Security Modules
LSM is a framework that allows security modules to be added to the Linux kernel. It provides hooks that are called whenever important kernel objects are accessed from userspace, such as `inodes`. Security modules register implementations of these hooks to enforce access control policies, determining whether a requested operation should be permitted or denied.

# The Yama LSM
One of the existing LSMs is Yama, which provides access control for the `ptrace` syscall. `ptrace` allows one process to inspect the memory of another process and is commonly used by debuggers such as `gdb`. While this feature is essential for debugging, it also presents a security risk, as a compromised process may attempt to inspect or manipulate other processes owned by the same user.

Yama mitigates this risk by limiting which processes may use `ptrace`. The restrictions are controlled by `ptrace_scope`, which supports four security levels:
```
#define YAMA_SCOPE_DISABLED     0
#define YAMA_SCOPE_RELATIONAL   1
#define YAMA_SCOPE_CAPABILITY   2
#define YAMA_SCOPE_NO_ATTACH    3
```
Internally, Yama implements four LSM hooks related to process tracing: `ptrace_access_check`, `ptrace_traceme`, `task_prctl`, and `task_free`. Through these hooks, Yama performs capability and relationship checks whenever a `ptrace` request is made. The module also provides support for the `PR_SET_PTRACER` interface, allowing applications to selectively grant debugging permissions while preserving the overall security policy.

Yama was selected because it is a relatively self-contained LSM whose functionality relies heavily on kernel data structures and interactions with existing C code, making it a suitable case study for evaluating Rust integration within the Linux kernel.

For this project, the focus was on reimplementing selected Yama functionality in Rust while preserving the original behaviour. This meant translating existing C policy logic into Rust and adding the supporting pieces needed to call into the kernel, mainly bindings and helpers for tasks, credentials, capabilities, and RCU. LSM and hook registration were left in C. Where interaction with C APIs was unavoidable, the implementation used FFI and `unsafe` code; elsewhere it relied on Rust's type system for stronger safety guarantees.

# Implementation
- Implemented three of Yama's four LSM hooks in Rust:
1. `yama_ptrace_access_check`
2. `yama_ptrace_traceme`
3. `yama_task_free`
- Preserved the original behaviour of the C implementation while integrating the Rust code into the existing LSM framework.
- Implemented Rust equivalents of required kernel infrastructure to support the Yama implementation, including linked-list traversal macros, capability checking, container access, and other kernel utilities.
- Generated Rust bindings and implemented FFI interfaces necessary for interoperability with existing C kernel code.
- Verified that the Rust implementation behaved consistently with the original C implementation through functional testing across the different `ptrace_scope` security levels.

Supporting this implementation required reimplementing several kernel helper functions and macros in Rust, including `list_for_each_entry_rcu`, `container_of`, `list_entry_rcu`, `has_ns_capability`, and additional RCU, capability, and task management utilities that were not yet available through the Rust-for-Linux bindings.

# Example implementation: `yama_ptrace_access_check`
One of the functions migrated during the project was `yama_ptrace_access_check`. This function implements the core Yama attach policy: when a process attempts `PTRACE_ATTACH`, Yama decides whether the attach is allowed according to the current `ptrace_scope` value. It was a suitable case study because it combines several aspects of the migration work in a single function: translating C control flow into Rust, interacting with `task_struct`, using RCU, performing capability checks, calling other helpers, and preserving the original security logic while interoperating with C through FFI.

## Integrating with the existing C hook
The LSM hook table still points at the original C entry point for all four hook functions. Under a `RUST` build flag, those functions immediately delegate to their Rust implementations and return their results, so the original C body is not executed:

```C
static int yama_ptrace_access_check(struct task_struct *child,
                                    unsigned int mode)
{
#ifdef RUST
        return rust_yama_ptrace_access_check(child, mode, ptrace_scope);
#endif
        /* original C implementation follows when RUST is not defined */
        ...
}
```

This keeps LSM registration and hook wiring in C for now, while the policy decision itself runs in Rust. Once Yama's LSM registration is moved to Rust, the Rust function is expected to be registered directly in the hook table rather than being reached through the C wrapper. The `ptrace_scope` value is passed as an argument for simplicity, rather than having to read the global value.

## Original C implementation
In the original Yama LSM, the function has the following form:

```C 
static int yama_ptrace_access_check(struct task_struct *child,
                                    unsigned int mode)
{
        int rc = 0;

        if (mode & PTRACE_MODE_ATTACH) {
                switch (ptrace_scope) {
                case YAMA_SCOPE_DISABLED:
                        break;
                case YAMA_SCOPE_RELATIONAL:
                        rcu_read_lock();
                        if (!pid_alive(child))
                                rc = -EPERM;
                        if (!rc && !task_is_descendant(current, child) &&
                            !ptracer_exception_found(current, child) &&
                            !ns_capable(__task_cred(child)->user_ns,
                                        CAP_SYS_PTRACE))
                                rc = -EPERM;
                        rcu_read_unlock();
                        break;
                case YAMA_SCOPE_CAPABILITY:
                        rcu_read_lock();
                        if (!ns_capable(__task_cred(child)->user_ns,
                                        CAP_SYS_PTRACE))
                                rc = -EPERM;
                        rcu_read_unlock();
                        break;
                case YAMA_SCOPE_NO_ATTACH:
                default:
                        rc = -EPERM;
                        break;
                }
        }

        if (rc && (mode & PTRACE_MODE_NOAUDIT) == 0)
                report_access("attach", child, current);

        return rc;
}
```

The function only applies additional restrictions when `PTRACE_MODE_ATTACH` is set. Under `YAMA_SCOPE_RELATIONAL`, an attach is allowed only if the tracer is an ancestor of the target, an exception has been registered, or the tracer has `CAP_SYS_PTRACE` in the target's user namespace. Under `YAMA_SCOPE_CAPABILITY`, only the capability check remains. Under `YAMA_SCOPE_NO_ATTACH`, all attaches are denied. Failed attaches may also be audited through `report_access`.

## Rust implementation
The corresponding Rust implementation preserves this policy while adapting it to the constraints of the Rust-for-Linux environment:

```Rust
/// yama_ptrace_access_check written in Rust.
///
/// # Safety
///
/// `child` must be a valid `task_struct` pointer, valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_yama_ptrace_access_check(
    child: *mut bindings::task_struct,
    mode: ffi::c_uint,
    ptrace_scope: ffi::c_int
) -> ffi::c_int {
    let mut rc: ffi::c_int = 0;

    let current = current!();
    let current_ptr = current.as_ptr();

    if mode & bindings::PTRACE_MODE_ATTACH != 0 {
        match ptrace_scope {
            val if val == bindings::YAMA_SCOPE_DISABLED as ffi::c_int => {}
            val if val == bindings::YAMA_SCOPE_RELATIONAL as ffi::c_int => {
                let _guard = read_lock();
                // SAFETY: `child` valid per this function's contract; RCU
                // held via `_guard` for the whole block; `current_ptr` is
                // the running task, always valid.
                unsafe {
                    if !pid_alive(child) {
                        rc = -(bindings::EPERM as ffi::c_int);
                    }

                    if rc == 0
                        && !task_is_descendant(current_ptr, child)
                        && !ptracer_exception_found(current_ptr, child)
                        && !bindings::ns_capable(
                            (*bindings::rust_task_cred(child)).user_ns,
                            bindings::CAP_SYS_PTRACE as i32,
                        )
                    {
                        rc = -(bindings::EPERM as ffi::c_int);
                    }
                }
            }
            val if val == bindings::YAMA_SCOPE_CAPABILITY as ffi::c_int => {
                let _guard = read_lock();
                // SAFETY: `child` valid per this function's contract; RCU
                // held via `_guard`.
                unsafe {
                    if !bindings::ns_capable(
                        (*bindings::rust_task_cred(child)).user_ns,
                        bindings::CAP_SYS_PTRACE as i32,
                    ) {
                        rc = -(bindings::EPERM as ffi::c_int);
                    }
                }
            }
            val if val == bindings::YAMA_SCOPE_NO_ATTACH as ffi::c_int => {
                rc = -(bindings::EPERM as ffi::c_int);
            }
            _ => {
                rc = -(bindings::EPERM as ffi::c_int);
            }
        }
    }

    if rc != 0 && (mode & bindings::PTRACE_MODE_NOAUDIT) == 0 {
        // SAFETY: `child` valid per this function's contract;
        // `_guard` holds `child->alloc_lock` for the duration of this call.
        let _guard = unsafe { TaskLockGuard::new(child) };
        unsafe {
            rust_report_access(
                c"attach".as_ptr().cast::<u8>(),
                child,
                current_ptr,
            );
        }
    }

    rc
}
```

## Translation and safety considerations
Several aspects of the translation show the practical challenges of writing kernel security logic in Rust.

**Raw pointers and `unsafe`**. `struct task_struct *child` becomes `*mut bindings::task_struct`. All Rust functions that get called from C need to be marked `unsafe extern "C"`. Dereferencing C pointers requires `unsafe`, since the compiler cannot verify lifetimes of C-managed kernel objects.

**Current task and credentials**. The running task is obtained via `current!()` and converted to a raw pointer for helpers that still use C-style `task_struct` pointers. Capability checks use bindings such as `rust_task_cred` and `ns_capable` in place of `__task_cred` and direct C field access.

**RCU and auditing**. Relational and capability scopes use `read_lock()`, a proposed abstraction around `rcu_read_lock()` that is not yet in released kernels. The lock is released when the guard goes out of scope, or earlier via `Guard::unlock()`. Denied attaches call `rust_report_access` under a `TaskLockGuard`, adopted from the Rust-for-Linux RCU lock example, which is dropped in the same way when it leaves scope.

**C versus Rust boundary**. Higher-level policy and helpers such as `task_is_descendant` and `ptracer_exception_found` were moved to Rust. Capability checks and some credential/task accessors remain FFI calls into C. Hook registration is still C-side; only the policy decision runs in Rust until registration itself is migrated.

The Rust version is therefore not a line-for-line rewrite: the security decisions match, but locking, pointer validity, and FFI contracts are made more explicit through contracts, `SAFETY` comments, and scoped guards.

# Testing
To verify the functionality of the Rust implementation, I designed a small testing framework to allow quick and easy addition of tests, and reusability of setup functions.

As Yama's primary functionality is around one process `ptrace`-ing another process, the framework spawns two processes per test case, with different setups. Test cases are defined as a list of structs, each one having a child and parent setup function pointers, their expected return values, and Yama scope.

The test framework first sets the Yama scope by writing to `/proc/sys/kernel/yama/ptrace_scope`. Then spawns the child and parent processes. The two processes communicate via pipes to signal ready, commands and results. After the processes have been spawned the child process setup is run, and the parent waits for that to complete. The child setup can involve things like calling `PTRACE_TRACEME`, setting `PR_SET_PTRACER` and more. After the child finishes setup the parent setup gets run, which might attempt the `PTRACE_ATTACH` with various capabilities.

The results of these child and parent functions are what get tested. For example, if a child tries to set `PR_SET_PTRACER` with an invalid `PID` an error is expected, or if a parent tries to attach to the child process without the right capability, permission will be denied.

This allows multiple test cases to share setup functions, for example to test the same Yama function with different scopes, all you need to do is copy the test case struct, and change the `.scope` field.

There is a problem with the framework however that I was not able to fix. Setting the `YAMA_SCOPE_NO_ATTACH` as the scope causes any future scope changes to be invalid, therefore it can't be changed back to lower scopes. Because of this, I've structured the test cases so they increase in scope, with the last (highest level) being the no attach scope. It also means that re-running the test program fails as it can't start from the low scope, so restart of the kernel is required - which is fast to do through QEMU so it didn't hinder development.

# Project Outcomes
- Successfully reimplemented 3 of the 4 LSM hooks used by Yama in Rust while preserving the behaviour of the original C implementation.
- Demonstrated that existing Linux Security Module functionality can be migrated to Rust, while working with the surrounding C kernel code.
- Developed the Rust infrastructure required to support the Yama implementation, identifying gaps in the current Rust-for-Linux support that required additional helper functions, macros, and bindings.
- Verified the Rust implementation against the original C implementation using a custom test suite covering the different `ptrace_scope` security levels and `PR_SET_PTRACER` behaviour.
- Identified the kernel interfaces that still require `unsafe` code, illustrating the current limitations of writing Linux kernel components in Rust and the areas where additional safe abstractions would be beneficial.

# Future Work
- Implement the remaining Yama LSM hook, `task_prctl`, in Rust to complete the migration of all Yama-specific LSM hooks.
- Continue the ongoing migration of the Yama LSM to Rust by moving the remaining components, including LSM registration, hook registration, and Yama-specific data structures. Although this work extends beyond the scope of the original project, it is currently in progress.
- Continue developing safer Rust abstractions for Read-Copy Update (RCU) operations. In particular, investigate using Rust's lifetime system to guarantee that an RCU read lock is held whenever RCU-protected data is accessed, reducing the potential for incorrect API usage and minimising reliance on `unsafe` code.
- Replace additional C helper functions and wrappers with safe Rust abstractions where possible, further reducing the reliance on `unsafe` code.
- Increase automated test coverage to include additional `ptrace` scenarios, edge cases, and regression tests.
- Evaluate the performance impact of the Rust implementation compared to the original C implementation.

# Dependency Analysis
When migrating kernel code to Rust, dependencies can either be reimplemented in Rust or exposed through bindings to existing C functions. The Yama implementation provided examples of both approaches. Higher-level abstractions such as `list_for_each_entry_rcu`, `list_entry_rcu`, and `container_of` were implemented mainly in Rust, with only the low-level operations delegated to existing C functions through bindings. For example, the iteration logic, type checking, pointer arithmetic, and macro expansion are implemented in Rust, while operations such as `rust_read_once` ultimately call the existing C implementation to perform the underlying memory access. This approach allows the majority of the implementation to benefit from Rust's type system while reusing existing kernel primitives where necessary.

In contrast, lower-level primitives such as `rcu_dereference` required Rust bindings to the existing C implementation. Although these bindings allow the functionality to be called from Rust, the underlying operation is still performed by C code. As a result, Rust provides limited additional safety for them, with most of the safety guarantees still depending on the correctness of the underlying C implementation.

The following figure presents a simplified dependency tree for `yama_ptrace_access_check`. Many lower-level dependencies have been omitted for clarity; the purpose of the figure is to illustrate the overall structure of the implementation rather than provide a complete call graph. The diagram distinguishes between functions, macros, and identifies whether each dependency originates from the Yama LSM or the wider Linux kernel.

[Figure here]

Performing this dependency analysis before implementing a kernel component in Rust provides a useful indication of the effort required for migration. Higher-level functionality implemented within the LSM can often be reimplemented directly in Rust, whereas dependencies on generic kernel infrastructure introduce additional complexity. By following the dependency tree, it becomes possible to identify which components are likely to require new Rust implementations, which can be reused through bindings, and which low-level kernel primitives or macros are likely to remain implemented in C.

This analysis also helps assess the potential safety improvements that can be achieved through migration. Dependencies that ultimately resolve to low-level kernel primitives, such as memory access or RCU operations, are difficult to replace directly and typically require safe Rust abstractions over existing C implementations. Consequently, the greatest safety benefits are obtained by reimplementing higher-level control flow and data structure manipulation in Rust while encapsulating unavoidable interactions with low-level kernel functionality behind well-defined interfaces.

# Reflection
At the beginning of the project, I had relatively limited experience with Rust. One of the main areas I developed was my understanding of the Rust borrow checker, ownership, and lifetimes. The project also gave me experience of working on a large, established codebase rather than developing a standalone application, which was a valuable change from many of my previous programming projects.

I also developed my understanding of the Linux kernel considerably. Linux had already been an area of interest for me before starting the project, and I had previously experimented with writing a Linux kernel driver for a macro-pad. This gave me some initial familiarity with concepts such as kernel modules, kernel APIs, and the difference between kernel-space and user-space code. Working with Linux Security Modules built on this knowledge and introduced me to a different part of the kernel. Rather than interacting primarily with hardware or device interfaces, I was working with security hooks and examining how security decisions are made within the kernel. Investigating Yama's ptrace restrictions also required me to understand how different kernel components interact, including the capability system, LSM hooks, process relationships, credentials, and RCU-protected data structures.

Another important area of learning was the interaction between C and Rust. Since the existing Yama implementation was written in C while the work involved implementing equivalent functionality in Rust, I had to understand how the two languages could work together through FFI. This involved learning more about Rust's foreign-function interface, kernel bindings, C-compatible types, and the considerations involved when passing kernel data structures between C and Rust. This was particularly interesting because it showed that introducing Rust into an existing C codebase does not necessarily mean rewriting the entire system. Instead, the two languages can work together by sharing resources and calling each other's functions.

The project also gave me experience with investigating software behaviour rather than relying solely on documentation or source-code inspection. I used debugging output, kernel instrumentation, call traces, and user-space test programs to determine which kernel paths were actually being executed. This was particularly useful when investigating the interaction between multiple LSM hooks. In some cases, the behaviour observed from user space was not immediately explained by the function being investigated, requiring me to trace the surrounding call path and understand the ordering of the security checks. This developed my ability to approach unfamiliar systems by forming hypotheses, testing them, and using the results to refine my understanding.

More generally, the UROP experience gave me a better understanding of what working on a research-oriented software project involves. I had to spend time reading existing code, investigating unfamiliar kernel mechanisms, designing experiments, and determining whether observed behaviour was caused by my implementation or by another part of the system. This made the project more challenging, but also made the process of reaching a working result more valuable.

Overall, the project has increased my interest in systems programming and low-level software development. It has given me practical experience with Rust and has strengthened my understanding of the Linux kernel and security mechanisms. It has shown me the value of understanding how software works internally rather than treating systems as black boxes.

# Conclusion
This project demonstrated that components of Linux Security Modules can be successfully reimplemented in Rust, while preserving the behaviour of the original C implementation. Three out of four of Yama's LSM hooks were migrated to Rust, including the supporting kernel functions.

The project also highlighted the practical challenges of introducing Rust into a C codebase. While higher-level kernel functionality could be expressed safely in Rust, lower-level primitives such as RCU operations and memory access mechanisms still relied on existing C implementations accessed through bindings.

Finally, the dependency analysis demonstrated that examining the dependency hierarchy of a kernel component provides a useful indication of both the implementation effort required and the potential safety improvements that can be achieved through migration. These observations may help guide future efforts to expand Rust support within the Linux kernel.

# Links
Project GitHub repo: [giji-lsm](https://github.com/giji676/giji-lsm)
My fork of Linux: [Linux-fork](https://github.com/giji676/linux/tree/giji-lsm)
