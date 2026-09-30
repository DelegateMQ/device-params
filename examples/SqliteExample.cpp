#include "SqliteExample.h"

#ifdef PARAM_BACKEND_SQLITE

#include "PumpParams.h"
#include "params/ParamStore.h"
#include "params/backends/SqliteBackend.h"
#include "sqlite3.h"
#include <cstdio>
#include <iostream>

namespace Example {

static const char* DB_PATH = "device-params-example.db";

void RunSqliteExample()
{
    std::cout << "--- SQLite Example ---\n";
    std::remove(DB_PATH);

    // First run: change settings; each change is saved as it is made
    {
        param::SqliteBackend backend(DB_PATH);
        if (!backend.IsOpen()) {
            std::cout << "Could not open " << DB_PATH << "\n";
            return;
        }
        param::ParamStore store(kPumpParams, &backend);
        store.SetSaveMode(param::SaveMode::Immediate);
        store.Init();

        store.Set(P::MaxRpm, 4200);
        store.Set(P::PidKp, 2.75f);
        store.Set(P::RunMode, Mode::Manual);
        store.Set(P::Telemetry, false);     // not PERSIST: never saved
    }

    // After a restart: the saved values come back
    {
        param::SqliteBackend backend(DB_PATH);
        param::ParamStore store(kPumpParams, &backend);
        store.Init();

        std::cout << "Reloaded MaxRpm:    " << store.Get(P::MaxRpm) << "\n";
        std::cout << "Reloaded PidKp:     " << store.Get(P::PidKp) << "\n";
        std::cout << "Reloaded RunMode:   " << static_cast<int>(store.Get(P::RunMode)) << "\n";
        std::cout << "Reloaded Telemetry: " << store.Get(P::Telemetry) << " (default)\n";
    }

    // The database is ordinary SQLite: any tool can inspect it
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
        std::cout << "SELECT id, type, length(value) FROM params:\n";
        sqlite3_exec(db, "SELECT id, type, length(value) FROM params ORDER BY id",
            [](void*, int cols, char** vals, char**) -> int {
                std::cout << "  ";
                for (int i = 0; i < cols; i++)
                    std::cout << (i ? " | " : "") << (vals[i] ? vals[i] : "NULL");
                std::cout << "\n";
                return 0;
            }, nullptr, nullptr);
    }
    sqlite3_close(db);

    std::remove(DB_PATH);
}

} // namespace Example

#else

namespace Example {
void RunSqliteExample() {}
}

#endif
