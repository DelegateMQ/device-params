#include "FlashLogBackend.h"
#include "../Record.h"
#include "delegate-mq/extras/util/Fault.h"
#include <array>
#include <cstring>

namespace param {

namespace {

void PutU32(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t GetU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

FlashLogBackend::FlashLogBackend(IFlash& flash) : m_flash(flash)
{
    const size_t ws = m_flash.WriteSize();
    DMQ_ASSERT_TRUE(ws > 0 && ws <= MAX_SLOT);
    m_slot = ((PACKED_RECORD_SIZE + ws - 1) / ws) * ws;
    DMQ_ASSERT_TRUE(m_slot <= MAX_SLOT);
    DMQ_ASSERT_TRUE(SlotsPerSector() >= 2);
}

bool FlashLogBackend::ReadSlot(size_t sector, size_t offset, uint8_t* buf)
{
    return m_flash.Read(sector, offset, buf, m_slot);
}

bool FlashLogBackend::IsErased(const uint8_t* buf) const
{
    for (size_t i = 0; i < m_slot; i++)
        if (buf[i] != 0xFF)
            return false;
    return true;
}

bool FlashLogBackend::ReadHeader(size_t sector, uint32_t& generation)
{
    uint8_t buf[MAX_SLOT];
    if (!ReadSlot(sector, 0, buf))
        return false;
    generation = GetU32(&buf[4]);
    return GetU32(buf) == MAGIC && generation != 0xFFFFFFFFu;
}

bool FlashLogBackend::Mount()
{
    if (m_mounted)
        return true;

    uint32_t gen0 = 0, gen1 = 0;
    const bool valid0 = ReadHeader(0, gen0);
    const bool valid1 = ReadHeader(1, gen1);

    if (valid0 && valid1)
        // Newer generation wins; the difference handles wraparound
        m_active = (static_cast<int32_t>(gen1 - gen0) > 0) ? 1 : 0;
    else if (valid0)
        m_active = 0;
    else if (valid1)
        m_active = 1;
    else
        m_active = NO_SECTOR;

    m_generation = (m_active == 1) ? gen1 : gen0;
    m_writeOffset = 0;

    if (m_active != NO_SECTOR) {
        // Find the end of the log: the first erased slot after the header
        uint8_t buf[MAX_SLOT];
        size_t offset = m_slot;
        const size_t end = SlotsPerSector() * m_slot;
        while (offset < end) {
            if (!ReadSlot(static_cast<size_t>(m_active), offset, buf))
                return false;
            if (IsErased(buf))
                break;
            offset += m_slot;
        }
        m_writeOffset = offset;
    }

    m_mounted = true;
    return true;
}

bool FlashLogBackend::ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink)
{
    if (!Mount())
        return false;
    if (m_active == NO_SECTOR)
        return true;

    // Deliver in log order; later records for an ID override earlier ones,
    // and a record with a bad CRC is rejected by the store (previous value kept).
    uint8_t buf[MAX_SLOT];
    for (size_t offset = m_slot; offset < m_writeOffset; offset += m_slot) {
        if (!ReadSlot(static_cast<size_t>(m_active), offset, buf))
            return false;
        sink(UnpackRecord(buf));
    }
    return true;
}

bool FlashLogBackend::ProgramRecord(size_t sector, size_t offset, const Record& rec)
{
    DMQ_ASSERT_TRUE(rec.id != 0xFFFF);
    uint8_t buf[MAX_SLOT];
    std::memset(buf, 0xFF, sizeof(buf));
    PackRecord(rec, buf);
    return m_flash.Program(sector, offset, buf, m_slot);
}

bool FlashLogBackend::Write(const Record* recs, size_t count)
{
    if (count == 0)
        return true;
    if (!Mount())
        return false;

    const size_t end = SlotsPerSector() * m_slot;
    if (m_active != NO_SECTOR && m_writeOffset + count * m_slot <= end) {
        for (size_t k = 0; k < count; k++) {
            if (!ProgramRecord(static_cast<size_t>(m_active), m_writeOffset, recs[k]))
                return false;
            m_writeOffset += m_slot;
        }
        return true;
    }
    return Compact(recs, count);
}

bool FlashLogBackend::Compact(const Record* recs, size_t count)
{
    // Newest valid record per ID: the new batch first, then the active log
    // newest-to-oldest, keeping the first seen for each ID.
    std::array<Record, MAX_COUNT> keep{};
    size_t n = 0;
    auto add = [&](const Record& rec) {
        for (size_t i = 0; i < n; i++)
            if (keep[i].id == rec.id)
                return true;
        if (n == keep.size())
            return false;
        keep[n++] = rec;
        return true;
    };

    for (size_t k = count; k-- > 0;)
        if (!add(recs[k]))
            return false;

    if (m_active != NO_SECTOR) {
        uint8_t buf[MAX_SLOT];
        for (size_t offset = m_writeOffset; offset > m_slot;) {
            offset -= m_slot;
            if (!ReadSlot(static_cast<size_t>(m_active), offset, buf))
                return false;
            Record rec = UnpackRecord(buf);
            Value unused;
            if (DecodeRecord(rec, unused) && !add(rec))
                return false;
        }
    }

    if ((n + 1) * m_slot > SlotsPerSector() * m_slot)
        return false;   // sector too small for one record per parameter

    const size_t target = (m_active == 0) ? 1 : 0;
    if (!m_flash.EraseSector(target))
        return false;

    size_t offset = m_slot;
    for (size_t i = 0; i < n; i++) {
        if (!ProgramRecord(target, offset, keep[i]))
            return false;
        offset += m_slot;
    }

    // Header last: only now does the new sector become active
    uint8_t header[MAX_SLOT];
    std::memset(header, 0xFF, sizeof(header));
    const uint32_t gen = (m_active == NO_SECTOR) ? 1 : m_generation + 1;
    PutU32(&header[0], MAGIC);
    PutU32(&header[4], gen == 0xFFFFFFFFu ? 0 : gen);
    if (!m_flash.Program(target, 0, header, m_slot))
        return false;

    m_active = static_cast<int>(target);
    m_generation = GetU32(&header[4]);
    m_writeOffset = offset;
    m_compactions++;
    return true;
}

bool FlashLogBackend::Erase()
{
    bool ok = m_flash.EraseSector(0);
    ok = m_flash.EraseSector(1) && ok;
    m_mounted = true;
    m_active = NO_SECTOR;
    m_generation = 0;
    m_writeOffset = 0;
    return ok;
}

} // namespace param
