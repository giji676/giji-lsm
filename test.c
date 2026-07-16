#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/capability.h>
#include <sys/syscall.h>

#define YAMA_SCOPE_DISABLED     0
#define YAMA_SCOPE_RELATIONAL   1
#define YAMA_SCOPE_CAPABILITY   2
#define YAMA_SCOPE_NO_ATTACH    3

static void set_ptrace_scope(int scope)
{
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d\n", scope);

    int fd = open("/proc/sys/kernel/yama/ptrace_scope", O_WRONLY);
    if (fd < 0) {
        perror("open ptrace_scope");
        exit(EXIT_FAILURE);
    }

    if (write(fd, buf, len) != len) {
        perror("write ptrace_scope");
        close(fd);
        exit(EXIT_FAILURE);
    }

    close(fd);

    printf("\n==================================================\n");
    printf("ptrace_scope = %d\n", scope);
    printf("==================================================\n");
}

/* ---------------------------------------------------------- */
/* Expectations                                               */
/* ---------------------------------------------------------- */

struct expect {
    bool check;   /* false = don't verify this side at all */
    int rc;       /* expected return value: 0 or -1 */
    int err;      /* expected errno, only meaningful when rc == -1 */
};

#define EXPECT_NONE        ((struct expect){ .check = false })
#define EXPECT_OK          ((struct expect){ .check = true, .rc = 0 })
#define EXPECT_DENIED(e)   ((struct expect){ .check = true, .rc = -1, .err = (e) })

struct result {
    int rc;
    int err;
};

static bool result_matches(const struct expect *exp, const struct result *got)
{
    if (!exp->check)
        return true; /* not checked -> trivially "pass" */
    if (got->rc != exp->rc)
        return false;
    if (exp->rc == -1 && got->err != exp->err)
        return false;
    return true;
}

static void print_result(const char *label, const struct expect *exp,
        const struct result *got)
{
    if (!exp->check) {
        printf("  %-8s rc=%d errno=%d (%s)  [not checked]\n",
                label, got->rc, got->err, strerror(got->err));
        return;
    }

    bool pass = result_matches(exp, got);
    printf("  %-8s rc=%d errno=%d (%s)  expected rc=%d%s%s -> %s\n",
            label, got->rc, got->err, strerror(got->err),
            exp->rc,
            exp->rc == -1 ? " errno=" : "",
            exp->rc == -1 ? strerror(exp->err) : "",
            pass ? "PASS" : "FAIL");
}

/* ---------------------------------------------------------- */
/* Test framework                                             */
/* ---------------------------------------------------------- */

enum command {
    CMD_RUN,
    CMD_EXIT,
};

static void send_cmd(int fd, enum command cmd)
{
    if (write(fd, &cmd, sizeof(cmd)) != sizeof(cmd)) {
        perror("write");
        exit(1);
    }
}

static enum command recv_cmd(int fd)
{
    enum command cmd;

    if (read(fd, &cmd, sizeof(cmd)) != sizeof(cmd)) {
        perror("read");
        exit(1);
    }

    return cmd;
}

static void signal_ready(int ready_fd)
{
    char byte = 1;
    write(ready_fd, &byte, 1);
}

struct test_case {
    const char *name;
    int scope;

    /* Child performs its own ptrace call (e.g. PTRACE_TRACEME) and reports
     * the result over `result_fd`. May be NULL if the child doesn't perform a
     * ptrace call under test (e.g. it's just the attach target).
     *
     * `ready_fd`: child must write one byte here once it has
     * completed any setup that `parent` depends on (e.g. PR_SET_PTRACER), and
     * before it does anything that `parent`'s ptrace call is meant to
     * observe. run_test() blocks on this before calling `parent`. */
    void (*child)(int result_fd, int cmd_fd, int ready_fd);

    /* Parent performs its own ptrace call (e.g. PTRACE_ATTACH) and returns
     * the result directly, since it runs in the harness's own process. */
    struct result (*parent)(pid_t child, int cmd_fd);
    
