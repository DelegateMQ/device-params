#ifndef _PARAM_FILE_BACKEND_H
#define _PARAM_FILE_BACKEND_H

// FileBackend.h
// Desktop backend storing records in a single binary file.
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
    bool Erase() override;

private:
    std::string m_path;
};

} // namespace param

#endif
