#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/app_runtime.h"

static AppRuntime* g_app = nullptr;

static inline pid_t get_tid() {
    return (pid_t)syscall(SYS_gettid);
}

static void on_sigint(int) {
    if (g_app) g_app->requestStop();
}

// 崩溃时直接把调用栈打印到 stderr（不需要 gdb）
static void crash_handler(int sig, siginfo_t* /*info*/, void* /*uctx*/) {
    char buf[256];
    int n = std::snprintf(buf, sizeof(buf), "\n[FATAL] signal=%d tid=%d\n", sig, (int)get_tid());
    if (n > 0) (void)!write(STDERR_FILENO, buf, (size_t)n);

    void* stack[64];
    int sz = backtrace(stack, 64);
    backtrace_symbols_fd(stack, sz, STDERR_FILENO);

    _exit(128 + sig);
}

static void install_crash_handlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
}

int main() {
    install_crash_handlers();
    signal(SIGINT, on_sigint);

    AppRuntime app;
    g_app = &app;

    AppRuntime::Config cfg;
    cfg.cam_dev = "/dev/video9";
    cfg.cam_w = 640;
    cfg.cam_h = 480;
    cfg.ui_out_w = 480;
    cfg.ui_out_h = 480;
    cfg.pool_blocks = 4;
    cfg.pool_align = 64;

    if (!app.init(cfg)) return -1;
    return app.run();
}
