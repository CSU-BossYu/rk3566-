#include "ota_manager.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

// ========================
// 1) tiny sha256 (minimal)
// ========================
namespace {
using u8  = unsigned char;
using u32 = unsigned int;
using u64 = unsigned long long;

struct Sha256Ctx {
    u64 len = 0;
    u32 h[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    u8  buf[64]{};
    u32 buf_len = 0;
};

static inline u32 rotr(u32 x, u32 n){ return (x>>n) | (x<<(32-n)); }
static inline u32 ch(u32 x,u32 y,u32 z){ return (x & y) ^ (~x & z); }
static inline u32 maj(u32 x,u32 y,u32 z){ return (x & y) ^ (x & z) ^ (y & z); }
static inline u32 bsig0(u32 x){ return rotr(x,2)^rotr(x,13)^rotr(x,22); }
static inline u32 bsig1(u32 x){ return rotr(x,6)^rotr(x,11)^rotr(x,25); }
static inline u32 ssig0(u32 x){ return rotr(x,7)^rotr(x,18)^(x>>3); }
static inline u32 ssig1(u32 x){ return rotr(x,17)^rotr(x,19)^(x>>10); }

static const u32 K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void sha256_transform(Sha256Ctx& c, const u8 block[64]){
    u32 w[64];
    for(int i=0;i<16;i++){
        w[i] = (u32)block[i*4+0]<<24 | (u32)block[i*4+1]<<16 | (u32)block[i*4+2]<<8 | (u32)block[i*4+3];
    }
    for(int i=16;i<64;i++) w[i] = ssig1(w[i-2]) + w[i-7] + ssig0(w[i-15]) + w[i-16];

    u32 A=c.h[0], B=c.h[1], C=c.h[2], D=c.h[3], E=c.h[4], F=c.h[5], G=c.h[6], H=c.h[7];
    for(int i=0;i<64;i++){
        u32 T1 = H + bsig1(E) + ch(E,F,G) + K[i] + w[i];
        u32 T2 = bsig0(A) + maj(A,B,C);
        H = G;
        G = F;
        F = E;
        E = D + T1;
        D = C;
        C = B;
        B = A;
        A = T1 + T2;
    }
    c.h[0] += A; c.h[1] += B; c.h[2] += C; c.h[3] += D;
    c.h[4] += E; c.h[5] += F; c.h[6] += G; c.h[7] += H;
}

static void sha256_update(Sha256Ctx& c, const u8* data, size_t n){
    c.len += (u64)n;
    while(n){
        size_t take = 64 - c.buf_len;
        if(take > n) take = n;
        std::memcpy(c.buf + c.buf_len, data, take);
        c.buf_len += (u32)take;
        data += take;
        n -= take;
        if(c.buf_len == 64){
            sha256_transform(c, c.buf);
            c.buf_len = 0;
        }
    }
}

static void sha256_final(Sha256Ctx& c, u8 out[32]){
    const u64 bit_len = c.len * 8ULL;
    const u8 one = 0x80;
    sha256_update(c, &one, 1);
    const u8 zero = 0x00;
    while(c.buf_len != 56) sha256_update(c, &zero, 1);
    u8 lenbe[8];
    for(int i=0;i<8;i++) lenbe[7-i] = (u8)((bit_len>>(i*8)) & 0xff);
    sha256_update(c, lenbe, 8);
    for(int i=0;i<8;i++){
        out[i*4+0] = (u8)(c.h[i]>>24);
        out[i*4+1] = (u8)(c.h[i]>>16);
        out[i*4+2] = (u8)(c.h[i]>>8);
        out[i*4+3] = (u8)(c.h[i]>>0);
    }
}

static std::string hex32(const u8* p, int n){
    static const char* hexd="0123456789abcdef";
    std::string s; s.resize((size_t)n*2);
    for(int i=0;i<n;i++){
        s[(size_t)i*2+0]=hexd[(p[i]>>4)&0xF];
        s[(size_t)i*2+1]=hexd[(p[i]>>0)&0xF];
    }
    return s;
}

static std::string now_timestr()
{
    char buf[64];
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

static std::string trim_eol(std::string s)
{
    while(!s.empty() && (s.back()=='\n' || s.back()=='\r')) s.pop_back();
    return s;
}

// ------------------------
// 2) tiny HTTP client
//    仅支持 http://host[:port]/path 且 HTTP/1.0 非 chunked
// ------------------------
static bool parse_url_http(const std::string& url,
                           std::string* host, int* port, std::string* path,
                           std::string* err)
{
    const char* prefix = "http://";
    if (url.rfind(prefix, 0) != 0) {
        if (err) *err = "only http:// is supported";
        return false;
    }
    std::string rest = url.substr(std::strlen(prefix));
    size_t slash = rest.find('/');
    std::string hostport = (slash==std::string::npos) ? rest : rest.substr(0, slash);
    *path = (slash==std::string::npos) ? "/" : rest.substr(slash);

    size_t colon = hostport.find(':');
    if (colon == std::string::npos) {
        *host = hostport;
        *port = 80;
    } else {
        *host = hostport.substr(0, colon);
        *port = std::atoi(hostport.substr(colon+1).c_str());
        if (*port <= 0) *port = 80;
    }
    if (host->empty()) {
        if (err) *err = "empty host";
        return false;
    }
    return true;
}

static int connect_tcp(const std::string& host, int port, std::string* err)
{
    struct addrinfo hints; std::memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    char portbuf[16];
    std::snprintf(portbuf, sizeof(portbuf), "%d", port);

    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), portbuf, &hints, &res);
    if (rc != 0) {
        if (err) *err = std::string("getaddrinfo: ") + gai_strerror(rc);
        return -1;
    }