    struct expect expect_child;
    struct expect expect_parent;
};

static void run_test(const struct test_case *test)
{
    printf("\n---- %s ----\n", test->name);

    set_ptrace_scope(test->scope);

    int cmd_pipe[2];
    int result_pipe[2];
    int ready_pipe[2];

    if (pipe(cmd_pipe) < 0 || pipe(result_pipe) < 0 || pipe(ready_pipe)) {
        perror("pipe");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        /* Child */

        close(cmd_pipe[1]);    /* child only reads commands */
        close(result_pipe[0]); /* child only writes results */
        close(ready_pipe[0]);  /* child only writes ready status */

        if (test->child)
            test->child(result_pipe[1], cmd_pipe[0], ready_pipe[1]);

        close(cmd_pipe[0]);
        close(result_pipe[1]);
        close(ready_pipe[1]);

        _exit(EXIT_SUCCESS);
    }

    /* Parent */

    close(cmd_pipe[0]);    /* parent only writes commands */
    close(result_pipe[1]); /* parent only reads results */
    close(ready_pipe[1]);  /* parent only reads ready status */

    /* Block until the child has finished any setup it needs to do before
     * `parent`'s ptrace call is meaningful. */
    char ready;
    ssize_t rn = read(ready_pipe[0], &ready, 1);
    close(ready_pipe[0]);
    if (rn != 1)
        printf("  (child never signaled ready)\n");

    struct result parent_res = {0, 0};

    if (test->parent)
        parent_res = test->parent(pid, cmd_pipe[1]);

    close(cmd_pipe[1]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status))
        printf("Child exited: %d\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("Child killed by signal %d\n", WTERMSIG(status));

    struct result child_res = {0, 0};

    if (test->expect_child.check) {
        ssize_t n = read(result_pipe[0], &child_res, sizeof(child_res));

        if (n != sizeof(child_res)) {
            printf("  (child never reported a result)\n");
            child_res.rc = -1;
            child_res.err = 0;
        }
    }

    close(result_pipe[0]);

    print_result("child", &test->expect_child, &child_res);
    print_result("parent", &test->expect_parent, &parent_res);
}

static void drop_cap_sys_ptrace(void)
{
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,  // 0 = this process
    };
    struct __user_cap_data_struct data[2];

    // Get current capability sets
    if (syscall(SYS_capget, &hdr, data) < 0) {
        perror("capget");
        exit(1);
    }

    data[0].effective   &= ~(1U << CAP_SYS_PTRACE);
    data[0].permitted   &= ~(1U << CAP_SYS_PTRACE);
    data[0].inheritable &= ~(1U << CAP_SYS_PTRACE);

    if (syscall(SYS_capset, &hdr, data) < 0) {
        perror("capset");
        exit(1);
    }
}

/* ---------------------------------------------------------- */
/* Child helpers                                              */
/* ---------------------------------------------------------- */

static void report(int result_fd, int rc)
{
    struct result res = { .rc = rc, .err = errno };
    write(result_fd, &res, sizeof(res));
}

static void child_traceme(int result_fd, int cmd_fd, int ready_fd)
{
    (void)cmd_fd;
    signal_ready(ready_fd);
    int rc = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
    report(result_fd, rc);
}

static void child_pause(int result_fd, int cmd_fd, int ready_fd)
{
    (void)result_fd;
    signal_ready(ready_fd);

    for (;;) {
        switch (recv_cmd(cmd_fd)) {
        case CMD_RUN:
            return;
        case CMD_EXIT:
            _exit(0);
        default:
            break;
        }
    }
}

static void child_pr_set_ptracer(int result_fd, int cmd_fd, int ready_fd)
{
    (void)result_fd;

    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    signal_ready(ready_fd);

    for (;;) {
        switch (recv_cmd(cmd_fd)) {
        case CMD_RUN:
            return;
        case CMD_EXIT:
            _exit(0);
        default:
            break;
        }
    }
}

