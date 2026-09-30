#ifndef _PUMP_PARAMS_H
#define _PUMP_PARAMS_H

// PumpParams.h
// Example parameter table for a pump controller.

#include "params/Param.h"
#include <climits>

enum class Mode : uint8_t { Manual, Auto, Service };

namespace P {
    inline constexpr param::Key<int32_t> MaxRpm   {1};
    inline constexpr param::Key<float>   PidKp    {2};
    inline constexpr param::Key<bool>    Telemetry{3};
    inline constexpr param::Key<Mode>    RunMode  {4};
    inline constexpr param::Key<int32_t> SerialNo {5};
}

inline constexpr param::Def kPumpParams[] = {
    param::Int  (P::MaxRpm,    "motor.max_rpm", 3000, 0, 6000, param::PERSIST).Units("rpm"),
    param::Float(P::PidKp,     "pid.kp",        1.2f, 0.0f, 10.0f, param::PERSIST),
    param::Bool (P::Telemetry, "telemetry.on",  true),
    param::Enum (P::RunMode,   "run.mode",      Mode::Auto, Mode::Service, param::PERSIST | param::REBOOT_REQUIRED),
    param::Int  (P::SerialNo,  "sys.serial",    0, 0, INT_MAX, param::PERSIST | param::READ_ONLY),
};

#endif