    int fd = -1;
    for (auto p=res; p; p=p->ai_next){
        fd = (int)socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && err) *err = "connect failed";
    return fd;
}

static bool http_get_raw(const std::string& url, std::vector<u8>* body, std::string* err)
{
    std::string host, path; int port = 80;
    if (!parse_url_http(url, &host, &port, &path, err)) return false;

    int fd = connect_tcp(host, port, err);
    if (fd < 0) return false;

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.0\r\n"
        << "Host: " << host << "\r\n"
        << "Connection: close\r\n\r\n";
    std::string s = req.str();

    if (write(fd, s.data(), s.size()) < 0) {
        if (err) *err = "write request failed";
        close(fd);
        return false;
    }

    std::string header;
    header.reserve(1024);
    std::vector<u8> buf(4096);

    bool header_done = false;
    while (true) {
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n == 0) break;
        if (n < 0) {
            if (err) *err = "read failed";
            close(fd);
            return false;
        }

        if (!header_done) {
            header.append((char*)buf.data(), (size_t)n);
            size_t pos = header.find("\r\n\r\n");
            if (pos != std::string::npos) {
                std::string head = header.substr(0, pos+4);
                std::string rest = header.substr(pos+4);
                header_done = true;

                if (head.find("200") == std::string::npos) {
                    if (err) *err = "HTTP status not 200";
                    close(fd);
                    return false;
                }
                body->insert(body->end(), rest.begin(), rest.end());
                header.clear();
            }
        } else {
            body->insert(body->end(), buf.begin(), buf.begin()+n);
        }
    }

    close(fd);
    return true;
}

// ------------------------
// 3) naive JSON getter
//    仅用于 manifest：\"key\": \"value\"
// ------------------------
static std::string json_get_str(const std::string& j, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return {};
    p = j.find(':', p);
    if (p == std::string::npos) return {};
    p++;
    while (p < j.size() && (j[p]==' '||j[p]=='\n'||j[p]=='\r'||j[p]=='\t')) p++;
    if (p>=j.size() || j[p] != '"') return {};
    p++;
    size_t e = j.find('"', p);
    if (e == std::string::npos) return {};
    return j.substr(p, e-p);
}

static bool ends_with(const std::string& s, const std::string& suf)
{
    return s.size() >= suf.size() && s.compare(s.size()-suf.size(), suf.size(), suf)==0;
}

static bool starts_with(const std::string& s, const char* pfx)
{
    return s.rfind(pfx, 0) == 0;
}

