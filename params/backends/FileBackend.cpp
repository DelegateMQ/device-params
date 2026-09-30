#include "FileBackend.h"
#include "../Record.h"
#include <cstdio>
#include <fstream>
#include <vector>

namespace param {

namespace {

bool LoadAll(const std::string& path, std::vector<Record>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return true;    // no file yet: nothing stored

    uint8_t buf[PACKED_RECORD_SIZE];
    while (in.read(reinterpret_cast<char*>(buf), sizeof(buf)))
        out.push_back(UnpackRecord(buf));
    return in.eof();
}

} // namespace

FileBackend::FileBackend(const std::string& path) : m_path(path)
{
}

bool FileBackend::ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink)
{
    std::vector<Record> recs;
    const bool ok = LoadAll(m_path, recs);
    for (const Record& rec : recs)
        sink(rec);
    return ok;
}

bool FileBackend::Write(const Record* recs, size_t count)
{
    std::vector<Record> all;
    if (!LoadAll(m_path, all))
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

    // Write a temp file, then replace, so a crash never leaves a partial file
    const std::string tmp = m_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        uint8_t buf[PACKED_RECORD_SIZE];
        for (const Record& r : all) {
            PackRecord(r, buf);
            out.write(reinterpret_cast<const char*>(buf), sizeof(buf));
        }
        if (!out.flush())
            return false;
    }
    std::remove(m_path.c_str());
    return std::rename(tmp.c_str(), m_path.c_str()) == 0;
}

bool FileBackend::Erase()
{
    std::remove(m_path.c_str());
    return true;
}

} // namespace param
