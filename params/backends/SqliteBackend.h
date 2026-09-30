#ifndef _PARAM_SQLITE_BACKEND_H
#define _PARAM_SQLITE_BACKEND_H

// SqliteBackend.h
// Desktop / embedded Linux backend. One row per parameter, keyed on ParamId;
// each Write() is a single transaction, so a Commit() is atomic. Built only
// when PARAM_BACKEND_SQLITE=ON.
//
//   CREATE TABLE params (
//       id         INTEGER PRIMARY KEY,   -- ParamId
//       type       INTEGER NOT NULL,      -- TypeTag
//       value      BLOB NOT NULL,         -- Record data bytes
//       updated_at INTEGER NOT NULL       -- Unix time (seconds)
//   );
//
// SQLite checks its own integrity, so no CRC is stored; one is recomputed on
// load for the store's record check.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "IBackend.h"
#include <string>

struct sqlite3;

namespace param {

class SqliteBackend : public IBackend {
public:
    /// Opens (creating if needed) the database file.
    explicit SqliteBackend(const std::string& dbPath);
    ~SqliteBackend() override;

    SqliteBackend(const SqliteBackend&) = delete;
    SqliteBackend& operator=(const SqliteBackend&) = delete;

    bool IsOpen() const { return m_db != nullptr; }

    bool ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink) override;
    bool Write(const Record* recs, size_t count) override;
    bool Erase() override;

private:
    bool Exec(const char* sql);

    sqlite3* m_db = nullptr;
};

} // namespace param

#endif