static std::string rstrip_slash(std::string s)
{
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

static std::string join_url(const std::string& base, const std::string& path)
{
    if (base.empty()) return path;
    if (path.empty()) return base;
    if (path.front() == '/') return base + path;
    return base + "/" + path;
}

static std::string normalize_pkg_url(const std::string& server_base_in, const std::string& pkg_url_in)
{
    const std::string server_base = rstrip_slash(server_base_in);
    std::string u = pkg_url_in;

    if (starts_with(u, "http:///")) {
        u = u.substr(std::strlen("http://")); // now starts with "/..."
        return join_url(server_base, u);
    }
    if (starts_with(u, "http://")) return u;

    if (u.empty()) return u;
    if (u.front() == '/') return join_url(server_base, u);
    return join_url(server_base, "/" + u);
}

// pending_attempts 解析：
// 格式： "<version> <count>\n"
struct PendingAttempts {
    std::string ver;
    int count = 0;
};

static PendingAttempts parse_attempts(const std::string& s)
{
    PendingAttempts a;
    std::istringstream iss(s);
    iss >> a.ver >> a.count;
    if (a.count < 0) a.count = 0;
    return a;
}

} // namespace

// ========================
// static members
// ========================
std::atomic<bool> OtaManager::s_cb_set{false};
OtaManager::StopCallback OtaManager::s_stop_cb{nullptr};

// ========================
// path helpers
// ========================
std::string OtaManager::BaseDir()          { return "/data/access"; }
std::string OtaManager::VersionsDir()      { return BaseDir() + "/versions"; }
std::string OtaManager::CurrentLink()      { return BaseDir() + "/current"; }
std::string OtaManager::LastGoodLink()     { return BaseDir() + "/last_good"; }
std::string OtaManager::OtaDir()           { return BaseDir() + "/ota"; }
std::string OtaManager::OtaLogsDir()       { return OtaDir() + "/logs"; }
std::string OtaManager::PendingFile()      { return OtaDir() + "/pending_version"; }
std::string OtaManager::LastErrorFile()    { return OtaDir() + "/last_error"; }
std::string OtaManager::ManifestCacheFile(){ return OtaDir() + "/last.json"; }
std::string OtaManager::CfgDir()           { return BaseDir() + "/cfg"; }

// Commit3: 持久化尝试次数（跨重启）
static std::string PendingAttemptsFile()
{
    return std::string("/data/access/ota/pending_attempts");
}

// ========================
// util
// ========================
void OtaManager::AppendLog(const std::string& line)
{
    (void)EnsureDir(OtaDir());
    (void)EnsureDir(OtaLogsDir());

    const std::string logp = OtaLogsDir() + "/ota.log";
    FILE* f = std::fopen(logp.c_str(), "ab");
    if (!f) return;
    std::string s = "[" + now_timestr() + "] " + line + "\n";
    (void)std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}

std::string OtaManager::ReadTextFile(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[4096];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        s.append(buf, buf + n);
    }
    std::fclose(f);
    return s;
}

bool OtaManager::WriteTextFileAtomic(const std::string& path, const std::string& s)
{
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    size_t n = std::fwrite(s.data(), 1, s.size(), f);
    std::fflush(f);
    int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    std::fclose(f);
    if (n != s.size()) return false;
    return RenameAtomic(tmp, path);
}

bool OtaManager::EnsureDir(const std::string& path, int mode)
{
    if (path.empty()) return false;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);

    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        cur.push_back(c);
        if (c == '/' && cur.size() > 1) (void)mkdir(cur.c_str(), mode);
    }
    if (mkdir(path.c_str(), mode) == 0) return true;
    return errno == EEXIST;
}

bool OtaManager::FileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool OtaManager::RenameAtomic(const std::string& from, const std::string& to)
{
    return ::rename(from.c_str(), to.c_str()) == 0;
}

bool OtaManager::RemoveAll(const std::string& path)
{
    std::string cmd = "rm -rf " + path;
    int rc = ::system(cmd.c_str());
    return rc == 0;
}

std::string OtaManager::ReadSymlinkTarget(const std::string& path)
{
    char buf[512];
    ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf)-1);
    if (n <= 0) return {};
    buf[n] = 0;
    return std::string(buf);
}

