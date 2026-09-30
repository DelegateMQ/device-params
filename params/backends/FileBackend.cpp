#include "FileBackend.h"
#include "../Record.h"
#include <cerrno>
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace param {

namespace {

bool Exists(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

/// Read every whole record in `path`. A missing file is empty, not an error;
/// a partial record at the end (torn write) is ignored. Returns false only on
/// a read error.
bool LoadFile(const std::string& path, std::vector<Record>& out)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return true;

    uint8_t buf[PACKED_RECORD_SIZE];
    while (std::fread(buf, 1, sizeof(buf), f) == sizeof(buf))
        out.push_back(UnpackRecord(buf));
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

/// Records to load: the main file, or, if there is none, the temp file left
/// by a crash during the first save.
bool LoadCurrent(const std::string& path, const std::string& tmp, std::vector<Record>& out)
{
    if (Exists(path))
        return LoadFile(path, out);
    return LoadFile(tmp, out);
}

/// Flush stdio and the OS cache for `f` to the storage device.
bool SyncFile(std::FILE* f)
{
    if (std::fflush(f) != 0)
        return false;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

/// Atomically replace `to` with `from`.
bool ReplaceAtomically(const std::string& from, const std::string& to)
{
#if defined(_WIN32)
    return MoveFileExA(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

/// Make the rename itself durable (POSIX: sync the containing directory).
void SyncDirectoryOf([[maybe_unused]] const std::string& path)
{
#if !defined(_WIN32)
    const size_t slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    const int fd = open(dir.c_str(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);      // best effort: some filesystems don't support it
        close(fd);
    }
#endif
}

bool RemoveIfExists(const std::string& path)
{
    return std::remove(path.c_str()) == 0 || !Exists(path);
}

} // namespace

FileBackend::FileBackend(const std::string& path) : m_path(path)
{
}

bool FileBackend::ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink)
{
    std::vector<Record> recs;
    const bool ok = LoadCurrent(m_path, TempPath(), recs);
    for (const Record& rec : recs)
        sink(rec);
    return ok;
}

bool FileBackend::Write(const Record* recs, size_t count)
{
    // Merge the new records into what is stored. On a read error, don't
    // write: saving only the new records would drop the others.
    std::vector<Record> all;
    if (!LoadCurrent(m_path, TempPath(), all))
        return false;

    for (size_t k = 0; k < count; k++) {
        bool found = false;
        for (Record& r : all) {
            if (r.id == recs[k].id) {
                r = recs[k];
                found = true;
                break;
            }
        }
        if (!found)
            all.push_back(recs[k]);
    }

    // Write and sync the temp file ("wb" truncates a stale one)
    const std::string tmp = TempPath();
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = true;
    uint8_t buf[PACKED_RECORD_SIZE];
    for (const Record& r : all) {
        PackRecord(r, buf);
        if (std::fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) {
            ok = false;
            break;
        }
    }
    ok = ok && SyncFile(f);
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }

    // Swap it in atomically, then make the swap durable
    if (!ReplaceAtomically(tmp, m_path))
        return false;
    SyncDirectoryOf(m_path);
    return true;
}

bool FileBackend::Erase()
{
    const bool tmpGone = RemoveIfExists(TempPath());
    const bool mainGone = RemoveIfExists(m_path);
    return tmpGone && mainGone;
}

} // namespace param
