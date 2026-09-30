#ifndef _SIM_FLASH_H
#define _SIM_FLASH_H

// SimFlash.h
// Two-sector NOR flash simulation for FlashLogBackend tests. Enforces NOR
// rules (erase sets 0xFF, program only clears bits, aligned writes) and can
// simulate power loss after a number of program operations.

#include "params/backends/FlashLogBackend.h"
#include <array>
#include <cstring>
#include <vector>

class SimFlash : public param::IFlash {
public:
    SimFlash(size_t sectorSize, size_t writeSize)
        : m_sectorSize(sectorSize), m_writeSize(writeSize) {
        for (auto& s : m_sectors)
            s.assign(sectorSize, 0xFF);
    }

    size_t SectorSize() const override { return m_sectorSize; }
    size_t WriteSize() const override { return m_writeSize; }

    bool Read(size_t sector, size_t offset, void* buf, size_t len) override {
        if (sector > 1 || offset + len > m_sectorSize)
            return false;
        std::memcpy(buf, &m_sectors[sector][offset], len);
        return true;
    }

    bool Program(size_t sector, size_t offset, const void* buf, size_t len) override {
        if (sector > 1 || offset + len > m_sectorSize)
            return false;
        if (offset % m_writeSize || len % m_writeSize) {
            m_violations++;
            return false;
        }
        const auto* src = static_cast<const uint8_t*>(buf);
        if (m_programsLeft == 0)
            return false;   // power is off
        if (m_programsLeft > 0 && --m_programsLeft == 0 && m_tearLastWrite) {
            // Torn write: only the first half lands
            len /= 2;
        }
        for (size_t i = 0; i < len; i++) {
            uint8_t& cell = m_sectors[sector][offset + i];
            if ((cell & src[i]) != src[i])
                m_violations++;         // tried to set a 0 bit back to 1
            cell &= src[i];
        }
        m_programs++;
        return true;
    }

    bool EraseSector(size_t sector) override {
        if (sector > 1 || m_programsLeft == 0)
            return false;
        m_sectors[sector].assign(m_sectorSize, 0xFF);
        m_erases++;
        return true;
    }

    /// Allow `n` more program operations, then fail everything (power loss).
    /// With tear, the last allowed program only writes half its bytes.
    void CutPowerAfter(int n, bool tear = false) { m_programsLeft = n; m_tearLastWrite = tear; }
    void RestorePower() { m_programsLeft = -1; m_tearLastWrite = false; }

    size_t Erases() const { return m_erases; }
    size_t Programs() const { return m_programs; }
    size_t Violations() const { return m_violations; }

private:
    size_t m_sectorSize;
    size_t m_writeSize;
    std::array<std::vector<uint8_t>, 2> m_sectors;
    int    m_programsLeft = -1;     // -1 = unlimited
    bool   m_tearLastWrite = false;
    size_t m_erases = 0;
    size_t m_programs = 0;
    size_t m_violations = 0;
};

#endif
