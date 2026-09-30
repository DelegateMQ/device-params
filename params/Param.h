#ifndef _PARAM_H
#define _PARAM_H

// Param.h
// Core parameter types: IDs, typed keys, values and the constexpr
// definition table the application declares once.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "ParamConfig.h"
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace param {

/// Stable parameter ID. Used in flash records and on the wire, so never reuse
/// or renumber an ID once released.
using ParamId = uint16_t;

enum class TypeTag : uint8_t { Bool, Int32, UInt32, Float, Enum };

enum class SetResult : uint8_t { OK, UNKNOWN_ID, TYPE_MISMATCH, OUT_OF_RANGE, READ_ONLY, REJECTED, REMOTE_DISABLED };
enum class Source    : uint8_t { Local, Remote, Load, Reset };

/// Parameter flags (bitmask).
enum Flags : uint32_t {
    NONE            = 0,
    PERSIST         = 1u << 0,  ///< Saved to the backend
    READ_ONLY       = 1u << 1,  ///< Remote set rejected; application may still set it
    REBOOT_REQUIRED = 1u << 2,  ///< New value takes effect after restart
    HIDDEN          = 1u << 3,  ///< Not listed to remote tools
};

/// Maps a C++ type to its TypeTag. Enums are stored as int32.
template <class T, class = void> struct TypeOf;
template <> struct TypeOf<bool>     { static constexpr TypeTag tag = TypeTag::Bool; };
template <> struct TypeOf<int32_t>  { static constexpr TypeTag tag = TypeTag::Int32; };
template <> struct TypeOf<uint32_t> { static constexpr TypeTag tag = TypeTag::UInt32; };
template <> struct TypeOf<float>    { static constexpr TypeTag tag = TypeTag::Float; };
template <class T> struct TypeOf<T, std::enable_if_t<std::is_enum_v<T>>> { static constexpr TypeTag tag = TypeTag::Enum; };

/// Blocks template deduction (C++20 std::type_identity).
template <class T> struct Identity { using type = T; };
template <class T> using IdentityT = typename Identity<T>::type;

/// Tagged value holding any parameter type (at most 4 bytes of payload).
struct Value {
    TypeTag type;
    union {
        bool     b;
        int32_t  i;
        uint32_t u;
        float    f;
    };

    constexpr Value()           : type(TypeTag::Int32), i(0) {}
    constexpr Value(bool v)     : type(TypeTag::Bool), b(v) {}
    constexpr Value(int32_t v)  : type(TypeTag::Int32), i(v) {}
    constexpr Value(uint32_t v) : type(TypeTag::UInt32), u(v) {}
    constexpr Value(float v)    : type(TypeTag::Float), f(v) {}

    template <class E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
    static constexpr Value FromEnum(E v) {
        Value r(static_cast<int32_t>(v));
        r.type = TypeTag::Enum;
        return r;
    }

    /// Read as T. Caller must ensure type matches (the store checks this).
    template <class T> T As() const {
        if constexpr (std::is_same_v<T, bool>)          return b;
        else if constexpr (std::is_same_v<T, int32_t>)  return i;
        else if constexpr (std::is_same_v<T, uint32_t>) return u;
        else if constexpr (std::is_same_v<T, float>)    return f;
        else if constexpr (std::is_enum_v<T>)           return static_cast<T>(i);
    }
};

/// Raw 32-bit payload of a value (little-endian on the wire / in storage).
inline uint32_t ToBits(const Value& v) {
    switch (v.type) {
    case TypeTag::Bool:   return v.b ? 1u : 0u;
    case TypeTag::UInt32: return v.u;
    case TypeTag::Float:  { uint32_t bits; std::memcpy(&bits, &v.f, sizeof(bits)); return bits; }
    case TypeTag::Int32:
    case TypeTag::Enum:   return static_cast<uint32_t>(v.i);
    }
    return 0;
}

/// Inverse of ToBits. Returns false for an unknown type tag.
inline bool FromBits(TypeTag type, uint32_t bits, Value& out) {
    switch (type) {
    case TypeTag::Bool:   out = Value(bits != 0); return true;
    case TypeTag::UInt32: out = Value(bits); return true;
    case TypeTag::Float:  { float f; std::memcpy(&f, &bits, sizeof(f)); out = Value(f); return true; }
    case TypeTag::Int32:  out = Value(static_cast<int32_t>(bits)); return true;
    case TypeTag::Enum:   out = Value(static_cast<int32_t>(bits)); out.type = TypeTag::Enum; return true;
    }
    return false;
}

/// Compile-time typed handle to a parameter.
template <class T>
struct Key {
    ParamId id;
    constexpr explicit Key(ParamId i) : id(i) {}
};

/// One entry in the application's constexpr parameter table.
struct Def {
    ParamId     id;
    TypeTag     type;
    Value       defValue;
    Value       minValue;
    Value       maxValue;
    uint32_t    flags;
#ifndef PARAM_NO_NAMES
    const char* name;
    const char* units;
#endif

    /// Builder-style units, e.g. param::Int(...).Units("rpm").
    constexpr Def Units([[maybe_unused]] const char* u) const {
        Def d = *this;
#ifndef PARAM_NO_NAMES
        d.units = u;
#endif
        return d;
    }
};

namespace detail {
constexpr Def MakeDef(ParamId id, TypeTag type, Value def, Value mn, Value mx,
                      uint32_t flags, [[maybe_unused]] const char* name) {
#ifndef PARAM_NO_NAMES
    return Def{ id, type, def, mn, mx, flags, name, nullptr };
#else
    return Def{ id, type, def, mn, mx, flags };
#endif
}
} // namespace detail

// Definition builders
constexpr Def Bool(Key<bool> k, const char* name, bool def, uint32_t flags = NONE) {
    return detail::MakeDef(k.id, TypeTag::Bool, Value(def), Value(false), Value(true), flags, name);
}

constexpr Def Int(Key<int32_t> k, const char* name, int32_t def, int32_t mn, int32_t mx, uint32_t flags = NONE) {
    return detail::MakeDef(k.id, TypeTag::Int32, Value(def), Value(mn), Value(mx), flags, name);
}

constexpr Def UInt(Key<uint32_t> k, const char* name, uint32_t def, uint32_t mn, uint32_t mx, uint32_t flags = NONE) {
    return detail::MakeDef(k.id, TypeTag::UInt32, Value(def), Value(mn), Value(mx), flags, name);
}

constexpr Def Float(Key<float> k, const char* name, float def, float mn, float mx, uint32_t flags = NONE) {
    return detail::MakeDef(k.id, TypeTag::Float, Value(def), Value(mn), Value(mx), flags, name);
}

/// Enum parameter; valid range is [0, maxValue].
template <class E>
constexpr Def Enum(Key<E> k, const char* name, E def, E maxValue, uint32_t flags = NONE) {
    static_assert(std::is_enum_v<E>, "Enum() requires an enum type");
    return detail::MakeDef(k.id, TypeTag::Enum, Value::FromEnum(def),
                           Value::FromEnum(static_cast<E>(0)), Value::FromEnum(maxValue), flags, name);
}

} // namespace param

#endif
