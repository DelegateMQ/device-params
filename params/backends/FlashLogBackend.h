#ifndef _PARAM_FLASH_LOG_BACKEND_H
#define _PARAM_FLASH_LOG_BACKEND_H

// FlashLogBackend.h
// Bare metal / RTOS backend for NOR flash. Records are appended to the active
// sector of two dedicated sectors; when it fills, the newest record for each
// ID (plus the new batch) is compacted into the other sector.
//
// Sector layout (slot = PACKED_RECORD_SIZE rounded up to IFlash::WriteSize()):
//   slot 0      header: magic "DPRM" + generation (little-endian u32 each)
//   slot 1..n   packed records (see Record.h); an all-0xFF slot ends the log
//
// Power-loss safety:
// - Append: a torn record fails its CRC on load and is skipped, so the
//   previous value for that ID is kept.
// - Compaction: records are written first and the header last. A sector with
//   no valid header is ignored, so an interrupted compaction leaves the old
//   sector active.
//
// ParamId 0xFFFF is reserved (it reads as erased flash).
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "IBackend.h"
#include <cstddef>
#include <cstdint>

namespace param {

/// Minimal NOR flash driver the application supplies for its part.
/// Erased flash reads 0xFF; Program() may only clear bits.
class IFlash {
public:
    virtual ~IFlash() = default;
    virtual size_t SectorSize() const = 0;
    /// Program granularity in bytes (1, 2, 4, 8, 16 or 32). Program() calls
    /// are always aligned to and a multiple of this size.
    virtual size_t WriteSize() const { return 1; }
    virtual bool   Read(size_t sector, size_t offset, void* buf, size_t len) = 0;
    virtual bool   Program(size_t sector, size_t offset, const void* buf, size_t len) = 0;
    virtual bool   EraseSector(size_t sector) = 0;
};

class FlashLogBackend : public IBackend {
public:
    /// @param flash  Driver for two dedicated sectors, numbered 0 and 1.
    explicit FlashLogBackend(IFlash& flash);

    bool ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink) override;
    bool Write(const Record* recs, size_t count) override;
    bool Erase() override;

    /// Number of compactions since construction (diagnostics / tests).
    size_t CompactionCount() const { return m_compactions; }

private:
    static constexpr uint32_t MAGIC = 0x4D525044;   // "DPRM"
    static constexpr size_t   MAX_SLOT = 32;
    static constexpr int      NO_SECTOR = -1;

    bool Mount();
    bool ReadHeader(size_t sector, uint32_t& generation);
    bool ReadSlot(size_t sector, size_t offset, uint8_t* buf);
    bool IsErased(const uint8_t* buf) const;
    bool ProgramRecord(size_t sector, size_t offset, const Record& rec);
    bool Compact(const Record* recs, size_t count);
    size_t SlotsPerSector() const { return m_flash.SectorSize() / m_slot; }

    IFlash& m_flash;
    size_t  m_slot;
    bool    m_mounted = false;
    int     m_active = NO_SECTOR;
    uint32_t m_generation = 0;
    size_t  m_writeOffset = 0;
    size_t  m_compactions = 0;
};

} // namespace param

#endif
