#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

class FaceDB {
public:
    struct Record {
        std::string name;
        std::vector<float> feat; // 512, L2 normalized
    };

    FaceDB();
    ~FaceDB();

    FaceDB(const FaceDB&) = delete;
    FaceDB& operator=(const FaceDB&) = delete;

    bool open(const std::string& db_path);
    void close();
    bool ok() const { return db_ != nullptr; }

    // CRUD
    bool upsert_feat(const std::string& name, const float* feat512, int dims = 512);
    bool remove_by_name(const std::string& name);

    // Query
    bool list_names(std::vector<std::string>& out_names) const;
    bool load_all(std::vector<Record>& out) const;

    const std::string& path() const { return path_; }

private:
    bool init_schema_();
    bool exec_(const char* sql) const;

private:
    sqlite3* db_ = nullptr;
    std::string path_;
};
