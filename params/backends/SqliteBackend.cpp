#include "SqliteBackend.h"
#include "sqlite3.h"

namespace param {

// CREATE TABLE IF NOT EXISTS params (
//     id         INTEGER PRIMARY KEY,
//     name       TEXT,
//     type       INTEGER NOT NULL,
//     value      BLOB NOT NULL,
//     updated_at INTEGER NOT NULL
// );

SqliteBackend::SqliteBackend(const std::string& dbPath)
{
    (void)dbPath;
    // TODO: sqlite3_open(), create table
}

SqliteBackend::~SqliteBackend()
{
    if (m_db)
        sqlite3_close(m_db);
}

bool SqliteBackend::ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink)
{
    (void)sink;
    // TODO: SELECT id, type, value FROM params
    return true;
}

bool SqliteBackend::Write(const Record* recs, size_t count)
{
    (void)recs; (void)count;
    // TODO: BEGIN; INSERT ... ON CONFLICT(id) DO UPDATE per record; COMMIT
    return true;
}

bool SqliteBackend::Erase()
{
    // TODO: DELETE FROM params
    return true;
}

} // namespace param
