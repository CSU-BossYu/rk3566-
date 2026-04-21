#pragma once

#include <atomic>
#include <functional>
#include <string>

class OtaManager {
public:
    struct Result {
        bool ok = false;
        std::string msg;
    };

    // =============================
    // Commit 1: 部署骨架 + 启动自愈接口
    // =============================

    // 开机（或服务启动）自检：如果存在 pending_version，说明上次切换到新版本后尚未“确认成功”。
    // - 若当前版本 == pending_version：视为新版本首次启动，立刻回滚到 last_good 并重启服务（自愈）。
    //   这样“未确认的新版本”只要启动失败/崩溃导致没来得及确认，下次启动一定回滚。
    // - 若当前版本 != pending_version：清理 pending（通常是手工回滚/或其他情况）。
    // 返回：
    //   ok=true: 继续启动当前进程。
    //   ok=false: 已触发回滚/重启，调用方应尽快退出。
    static Result BootReconcile();

    // 在新版本稳定运行后调用（例如 UI 初始化完成 + 推理线程正常后 N 秒），
    // 将 last_good 指向 current，并清理 pending_version。
    static Result MarkBootSuccessful();

    // UI 触发：fork + exec self --ota（后台执行 OTA，最后会触发服务 restart）
    // server_url 为空时：从 /data/access/cfg/ota.conf 读取 OTA_SERVER=...。
    static bool SpawnOtaProcess(const std::string& server_url);

    // =============================
    // Commit 2: 真正 OTA 执行入口
    // =============================

    // main --ota 模式入口：拉取 manifest + 包 + 校验 + 解包 + 原子切换 + 标记 pending + 重启。
    static Result RunOta(const std::string& server_url);

    // 可选：让主程序在触发 OTA 前优雅停掉采集/推理（避免升级过程资源竞争）。
    // 不要求 UI 线程参与；由上层在 SpawnOtaProcess 前调用。
    using StopCallback = std::function<void()>;
    static void SetStopCallback(StopCallback cb);

private:
    // -------- path helpers --------
    static std::string BaseDir();
    static std::string VersionsDir();
    static std::string CurrentLink();
    static std::string LastGoodLink();
    static std::string OtaDir();
    static std::string OtaLogsDir();
    static std::string PendingFile();
    static std::string LastErrorFile();
    static std::string ManifestCacheFile();
    static std::string CfgDir();

    // -------- util --------
    static void AppendLog(const std::string& line);
    static std::string ReadTextFile(const std::string& path);
    static bool WriteTextFileAtomic(const std::string& path, const std::string& s);

    static bool EnsureDir(const std::string& path, int mode = 0755);
    static bool FileExists(const std::string& path);
    static bool RenameAtomic(const std::string& from, const std::string& to);
    static bool RemoveAll(const std::string& path);

    static std::string ReadSymlinkTarget(const std::string& path);
    static bool SymlinkAtomic(const std::string& target, const std::string& link_path);

    static bool HttpGetToString(const std::string& url, std::string* out, std::string* err);
    static bool HttpGetToFile(const std::string& url, const std::string& out_path, std::string* err);

    static std::string Sha256HexOfFile(const std::string& path);

    // package ops
    static bool ExtractTarGzToDir(const std::string& tar_gz_path, const std::string& out_dir, std::string* err);
    static bool ValidateStagingDir(const std::string& staging_dir, std::string* err);

    // service control
    static bool RestartService(std::string* err);

    // config
    static std::string LoadServerFromConf(); // /data/access/cfg/ota.conf

private:
    static std::atomic<bool> s_cb_set;
    static StopCallback s_stop_cb;
};
