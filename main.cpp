#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app/app_runtime.h"

static AppRuntime* g_app = nullptr;

static inline pid_t get_tid() {
    return (pid_t)syscall(SYS_gettid);
}

static void on_stop_signal(int) {
    if (g_app) g_app->requestStop();
}

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

static bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

static bool dir_exists(const std::string& p) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static std::string dirname_of(const std::string& p) {
    auto pos = p.find_last_of('/');
    if (pos == std::string::npos) return {};
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

static void prepend_ld_library_path(const std::string& libdir) {
    if (libdir.empty()) return;
    const char* oldp = ::getenv("LD_LIBRARY_PATH");
    std::string old = oldp ? oldp : "";
    if (!old.empty() && old.find(libdir) != std::string::npos) return;

    std::string neu = libdir;
    if (!old.empty()) neu += ":" + old;
    ::setenv("LD_LIBRARY_PATH", neu.c_str(), 1);
}

static void ensure_runtime_libpath() {
    char exe[512];
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = 0;
    std::string exep = exe;

    std::string exedir = dirname_of(exep);
    std::string base = dirname_of(exedir);

    // A: .../bin/demo -> .../lib
    std::string libA = base + "/lib";
    if (file_exists(libA)) prepend_ld_library_path(libA);

    // B: .../deploy/demo -> .../deploy/lib
    std::string libB = exedir + "/lib";
    if (file_exists(libB)) prepend_ld_library_path(libB);
}

// 为了让 "models/..." "faces.db" 等相对路径稳定可用：
// - 优先 chdir 到包含 models/ 的目录（通常是 exe 的上一级，例如 .../bin/demo -> .../models）
// - 否则 chdir 到 exe 所在目录（例如 adb push 到 /data/demo/ 直接运行 ./demo）
static void ensure_runtime_workdir() {
    const char* no = ::getenv("NO_CHDIR");
    if (no && (std::strcmp(no, "1") == 0 || std::strcmp(no, "true") == 0)) return;

    char exe[512];
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = 0;
    std::string exep = exe;

    std::string exedir = dirname_of(exep);
    std::string base = dirname_of(exedir);

    // 先试 base（常见布局：base/bin/demo + base/models）
    if (!base.empty() && dir_exists(base + "/models")) {
        (void)::chdir(base.c_str());
        return;
    }

    // 再试 exedir（常见布局：exedir/demo + exedir/models）
    if (!exedir.empty() && dir_exists(exedir + "/models")) {
        (void)::chdir(exedir.c_str());
        return;
    }

    // 最后兜底：尽量让相对路径至少以可执行文件目录为基准
    if (!exedir.empty()) {
        (void)::chdir(exedir.c_str());
    }
}

int main(int argc, char** argv) {
    install_crash_handlers();
    ensure_runtime_libpath();

    // 简化部署模式：adb push 资源 + 直接运行。
    // 这里自动把工作目录切到合适位置，避免必须手工 cd。
    ensure_runtime_workdir();

    signal(SIGINT,  on_stop_signal);
    signal(SIGTERM, on_stop_signal);

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