bool OtaManager::SymlinkAtomic(const std::string& target, const std::string& link_path)
{
    std::string tmp = link_path + ".new";
    (void)::unlink(tmp.c_str());
    if (::symlink(target.c_str(), tmp.c_str()) != 0) return false;
    if (!RenameAtomic(tmp, link_path)) {
        (void)::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool OtaManager::HttpGetToString(const std::string& url, std::string* out, std::string* err)
{
    std::vector<u8> body;
    if (!http_get_raw(url, &body, err)) return false;
    out->assign((char*)body.data(), (char*)body.data() + body.size());
    return true;
}

bool OtaManager::HttpGetToFile(const std::string& url, const std::string& out_path, std::string* err)
{
    std::vector<u8> body;
    if (!http_get_raw(url, &body, err)) return false;

    FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) {
        if (err) *err = std::string("open file failed: ") + std::strerror(errno);
        return false;
    }
    size_t n = std::fwrite(body.data(), 1, body.size(), f);
    std::fflush(f);
    int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    std::fclose(f);

    if (n != body.size()) {
        if (err) *err = "write file short";
        return false;
    }
    return true;
}

std::string OtaManager::Sha256HexOfFile(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    Sha256Ctx ctx;
    u8 buf[8192];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        sha256_update(ctx, buf, n);
    }
    std::fclose(f);
    u8 out[32];
    sha256_final(ctx, out);
    return hex32(out, 32);
}

// ========================
// package ops
// ========================
bool OtaManager::ExtractTarGzToDir(const std::string& tar_gz_path, const std::string& out_dir, std::string* err)
{
    std::string cmd = "tar -xzf " + tar_gz_path + " -C " + out_dir;
    int rc = ::system(cmd.c_str());
    if (rc != 0) {
        if (err) *err = "tar extract failed";
        return false;
    }
    return true;
}

bool OtaManager::ValidateStagingDir(const std::string& staging_dir, std::string* err)
{
    const std::string bin = staging_dir + "/bin/demo_rga_v4l2";
    struct stat st;
    if (stat(bin.c_str(), &st) != 0) {
        if (err) *err = "missing bin/demo_rga_v4l2";
        return false;
    }
    if ((st.st_mode & S_IXUSR) == 0) {
        (void)::chmod(bin.c_str(), 0755);
    }
    return true;
}

// ========================
// service control
// ========================
bool OtaManager::RestartService(std::string* err)
{
    const int rc = ::system("/etc/init.d/S90access restart");
    if (rc != 0) {
        if (err) *err = "service restart failed";
        return false;
    }
    return true;
}

std::string OtaManager::LoadServerFromConf()
{
    const std::string conf = ReadTextFile(CfgDir() + "/ota.conf");
    size_t p = conf.find("OTA_SERVER=");
    if (p == std::string::npos) return {};
    p += std::strlen("OTA_SERVER=");
    size_t e = conf.find_first_of("\r\n", p);
    if (e == std::string::npos) e = conf.size();
    std::string v = conf.substr(p, e-p);
    while (!v.empty() && (v.back()==' '||v.back()=='\t')) v.pop_back();
    while (!v.empty() && (v.front()==' '||v.front()=='\t')) v.erase(v.begin());
    return v;
}

// ========================
// public API
// ========================
void OtaManager::SetStopCallback(StopCallback cb)
{
    s_stop_cb = std::move(cb);
    s_cb_set.store(true);
}

// Commit3: 允许 pending 启动若干次，超阈值回滚
static constexpr int kMaxPendingAttempts = 3;

