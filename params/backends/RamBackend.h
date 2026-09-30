#ifndef _PARAM_RAM_BACKEND_H
#define _PARAM_RAM_BACKEND_H

// RamBackend.h
// In-memory backend for unit tests. Contents are lost on destruction.
// Thread-safe, so tests can inspect it while a save thread writes.
// Test hooks: FailWrites() makes Write() fail; Corrupt() damages a stored record.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "IBackend.h"
#include "delegate-mq/extras/util/Fault.h"
#include <array>

namespace param {

class RamBackend : public IBackend {
public:
    bool ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink) override {
        std::array<Record, MAX_COUNT> copy;
        size_t n;
        {
            dmq::LockGuard<dmq::Mutex> lock(m_lock);
            copy = m_records;
            n = m_count;
        }
        for (size_t i = 0; i < n; i++)
            sink(copy[i]);      // outside the lock: the sink may call back in
        return true;
    }

    bool Write(const Record* recs, size_t count) override {
        dmq::LockGuard<dmq::Mutex> lock(m_lock);
        if (!m_failWrites) {
            for (size_t k = 0; k < count; k++) {
                size_t i = 0;
                while (i < m_count && m_records[i].id != recs[k].id)
                    i++;
                if (i == m_count) {
                    DMQ_ASSERT_TRUE(m_count < m_records.size());
                    m_count++;
                }
                m_records[i] = recs[k];
            }
        }
        m_writeCount++;         // counted once the records are in place
        return !m_failWrites;
    }

    bool Erase() override {
        dmq::LockGuard<dmq::Mutex> lock(m_lock);
        m_count = 0;
        return true;
    }

    size_t RecordCount() const { dmq::LockGuard<dmq::Mutex> lock(m_lock); return m_count; }
    size_t WriteCount() const  { dmq::LockGuard<dmq::Mutex> lock(m_lock); return m_writeCount; }
    void   FailWrites(bool fail) { dmq::LockGuard<dmq::Mutex> lock(m_lock); m_failWrites = fail; }

    /// Store a record as-is (e.g. with a wrong type) to test load handling.
    void Inject(const Record& rec) { Write(&rec, 1); }

    /// Flip a data bit in the record for id, invalidating its CRC.
    void Corrupt(ParamId id) {
        dmq::LockGuard<dmq::Mutex> lock(m_lock);
        for (size_t i = 0; i < m_count; i++)
            if (m_records[i].id == id)
                m_records[i].data[0] ^= 0x01;
    }

private:
    mutable dmq::Mutex m_lock;
    std::array<Record, MAX_COUNT> m_records{};
    size_t m_count = 0;
    size_t m_writeCount = 0;
    bool   m_failWrites = false;
};

} // namespace param

#endif
