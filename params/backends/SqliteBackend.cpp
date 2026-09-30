#include "SqliteBackend.h"
#include "../Record.h"
#include "sqlite3.h"
#include <cstring>
#include <ctime>

namespace param {

SqliteBackend::SqliteBackend(const std::string& dbPath)
{
    if (sqlite3_open(dbPath.c_str(), &m_db) != SQLITE_OK) {
        sqlite3_close(m_db);
        m_db = nullptr;
        return;
    }
    if (!Exec("CREATE TABLE IF NOT EXISTS params ("
              "id INTEGER PRIMARY KEY, "
              "type INTEGER NOT NULL, "
              "value BLOB NOT NULL, "
              "updated_at INTEGER NOT NULL)")) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

SqliteBackend::~SqliteBackend()
{
    if (m_db)
        sqlite3_close(m_db);
}

bool SqliteBackend::Exec(const char* sql)
{
    return m_db && sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool SqliteBackend::ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink)
{
    if (!m_db)
        return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT id, type, value FROM params", -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Record rec{};
        rec.id = static_cast<ParamId>(sqlite3_column_int(stmt, 0));
        rec.type = static_cast<TypeTag>(sqlite3_column_int(stmt, 1));
        const int len = sqlite3_column_bytes(stmt, 2);
        if (len < 0 || len > static_cast<int>(sizeof(rec.data)))
            continue;
        rec.len = static_cast<uint8_t>(len);
        if (len > 0)
            std::memcpy(rec.data, sqlite3_column_blob(stmt, 2), static_cast<size_t>(len));
        rec.crc = RecordCrc(rec);
        sink(rec);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool SqliteBackend::Write(const Record* recs, size_t count)
{
    if (!m_db)
        return false;
    if (!Exec("BEGIN"))
        return false;

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(m_db,
        "INSERT INTO params (id, type, value, updated_at) VALUES (?1, ?2, ?3, ?4) "
        "ON CONFLICT(id) DO UPDATE SET type = ?2, value = ?3, updated_at = ?4",
        -1, &stmt, nullptr) == SQLITE_OK;

    const sqlite3_int64 now = static_cast<sqlite3_int64>(std::time(nullptr));
    for (size_t k = 0; ok && k < count; k++) {
        sqlite3_bind_int(stmt, 1, recs[k].id);
        sqlite3_bind_int(stmt, 2, static_cast<int>(recs[k].type));
        sqlite3_bind_blob(stmt, 3, recs[k].data, recs[k].len, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, now);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);

    if (!ok) {
        Exec("ROLLBACK");
        return false;
    }
    return Exec("COMMIT");
}

bool SqliteBackend::Erase()
{
    return Exec("DELETE FROM params");
}

} // namespace param