static void child_capability_yes(int result_fd, int cmd_fd)
{
    (void)result_fd;
    /* TODO: arrange CAP_SYS_PTRACE, attempt attach */
}

static void child_capability_no(int result_fd, int cmd_fd)
{
    (void)result_fd;
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,  // 0 = operate on this process
    };
    struct __user_cap_data_struct data[2];

    // Get current capability sets
    if (syscall(SYS_capget, &hdr, data) < 0) {
        perror("capget");
        return;
    }

    data[0].effective   &= ~(1U << CAP_SYS_PTRACE);
    data[0].permitted   &= ~(1U << CAP_SYS_PTRACE);
    data[0].inheritable &= ~(1U << CAP_SYS_PTRACE);

    if (syscall(SYS_capset, &hdr, data) < 0) {
        perror("capset");
        return;
    }
}

static void child_traceme_parent_no_cap(int result_fd, int cmd_fd, int ready_fd)
{
    (void)cmd_fd;

    drop_cap_sys_ptrace();

    pid_t inner = fork();
    if (inner < 0) {
        perror("fork (inner)");
        signal_ready(ready_fd);
        struct result r = { .rc = -1, .err = EIO };
        write(result_fd, &r, sizeof(r));
        _exit(1);
    }

    if (inner == 0) {
        /* Innermost process: the actual PTRACE_TRACEME caller. */
        int rc = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        report(result_fd, rc);
        _exit(0);
    }

    /* This process reaps the innermost child directly, so run_test()'s
     * own waitpid() on the top-level fork isn't left waiting on a
     * grandchild it doesn't know about. */
    waitpid(inner, NULL, 0);
    _exit(0);
}

/* ---------------------------------------------------------- */
/* Parent helpers                                             */
/* ---------------------------------------------------------- */

static struct result parent_do_nothing(pid_t child, int cmd_fd)
{
    (void)child;
    (void)cmd_fd;

    return (struct result){0, 0};
}

static struct result do_attach_in_subprocess(pid_t target, bool drop_cap, int cmd_fd)
{
    int result_pipe[2];
    if (pipe(result_pipe) < 0) {
        perror("pipe");
        return (struct result){ -1, EIO };
    }

    pid_t tracer_pid = fork();
    if (tracer_pid < 0) {
        perror("fork");
        return (struct result){ -1, EIO };
    }

    if (tracer_pid == 0) {
        /* Throwaway subprocess: any capability drop here dies with it,
         * and doesn't affects the harness's own process. */
        close(result_pipe[0]);

        if (drop_cap)
            drop_cap_sys_ptrace();

        struct result r;
        r.rc = ptrace(PTRACE_ATTACH, target, NULL, NULL);
        r.err = errno;

        if (r.rc == 0) {
            waitpid(target, NULL, 0);
            ptrace(PTRACE_DETACH, target, NULL, NULL);
        }

        write(result_pipe[1], &r, sizeof(r));
        close(result_pipe[1]);
        _exit(0);
    }

    /* Harness process - capabilities untouched. */
    close(result_pipe[1]);
    struct result r = { -1, 0 };
    ssize_t n = read(result_pipe[0], &r, sizeof(r));
    if (n != sizeof(r)) {
        r.rc = -1;
        r.err = EIO;
    }
    close(result_pipe[0]);
    waitpid(tracer_pid, NULL, 0);

    send_cmd(cmd_fd, CMD_EXIT);
    return r;
}

static struct result parent_try_attach(pid_t child, int cmd_fd)
{
    return do_attach_in_subprocess(child, false, cmd_fd);
}

static struct result parent_try_attach_no_cap(pid_t child, int cmd_fd)
{
    return do_attach_in_subprocess(child, true, cmd_fd);
}

