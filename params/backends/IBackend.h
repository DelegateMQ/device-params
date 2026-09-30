#ifndef _PARAM_IBACKEND_H
#define _PARAM_IBACKEND_H

// IBackend.h
// Persistence interface for ParamStore. A backend stores and loads records;
// it knows nothing about parameter definitions, ranges or defaults.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "../Param.h"
#include "delegate-mq/DelegateMQ.h"
#include <cstddef>
#include <cstdint>

namespace param {

/// One persisted parameter value.
struct Record {
    ParamId  id;
    TypeTag  type;
    uint8_t  len;       ///< Bytes used in data
    uint8_t  data[8];
    uint16_t crc;       ///< CRC16 over id, type, len and data (backends may ignore)
};

class IBackend {
public:
    virtual ~IBackend() = default;

    /// Deliver every stored record to sink. Returns false on a storage error.
    virtual bool ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink) = 0;

    /// Store a batch of records atomically where the medium allows (one batch per Commit()).
    virtual bool Write(const Record* recs, size_t count) = 0;

    /// Remove all stored records.
    virtual bool Erase() = 0;
};

} // namespace param

#endif
