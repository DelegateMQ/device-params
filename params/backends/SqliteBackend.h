#ifndef _PARAM_SQLITE_BACKEND_H
#define _PARAM_SQLITE_BACKEND_H

// SqliteBackend.h
// Desktop / embedded Linux backend. One row per parameter, keyed on ParamId;
// each Write() is a single transaction. Built only when PARAM_BACKEND_SQLITE=ON.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "IBackend.h"
#include <string>

struct sqlite3;

namespace param {

class SqliteBackend : public IBackend {
public:
    explicit SqliteBackend(const std::string& dbPath);
    ~SqliteBackend() override;

    SqliteBackend(const SqliteBackend&) = delete;
    SqliteBackend& operator=(const SqliteBackend&) = delete;

    bool ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink) override;
    bool Write(const Record* recs, size_t count) override;
    bool Erase() override;

private:
    sqlite3* m_db = nullptr;
};

} // namespace param

#endif
