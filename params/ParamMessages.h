#ifndef _PARAM_MESSAGES_H
#define _PARAM_MESSAGES_H

// ParamMessages.h
// DataBus messages for remote parameter access. Topic names, message layouts
// and remote IDs are shared by the device (ParamService) and tools
// (ParamClient, dmq-param).
//
//   Topic                     Type             From -> To
//   <prefix>/param/list       ParamListReq     tool -> device
//   <prefix>/param/get        ParamGetReq      tool -> device
//   <prefix>/param/set        ParamSetReq      tool -> device
//   <prefix>/param/desc       ParamDescMsg     device -> tool (one per parameter, index/count)
//   <prefix>/param/value      ParamValueMsg    device -> tool (get/set reply)
//   <prefix>/param/changed    ParamValueMsg    device -> tool (every change, requestId 0)
//
// Values travel as a type tag plus 32 raw bits (see ToBits/FromBits).
// With DMQ_SERIALIZE_SERIALIZE the messages are serializable for the network;
// otherwise they work on the local DataBus only.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "Param.h"
#include "delegate-mq/DelegateMQ.h"
#include <cstdint>
#include <cstring>
#include <string>

namespace param {

#if defined(DMQ_SERIALIZE_SERIALIZE)
#define PARAM_MSG_BASE : public serialize::I
#define PARAM_MSG_OVERRIDE override
#else
#define PARAM_MSG_BASE
#define PARAM_MSG_OVERRIDE
#endif

/// Remote IDs used on the wire, offset from an application-chosen base.
enum class ParamRid : dmq::DelegateRemoteId { List = 0, Get, Set, Desc, Value, Changed, Count };

/// Topic names for a given prefix.
struct ParamTopics {
    dmq::xstring list, get, set, desc, value, changed;

