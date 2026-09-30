#ifndef _PARAM_FLASH_LOG_BACKEND_H
#define _PARAM_FLASH_LOG_BACKEND_H

// FlashLogBackend.h
// Bare metal / RTOS backend. Appends records across two flash sectors and
// compacts into the other sector when the active one fills. The newest record
// for each ID wins on load.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "IBackend.h"

namespace param {

/// Minimal flash driver the application supplies for its part.
class IFlash {
public:
    virtual ~IFlash() = default;
    virtual size_t SectorSize() const = 0;
    virtual bool   Read(size_t sector, size_t offset, void* buf, size_t len) = 0;
    virtual bool   Program(size_t sector, size_t offset, const void* buf, size_t len) = 0;
    virtual bool   EraseSector(size_t sector) = 0;
};

class FlashLogBackend : public IBackend {
public:
    /// @param flash  Driver for two dedicated sectors (0 and 1).
    explicit FlashLogBackend(IFlash& flash);

    bool ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink) override;
    bool Write(const Record* recs, size_t count) override;
    bool Erase() override;

private:
    IFlash& m_flash;
    size_t  m_activeSector = 0;
    size_t  m_writeOffset = 0;
};

} // namespace param

#endif
