#ifndef _PARAM_CONFIG_H
#define _PARAM_CONFIG_H

// ParamConfig.h
// Compile-time tunables for device-params. Override any of these by defining
// the macro before this header is included (e.g. on the compiler command line).
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include <cstddef>
#include <cstdint>

// Maximum number of parameters a single ParamStore can hold.
#ifndef PARAM_MAX_COUNT
#define PARAM_MAX_COUNT 64
#endif

// Default delay before a Deferred save writes pending changes (milliseconds).
#ifndef PARAM_SAVE_DELAY_MS
#define PARAM_SAVE_DELAY_MS 1000
#endif

// Define to compile parameter names out of Def (tools then use IDs only).
// #define PARAM_NO_NAMES

namespace param {

/// Maximum parameters per store. See PARAM_MAX_COUNT.
inline constexpr size_t MAX_COUNT = PARAM_MAX_COUNT;

/// Default Deferred save delay in milliseconds. See PARAM_SAVE_DELAY_MS.
inline constexpr uint32_t SAVE_DELAY_MS = PARAM_SAVE_DELAY_MS;

} // namespace param

#endif
