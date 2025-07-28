#include "utils/face_db.h"

#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" {
#include "sqlite3.h"
}

FaceDB::FaceDB() = default;
FaceDB::~FaceDB() { close(); }

bool FaceDB::exec_(const char* sql) const {
    if (!db_ || !sql) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            std::fprintf(stderr, "[FaceDB] sqlite3_exec error: %s | SQL: %s\n", err, sql);
            sqlite3_free(err);
        } else {
            std::fprintf(stderr, "[FaceDB] sqlite3_exec error rc=%d | SQL: %s\n", rc, sql);
        }
        return false;
    }
    return true;
}

bool FaceDB::init_schema_() {
    if (!exec_("PRAGMA journal_mode=WAL;")) return false;
    if (!exec_("PRAGMA synchronous=NORMAL;")) return false;
    if (!exec_("PRAGMA temp_store=MEMORY;")) return false;

    const char* ddl =
        "CREATE TABLE IF NOT EXISTS faces ("
        "  name TEXT PRIMARY KEY,"
        "  feat BLOB NOT NULL,"
        "  dims INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ");";
    return exec_(ddl);
}

bool FaceDB::open(const std::string& db_path) {
    close();
    path_ = db_path;

    int rc = sqlite3_open_v2(path_.c_str(), &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[FaceDB] sqlite3_open_v2 failed rc=%d (%s)\n", rc,
                     db_ ? sqlite3_errmsg(db_) : "null");
        close();
        return false;
    }

    sqlite3_busy_timeout(db_, 1000);

    if (!init_schema_()) {
        close();
        return false;
    }
    return true;
}

void FaceDB::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    path_.clear();
}

bool FaceDB::upsert_feat(const std::string& name, const float* feat512, int dims) {
    if (!db_ || name.empty() || !feat512) return false;
    if (dims <= 0) return false;

    const char* sql_upd = "UPDATE faces SET feat=?, dims=?, updated_at=? WHERE name=?;";
    sqlite3_stmt* st = nullptr;

    int rc = sqlite3_prepare_v2(db_, sql_upd, -1, &st, nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[FaceDB] prepare UPDATE failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }

    const int64_t now = (int64_t)time(nullptr);
    const int feat_bytes = dims * (int)sizeof(float);

    sqlite3_bind_blob(st, 1, feat512, feat_bytes, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 2, dims);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_text(st, 4, name.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        std::fprintf(stderr, "[FaceDB] UPDATE step failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }

    if (sqlite3_changes(db_) > 0) return true;

    const char* sql_ins = "INSERT INTO faces(name, feat, dims, created_at, updated_at) VALUES(?,?,?,?,?);";
    rc = sqlite3_prepare_v2(db_, sql_ins, -1, &st, nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[FaceDB] prepare INSERT failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 2, feat512, feat_bytes, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 3, dims);
    sqlite3_bind_int64(st, 4, now);
    sqlite3_bind_int64(st, 5, now);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        std::fprintf(stderr, "[FaceDB] INSERT step failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool FaceDB::remove_by_name(const std::string& name) {
    if (!db_ || name.empty()) return false;

    const char* sql = "DELETE FROM faces WHERE name=?;";
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &st, nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[FaceDB] prepare DELETE failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        std::fprintf(stderr, "[FaceDB] DELETE step failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }

    // ✅必须真正删到行，才算成功（否则 UI 会误以为删了）
    return sqlite3_changes(db_) > 0;
}

bool FaceDB::list_names(std::vector<std::string>& out_names) const {
    out_names.clear();
    if (!db_) return false;

    const char* sql = "SELECT name FROM faces ORDER BY name COLLATE NOCASE;";
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &st, nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[FaceDB] prepare list_names failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char* s = sqlite3_column_text(st, 0);
        if (s) out_names.emplace_back((const char*)s);
    }
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        std::fprintf(stderr, "[FaceDB] list_names step failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool FaceDB::load_all(std::vector<Record>& out) const {
    out.clear();
    if (!db_) return false;

    const char* sql = "SELECT name, feat, dims FROM faces ORDER BY name COLLATE NOCASE;";
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &st, nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[FaceDB] prepare load_all failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(st, 0);
        const void* blob = sqlite3_column_blob(st, 1);
        int bytes = sqlite3_column_bytes(st, 1);
        int dims = sqlite3_column_int(st, 2);

        if (!name || !blob || dims <= 0) continue;
        if (bytes != dims * (int)sizeof(float)) continue;

        Record r;
        r.name = (const char*)name;
        r.feat.resize((size_t)dims);
        std::memcpy(r.feat.data(), blob, (size_t)bytes);
        out.emplace_back(std::move(r));
    }

    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        std::fprintf(stderr, "[FaceDB] load_all step failed: %s\n", sqlite3_errmsg(db_));
        return false;
    }
    return true;
}