/* ---------------------------------------------------------- */
/* Test list                                                  */
/* ---------------------------------------------------------- */

static struct test_case tests[] = {

    /* Scope 0 */
    {
        .name = "DISABLED: PTRACE_TRACEME",
        .scope = YAMA_SCOPE_DISABLED,
        .child = child_traceme,
        .parent = parent_do_nothing,
        .expect_child = EXPECT_OK,
        .expect_parent = EXPECT_NONE,
    },
    {
        .name = "DISABLED: PTRACE_ATTACH",
        .scope = YAMA_SCOPE_DISABLED,
        .child = child_pause,
        .parent = parent_try_attach,
        .expect_child = EXPECT_NONE,
        .expect_parent = EXPECT_OK,
    },

    /* Scope 1 */
    {
        .name = "RELATIONAL: PTRACE_TRACEME",
        .scope = YAMA_SCOPE_RELATIONAL,
        .child = child_traceme,
        .parent = parent_do_nothing,
        .expect_child = EXPECT_OK,
        .expect_parent = EXPECT_NONE,
    },
    {
        .name = "RELATIONAL: parent attach",
        .scope = YAMA_SCOPE_RELATIONAL,
        .child = child_pause,
        .parent = parent_try_attach,
        .expect_child = EXPECT_NONE,
        .expect_parent = EXPECT_OK,
    },
    {
        .name = "RELATIONAL: unrelated tracer, no exception, no cap",
        .scope = YAMA_SCOPE_RELATIONAL,
        .child = child_pause,
        .parent = parent_try_attach_no_cap,
        .expect_child = EXPECT_NONE,
        .expect_parent = EXPECT_DENIED(EPERM),
    },
    {
        .name = "RELATIONAL: PR_SET_PTRACER exception, tracer lacks cap",
        .scope = YAMA_SCOPE_RELATIONAL,
        .child = child_pr_set_ptracer,
        .parent = parent_try_attach_no_cap,
        .expect_child = EXPECT_NONE,
        .expect_parent = EXPECT_OK,
    },

    /* Scope 2 */
    {
        .name = "CAPABILITY: tracer has CAP_SYS_PTRACE",
        .scope = YAMA_SCOPE_CAPABILITY,
        .child = child_pause,
        .parent = parent_try_attach,
        .expect_child = EXPECT_NONE,
        .expect_parent = EXPECT_OK,
    },
    {
        .name = "CAPABILITY: tracer lacks CAP_SYS_PTRACE",
        .scope = YAMA_SCOPE_CAPABILITY,
        .child = child_pause,
        .parent = parent_try_attach_no_cap,
        .expect_child = EXPECT_NONE,
        .expect_parent = EXPECT_DENIED(EPERM),
    },
    {
        .name = "CAPABILITY: PTRACE_TRACEME, parent lacks CAP_SYS_PTRACE",
        .scope = YAMA_SCOPE_CAPABILITY,
        .child = child_traceme_parent_no_cap,
        .parent = parent_do_nothing,
        .expect_child = EXPECT_DENIED(EPERM),
        .expect_parent = EXPECT_NONE,
    },

    /* Scope 3 */
    {
        .name = "NO_ATTACH: PTRACE_TRACEME",
        .scope = YAMA_SCOPE_NO_ATTACH,
        .child = child_traceme,
        .parent = parent_do_nothing,
        .expect_child = EXPECT_DENIED(EPERM),
        .expect_parent = EXPECT_NONE,
    },
    {
        .name = "NO_ATTACH: PTRACE_ATTACH",
        .scope = YAMA_SCOPE_NO_ATTACH,
        .child = child_pause,
        .parent = parent_try_attach,
        .expect_child = EXPECT_NONE,
        .expect_parent = EXPECT_DENIED(EPERM),
    },
};

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    size_t count = sizeof(tests) / sizeof(tests[0]);

    for (size_t i = 0; i < count; i++)
        run_test(&tests[i]);

    return 0;
}
