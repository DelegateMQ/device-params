#ifndef _SQLITE_EXAMPLE_H
#define _SQLITE_EXAMPLE_H

// SqliteExample.h
// Persist parameters to a SQLite database, reload them after a restart, and
// read the stored table with plain SQL. No-op unless PARAM_BACKEND_SQLITE.

namespace Example {
    void RunSqliteExample();
}

#endif