OtaManager::Result OtaManager::BootReconcile()
{
    Result r;
    (void)EnsureDir(BaseDir());
    (void)EnsureDir(OtaDir());
    (void)EnsureDir(VersionsDir());

    const std::string pending_path = PendingFile();
    const std::string attempts_path = PendingAttemptsFile();

    if (!FileExists(pending_path)) {
        // 无 pending，清理 attempts（防脏数据）
        (void)::unlink(attempts_path.c_str());
        r.ok = true;
        r.msg = "no pending";
        return r;
    }

    const std::string pending = trim_eol(ReadTextFile(pending_path));
    if (pending.empty()) {
        (void)::unlink(pending_path.c_str());
        (void)::unlink(attempts_path.c_str());
        r.ok = true;
        r.msg = "pending empty -> cleared";
        return r;
    }

    const std::string cur_target = ReadSymlinkTarget(CurrentLink());
    const std::string last_good_target = ReadSymlinkTarget(LastGoodLink());
    const std::string cur_ver = cur_target.empty() ? "" : cur_target.substr(cur_target.find_last_of('/') + 1);

    AppendLog("boot_reconcile: pending=" + pending + " cur_target=" + cur_target + " last_good=" + last_good_target);

    if (cur_ver != pending) {
        // 当前跑的不是 pending 版本：说明已经被回滚/或人为切换
        (void)::unlink(pending_path.c_str());
        (void)::unlink(attempts_path.c_str());
        r.ok = true;
        r.msg = "pending cleared (cur != pending)";
        return r;
    }

    // cur_ver == pending：说明我们正在尝试启动“未确认的新版本”
    PendingAttempts a = parse_attempts(ReadTextFile(attempts_path));
    if (a.ver != pending) {
        a.ver = pending;
        a.count = 0;
    }
    a.count += 1;

    {
        std::ostringstream oss;
        oss << a.ver << " " << a.count << "\n";
        (void)WriteTextFileAtomic(attempts_path, oss.str());
    }

    if (a.count < kMaxPendingAttempts) {
        r.ok = true;
        std::ostringstream oss;
        oss << "pending boot attempt " << a.count << "/" << kMaxPendingAttempts;
        r.msg = oss.str();
        AppendLog("boot_reconcile: " + r.msg + " (allow continue)");
        return r;
    }

    // 超阈值：回滚
    if (last_good_target.empty()) {
        r.ok = true;
        r.msg = "pending attempts exceeded but last_good missing (allow continue)";
        AppendLog("boot_reconcile: " + r.msg);
        return r;
    }

    (void)SymlinkAtomic(last_good_target, CurrentLink());
    (void)::unlink(pending_path.c_str());
    (void)::unlink(attempts_path.c_str());

    std::string err;
    (void)RestartService(&err);
    AppendLog("boot_reconcile: rollback -> restart, err=" + err);

    r.ok = false;
    r.msg = "rolled back to last_good and restarting";
    return r;
}

OtaManager::Result OtaManager::MarkBootSuccessful()
{
    Result r;
    (void)EnsureDir(BaseDir());
    (void)EnsureDir(OtaDir());

    const std::string cur_target = ReadSymlinkTarget(CurrentLink());
    if (cur_target.empty()) {
        r.msg = "current symlink missing";
        return r;
    }

    if (!SymlinkAtomic(cur_target, LastGoodLink())) {
        r.msg = "update last_good failed";
        return r;
    }

    (void)::unlink(PendingFile().c_str());
    (void)::unlink(PendingAttemptsFile().c_str());
    AppendLog("mark_boot_successful: last_good=" + cur_target);

    r.ok = true;
    r.msg = "boot confirmed";
    return r;
}

bool OtaManager::SpawnOtaProcess(const std::string& server_url)
{
    if (s_cb_set.load() && s_stop_cb) {
        try { s_stop_cb(); } catch (...) {}
    }

    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();
        const char* self = "/proc/self/exe";

        std::string srv = server_url;
        if (srv.empty()) srv = LoadServerFromConf();
        if (srv.empty()) srv = "http://127.0.0.1:5000";

        ::execl(self, "demo_rga_v4l2", "--ota", srv.c_str(), (char*)nullptr);
        _exit(127);
    }
    return true;
}

