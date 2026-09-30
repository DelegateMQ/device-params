#ifndef _PARAM_RECORD_H
#define _PARAM_RECORD_H

// Record.h
// Converts between Value and Record, and packs a Record into a fixed
// little-endian byte layout for backends that store raw bytes:
//
//   offset  size  field
//   0       2     id
//   2       1     type
//   3       1     len
//   4       8     data
//   12      2     crc (CRC16-CCITT over bytes 0..11)
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "Param.h"
#include "backends/IBackend.h"
#include "delegate-mq/extras/util/crc16.h"
#include <cstring>

namespace param {

inline constexpr size_t PACKED_RECORD_SIZE = 14;

inline uint16_t RecordCrc(const Record& rec)
{
    uint8_t buf[12];
    buf[0] = static_cast<uint8_t>(rec.id & 0xFF);
    buf[1] = static_cast<uint8_t>(rec.id >> 8);
    buf[2] = static_cast<uint8_t>(rec.type);
    buf[3] = rec.len;
    std::memcpy(&buf[4], rec.data, sizeof(rec.data));
    return dmq::util::Crc16CalcBlock(buf, static_cast<int>(sizeof(buf)));
}

/// Build a record (with CRC) for a parameter value.
inline Record EncodeRecord(ParamId id, const Value& v)
{
    Record rec{};
    rec.id = id;
    rec.type = v.type;
    const uint32_t bits = ToBits(v);
    rec.len = (v.type == TypeTag::Bool) ? 1 : 4;
    for (int i = 0; i < rec.len; i++)
        rec.data[i] = static_cast<uint8_t>(bits >> (8 * i));
    rec.crc = RecordCrc(rec);
    return rec;
}

/// Decode a record into a value. Returns false on a bad CRC, unknown type or bad length.
inline bool DecodeRecord(const Record& rec, Value& out)
{
    if (rec.crc != RecordCrc(rec))
        return false;
    if (rec.len != ((rec.type == TypeTag::Bool) ? 1 : 4))
        return false;

    uint32_t bits = 0;
    for (int i = 0; i < rec.len; i++)
        bits |= static_cast<uint32_t>(rec.data[i]) << (8 * i);
    return FromBits(rec.type, bits, out);
}

/// Pack a record into PACKED_RECORD_SIZE bytes.
inline void PackRecord(const Record& rec, uint8_t* out)
{
    out[0] = static_cast<uint8_t>(rec.id & 0xFF);
    out[1] = static_cast<uint8_t>(rec.id >> 8);
    out[2] = static_cast<uint8_t>(rec.type);
    out[3] = rec.len;
    std::memcpy(&out[4], rec.data, sizeof(rec.data));
    out[12] = static_cast<uint8_t>(rec.crc & 0xFF);
    out[13] = static_cast<uint8_t>(rec.crc >> 8);
}

/// Unpack PACKED_RECORD_SIZE bytes. CRC is copied, not checked (DecodeRecord checks it).
inline Record UnpackRecord(const uint8_t* in)
{
    Record rec{};
    rec.id = static_cast<ParamId>(in[0] | (in[1] << 8));
    rec.type = static_cast<TypeTag>(in[2]);
    rec.len = in[3];
    std::memcpy(rec.data, &in[4], sizeof(rec.data));
    rec.crc = static_cast<uint16_t>(in[12] | (in[13] << 8));
    return rec;
}

} // namespace param

#endif
