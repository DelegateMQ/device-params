#ifndef _PARAM_FILE_BACKEND_H
#define _PARAM_FILE_BACKEND_H

// FileBackend.h
// Desktop / embedded Linux backend storing records in a single binary file
// (packed records, see Record.h).
//
// Crash safety: a save writes `<path>.tmp`, syncs it to disk, then atomically
// replaces `<path>` with it (rename on POSIX, MoveFileEx on Windows) and syncs
// the directory. A crash at any point leaves either the old file or the new
// one, never a mix and never neither:
// - Crash before the replace: `<path>` is the old file; a stale `.tmp` is
//   ignored and removed on the next save.
// - Crash during the very first save (no `<path>` yet): the `.tmp` is loaded,
//   since it is the only copy. A record torn by the crash fails its CRC and
//   keeps its default.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "IBackend.h"
#include <string>

namespace param {

class FileBackend : public IBackend {
public:
    explicit FileBackend(const std::string& path);

    bool ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink) override;
    bool Write(const Record* recs, size_t count) override;

    /// Delete the stored file (and any leftover temp file).
    bool Erase() override;

    const std::string& Path() const { return m_path; }
    std::string TempPath() const { return m_path + ".tmp"; }

private:
    std::string m_path;
};

} // namespace param

#endif