OtaManager::Result OtaManager::RunOta(const std::string& server_url)
{
    Result r;
    (void)EnsureDir(BaseDir());
    (void)EnsureDir(OtaDir());
    (void)EnsureDir(VersionsDir());
    (void)EnsureDir(CfgDir());
    (void)EnsureDir(OtaLogsDir());

    std::string srv = server_url;
    if (srv.empty()) srv = LoadServerFromConf();
    if (srv.empty()) srv = "http://127.0.0.1:5000";
    srv = rstrip_slash(srv);

    // 1) 拉 manifest
    const std::string manifest_url = srv + "/version.json";
    std::string json, err;
    if (!HttpGetToString(manifest_url, &json, &err)) {
        r.msg = "fetch version.json failed: " + err;
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        return r;
    }
    (void)WriteTextFileAtomic(ManifestCacheFile(), json);

    const std::string new_ver = json_get_str(json, "version");
    std::string pkg_url = json_get_str(json, "download_url");
    if (pkg_url.empty()) pkg_url = json_get_str(json, "package_url");
    std::string sha = json_get_str(json, "checksum");
    if (sha.empty()) sha = json_get_str(json, "sha256");

    if (new_ver.empty() || pkg_url.empty() || sha.empty()) {
        r.msg = "invalid version.json fields (need version, package_url/download_url, sha256/checksum)";
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        return r;
    }

    // 2) 已是当前版本
    const std::string cur_target = ReadSymlinkTarget(CurrentLink());
    const std::string cur_ver = cur_target.empty() ? "" : cur_target.substr(cur_target.find_last_of('/') + 1);
    if (cur_ver == new_ver) {
        r.ok = true;
        r.msg = "already latest: " + new_ver;
        AppendLog(r.msg);
        return r;
    }

    // 3) 规范化 package_url
    const std::string pkg_url_norm = normalize_pkg_url(srv, pkg_url);
    AppendLog("manifest: version=" + new_ver + " pkg_url(raw)=" + pkg_url + " -> " + pkg_url_norm);

    // 4) 下载包到 ota/（.tmp 校验后 rename）
    std::string ext = ".tar.gz";
    if (ends_with(pkg_url_norm, ".tgz")) ext = ".tgz";
    else if (ends_with(pkg_url_norm, ".tar.gz")) ext = ".tar.gz";

    const std::string pkg_path = OtaDir() + "/pkg_" + new_ver + ext;
    const std::string pkg_tmp  = pkg_path + ".tmp";
    (void)::unlink(pkg_tmp.c_str());
    (void)::unlink(pkg_path.c_str());

    AppendLog("download: " + pkg_url_norm + " -> " + pkg_tmp);
    if (!HttpGetToFile(pkg_url_norm, pkg_tmp, &err)) {
        r.msg = "download failed: " + err;
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        return r;
    }

    // 5) sha256 校验（对 .tmp 校验）
    const std::string got = Sha256HexOfFile(pkg_tmp);
    if (got.empty() || got != sha) {
        r.msg = "sha256 mismatch: got=" + got + " expect=" + sha;
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        return r;
    }

    // 6) 包类型判断：必须对 pkg_path 判断，不能对 pkg_tmp
    if (!(ends_with(pkg_path, ".tar.gz") || ends_with(pkg_path, ".tgz"))) {
        r.msg = "package must be .tar.gz/.tgz";
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        return r;
    }

    // 7) tmp -> pkg_path 原子落盘
    if (!RenameAtomic(pkg_tmp, pkg_path)) {
        r.msg = std::string("rename pkg_tmp->pkg_path failed: ") + std::strerror(errno);
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        return r;
    }

    // 8) 解包 staging
    const std::string staging = VersionsDir() + "/." + new_ver + ".staging";
    const std::string final_dir = VersionsDir() + "/" + new_ver;

    (void)RemoveAll(staging);
    (void)EnsureDir(staging);

    if (!ExtractTarGzToDir(pkg_path, staging, &err)) {
        r.msg = "extract failed: " + err;
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        (void)RemoveAll(staging);
        return r;
    }

    if (!ValidateStagingDir(staging, &err)) {
        r.msg = "validate failed: " + err;
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        (void)RemoveAll(staging);
        return r;
    }

    // 9) staging -> versions/<ver>
    (void)RemoveAll(final_dir);
    if (!RenameAtomic(staging, final_dir)) {
        r.msg = std::string("rename staging->final failed: ") + std::strerror(errno);
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        (void)RemoveAll(staging);
        return r;
    }

    // 10) current 原子切换
    if (!SymlinkAtomic(final_dir, CurrentLink())) {
        r.msg = "atomic switch current symlink failed";
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        return r;
    }

    // 11) 写 pending，并清 attempts（新版本第一次启动 attempts=0）
    (void)WriteTextFileAtomic(PendingFile(), new_ver + "\n");
    (void)::unlink(PendingAttemptsFile().c_str());

    // 12) 重启服务
    if (!RestartService(&err)) {
        r.msg = "restart failed: " + err;
        (void)WriteTextFileAtomic(LastErrorFile(), r.msg + "\n");
        AppendLog(r.msg);
        return r;
    }

    r.ok = true;
    r.msg = "update ok -> " + new_ver;
    AppendLog(r.msg);
    return r;
}