    explicit ParamTopics(const std::string& prefix)
        : list(Make(prefix, "/param/list")), get(Make(prefix, "/param/get")), set(Make(prefix, "/param/set")),
          desc(Make(prefix, "/param/desc")), value(Make(prefix, "/param/value")), changed(Make(prefix, "/param/changed")) {}

private:
    static dmq::xstring Make(const std::string& prefix, const char* suffix) {
        dmq::xstring t(prefix.c_str());
        t += suffix;
        return t;
    }
};

inline constexpr size_t NAME_LEN = 32;
inline constexpr size_t UNITS_LEN = 8;

/// Copy src into a fixed char field, truncating and always NUL-terminating.
inline void CopyStr(char* dst, size_t size, const char* src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < size; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

struct ParamListReq PARAM_MSG_BASE {
    uint32_t requestId = 0;

#if defined(DMQ_SERIALIZE_SERIALIZE)
    std::istream& read(serialize& ms, std::istream& is) PARAM_MSG_OVERRIDE { return ms.read(is, requestId); }
    std::ostream& write(serialize& ms, std::ostream& os) PARAM_MSG_OVERRIDE { return ms.write(os, requestId); }
#endif
};

struct ParamGetReq PARAM_MSG_BASE {
    uint32_t requestId = 0;
    ParamId  id = 0;

#if defined(DMQ_SERIALIZE_SERIALIZE)
    std::istream& read(serialize& ms, std::istream& is) PARAM_MSG_OVERRIDE {
        ms.read(is, requestId);
        return ms.read(is, id);
    }
    std::ostream& write(serialize& ms, std::ostream& os) PARAM_MSG_OVERRIDE {
        ms.write(os, requestId);
        return ms.write(os, id);
    }
#endif
};

struct ParamSetReq PARAM_MSG_BASE {
    uint32_t requestId = 0;
    ParamId  id = 0;
    uint8_t  type = 0;      ///< TypeTag
    uint32_t bits = 0;

    Value GetValue(bool& ok) const { Value v; ok = FromBits(static_cast<TypeTag>(type), bits, v); return v; }
    void  SetValue(const Value& v) { type = static_cast<uint8_t>(v.type); bits = ToBits(v); }

#if defined(DMQ_SERIALIZE_SERIALIZE)
    std::istream& read(serialize& ms, std::istream& is) PARAM_MSG_OVERRIDE {
        ms.read(is, requestId);
        ms.read(is, id);
        ms.read(is, type);
        return ms.read(is, bits);
    }
    std::ostream& write(serialize& ms, std::ostream& os) PARAM_MSG_OVERRIDE {
        ms.write(os, requestId);
        ms.write(os, id);
        ms.write(os, type);
        return ms.write(os, bits);
    }
#endif
};

/// Get/set reply and change notification.
struct ParamValueMsg PARAM_MSG_BASE {
    uint32_t requestId = 0;     ///< 0 for a change notification
    ParamId  id = 0;
    uint8_t  result = 0;        ///< SetResult (OK for get replies and notifications)
    uint8_t  source = 0;        ///< Source of the change (notifications)
    uint8_t  type = 0;          ///< TypeTag of the current value
    uint32_t bits = 0;          ///< Current value (after the set, successful or not)

    Value GetValue(bool& ok) const { Value v; ok = FromBits(static_cast<TypeTag>(type), bits, v); return v; }
    void  SetValue(const Value& v) { type = static_cast<uint8_t>(v.type); bits = ToBits(v); }

#if defined(DMQ_SERIALIZE_SERIALIZE)
    std::istream& read(serialize& ms, std::istream& is) PARAM_MSG_OVERRIDE {
        ms.read(is, requestId);
        ms.read(is, id);
        ms.read(is, result);
        ms.read(is, source);
        ms.read(is, type);
        return ms.read(is, bits);
    }
    std::ostream& write(serialize& ms, std::ostream& os) PARAM_MSG_OVERRIDE {
        ms.write(os, requestId);
        ms.write(os, id);
        ms.write(os, result);
        ms.write(os, source);
        ms.write(os, type);
        return ms.write(os, bits);
    }
#endif
};

/// One parameter's full description plus current value (list reply).
struct ParamDescMsg PARAM_MSG_BASE {
    uint32_t requestId = 0;
    uint16_t index = 0;         ///< 0..count-1 among listed (non-hidden) parameters
    uint16_t count = 0;
    ParamId  id = 0;
    uint8_t  type = 0;
    uint32_t flags = 0;
    uint32_t defBits = 0, minBits = 0, maxBits = 0, valueBits = 0;
    char     name[NAME_LEN] = {};
    char     units[UNITS_LEN] = {};

#if defined(DMQ_SERIALIZE_SERIALIZE)
    std::istream& read(serialize& ms, std::istream& is) PARAM_MSG_OVERRIDE {
        ms.read(is, requestId);
        ms.read(is, index);
        ms.read(is, count);
        ms.read(is, id);
        ms.read(is, type);
        ms.read(is, flags);
        ms.read(is, defBits);
        ms.read(is, minBits);
        ms.read(is, maxBits);
        ms.read(is, valueBits);
        ms.read(is, name, sizeof(name));
        ms.read(is, units, sizeof(units));
        name[NAME_LEN - 1] = units[UNITS_LEN - 1] = '\0';
        return is;
    }
    std::ostream& write(serialize& ms, std::ostream& os) PARAM_MSG_OVERRIDE {
        ms.write(os, requestId);
        ms.write(os, index);
        ms.write(os, count);
        ms.write(os, id);
        ms.write(os, type);
        ms.write(os, flags);
        ms.write(os, defBits);
        ms.write(os, minBits);
        ms.write(os, maxBits);
        ms.write(os, valueBits);
        ms.write(os, static_cast<const char*>(name));
        return ms.write(os, static_cast<const char*>(units));
    }
#endif
};

#if defined(DMQ_SERIALIZE_SERIALIZE)
/// Serializer instances for NetworkNode::Send/Receive.
struct ParamSerializers {
    dmq::serialization::serializer::Serializer<void(ParamListReq)>  list;
    dmq::serialization::serializer::Serializer<void(ParamGetReq)>   get;
    dmq::serialization::serializer::Serializer<void(ParamSetReq)>   set;
    dmq::serialization::serializer::Serializer<void(ParamDescMsg)>  desc;
    dmq::serialization::serializer::Serializer<void(ParamValueMsg)> value;

    static ParamSerializers& Instance() {
        static ParamSerializers s;
        return s;
    }
};

inline dmq::DelegateRemoteId Rid(dmq::DelegateRemoteId base, ParamRid r) {
    return static_cast<dmq::DelegateRemoteId>(base + static_cast<dmq::DelegateRemoteId>(r));
}

/// Device side: receive requests, send replies and notifications.
/// Net is a dmq::databus::NetworkNode<Transport>.
template <class Net>
void ExposeParamsOnNetwork(Net& net, const std::string& prefix, dmq::DelegateRemoteId baseRid)
{
    ParamTopics t(prefix);
    auto& s = ParamSerializers::Instance();
    net.template Receive<ParamListReq>(t.list, Rid(baseRid, ParamRid::List), s.list);
    net.template Receive<ParamGetReq>(t.get, Rid(baseRid, ParamRid::Get), s.get);
    net.template Receive<ParamSetReq>(t.set, Rid(baseRid, ParamRid::Set), s.set);
    net.template Send<ParamDescMsg>(t.desc, Rid(baseRid, ParamRid::Desc), s.desc);
    net.template Send<ParamValueMsg>(t.value, Rid(baseRid, ParamRid::Value), s.value);
    net.template Send<ParamValueMsg>(t.changed, Rid(baseRid, ParamRid::Changed), s.value);
}

/// Tool side: the mirror of ExposeParamsOnNetwork.
template <class Net>
void AttachParamClientToNetwork(Net& net, const std::string& prefix, dmq::DelegateRemoteId baseRid)
{
    ParamTopics t(prefix);
    auto& s = ParamSerializers::Instance();
    net.template Send<ParamListReq>(t.list, Rid(baseRid, ParamRid::List), s.list);
    net.template Send<ParamGetReq>(t.get, Rid(baseRid, ParamRid::Get), s.get);
    net.template Send<ParamSetReq>(t.set, Rid(baseRid, ParamRid::Set), s.set);
    net.template Receive<ParamDescMsg>(t.desc, Rid(baseRid, ParamRid::Desc), s.desc);
    net.template Receive<ParamValueMsg>(t.value, Rid(baseRid, ParamRid::Value), s.value);
    net.template Receive<ParamValueMsg>(t.changed, Rid(baseRid, ParamRid::Changed), s.value);
}
#endif

#undef PARAM_MSG_BASE
#undef PARAM_MSG_OVERRIDE

} // namespace param

#endif
