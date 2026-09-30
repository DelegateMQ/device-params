![License MIT](https://img.shields.io/badge/license-MIT-blue)
[![CI](https://github.com/DelegateMQ/device-params/actions/workflows/ci.yml/badge.svg)](https://github.com/DelegateMQ/device-params/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20RTOS%20%7C%20Bare--Metal-informational)
![Status](https://img.shields.io/badge/status-early-orange)

# Device Params in C++

**Typed device parameters for embedded C++.** Declare your device's settings once, in a table. device-params gives you range-checked access from any thread, change notification on the thread you choose, power-loss-safe persistence, and remote get/set over any transport.

Almost every device has settings such as limits, calibration values, operating modes and IDs. Each one needs a default, a valid range, a place in non-volatile storage, and often a way for a service tool to read or change it. device-params is that layer, built on [DelegateMQ](https://github.com/DelegateMQ/DelegateMQ).

## Features

- **Declarative table:** each parameter has a stable ID, name, type, default, range and flags.
- **Type-safe:** `Get`/`Set` are checked at compile time; every set is range-checked.
- **Change notification:** synchronous, or on any thread, without overflowing its queue.
- **Persistence:** manual, immediate or deferred saves to any storage, with retry on failure.
- **Remote access:** list, get and set parameters from a tool over the DelegateMQ DataBus.
- **Embedded first:** runs on bare metal with no threads; about 9 KB of code on Cortex-M4.

## Supported Integrations

Storage is pluggable: support for another medium (EEPROM, FRAM, a different flash layout, a database) is one small class implementing `IBackend` (three methods). A new flash part needs only an `IFlash` driver (read, program, erase). Operating systems and transports come from DelegateMQ, so any DelegateMQ port works.

| Category | Supported |
| :--- | :--- |
| **Persistent Storage** | NOR flash (power-loss-safe log, any program size from 1 to 32 bytes), file, [SQLite](https://sqlite.org/), RAM (tests), or your own `IBackend` |
| **Operating Systems** | Windows, Linux, POSIX, FreeRTOS, ThreadX, Zephyr, CMSIS-RTOS2, NuttX, Qt, bare metal (via [DelegateMQ](https://github.com/DelegateMQ/DelegateMQ) ports) |
| **Transport** (remote access) | Any DelegateMQ transport: UDP, TCP, serial port, [ZeroMQ](https://zeromq.org/), [NNG](https://github.com/nanomsg/nng), [MQTT](https://github.com/eclipse-paho/paho.mqtt.c), ARM LwIP, ThreadX NetX/Duo, Zephyr networking, and more |
| **Serialization** (remote access) | DelegateMQ's built-in [MessageSerialize](https://github.com/endurodave/MessageSerialize) |

## Quick Start

```bash
git clone https://github.com/DelegateMQ/device-params.git
cd device-params
cmake -B build .
cmake --build build
./build/DeviceParamsApp          # Windows: build\Debug\DeviceParamsApp.exe
```

Define your parameters once. Each ID identifies the parameter in persistent storage, so never reuse or renumber one.

```cpp
enum class Mode : uint8_t { Manual, Auto, Service };

namespace P {
    inline constexpr param::Key<int32_t> MaxRpm {1};
    inline constexpr param::Key<float>   PidKp  {2};
    inline constexpr param::Key<Mode>    RunMode{3};
}

inline constexpr param::Def kParams[] = {
    param::Int  (P::MaxRpm,  "motor.max_rpm", 3000, 0, 6000, param::PERSIST).Units("rpm"),
    param::Float(P::PidKp,   "pid.kp",        1.2f, 0.0f, 10.0f, param::PERSIST),
    param::Enum (P::RunMode, "run.mode",      Mode::Auto, Mode::Service, param::PERSIST),
};
```

Then use them:

```cpp
param::FileBackend backend("settings.params");
param::ParamStore store(kParams, &backend);
store.Init();                               // defaults, then saved values

int32_t rpm = store.Get(P::MaxRpm);         // typed
store.Set(P::MaxRpm, 4500);                 // returns SetResult::OK
store.Set(P::MaxRpm, 9000);                 // returns SetResult::OUT_OF_RANGE
store.Commit();                             // save to the backend
```

## Examples

### Change notification

```cpp
auto conn = store.Subscribe(P::MaxRpm, [](int32_t rpm, param::Source) {
    motor.SetLimit(rpm);                    // runs on motorThread
}, &motorThread);
```

Keep the returned connection; letting it go out of scope unsubscribes. A thread subscriber always receives the latest value, so a burst of changes can't overflow its queue.

### Persistence

```cpp
// Save changes 2 seconds after the last one, on saveThread
store.SetSaveMode(param::SaveMode::Deferred, std::chrono::milliseconds(2000), &saveThread);
```

On desktop or embedded Linux, SQLite keeps settings in an ordinary database that any SQLite tool can open (see `examples/SqliteExample.cpp`):

```cpp
param::SqliteBackend backend("settings.db");
param::ParamStore store(kParams, &backend);
```

For flash, implement a small driver for two sectors of your part:

```cpp
class MyFlash : public param::IFlash {
public:
    size_t SectorSize() const override { return 4096; }
    size_t WriteSize() const override { return 8; }
    bool Read(size_t sector, size_t offset, void* buf, size_t len) override;
    bool Program(size_t sector, size_t offset, const void* buf, size_t len) override;
    bool EraseSector(size_t sector) override;
};

MyFlash flash;
param::FlashLogBackend backend(flash);
```

### Bare metal

With no threads, deferred saves run from your main loop, never from an interrupt:

```cpp
store.SetSaveMode(param::SaveMode::Deferred, std::chrono::milliseconds(2000));

void SysTick_Handler() { dmq::util::Timer::ProcessTimers(); }

for (;;) {
    store.Poll();                           // saves when due
}
```

### Remote access

```cpp
// Device
param::ParamService service(store, "pump");
service.AllowRemoteSet(true);               // off by default
service.Start(&paramThread);

// Tool
param::ParamClient client("pump");
client.Start();
client.RequestList();
client.RequestSet(P::MaxRpm.id, param::Value(int32_t(4500)));
```

Remote sets go through the same range checks as local ones.

## Building

Requires a C++17 compiler and CMake 3.10+. DelegateMQ is included under `delegate-mq/`.

| CMake option | Default | Effect |
| :--- | :--- | :--- |
| `PARAM_REMOTE` | `ON` | Build remote access (`ParamService`, `ParamClient`) |
| `PARAM_BACKEND_SQLITE` | `ON` if SQLite is present | Build the SQLite backend |

To use it in your project, add `params/` and `delegate-mq/` to your build. On a microcontroller the core is `ParamStore.cpp` plus a backend such as `FlashLogBackend.cpp`.

## Testing

`DeviceParamsApp` runs the unit tests and randomized chaos tests: power loss and bit rot on flash, heavy multithreaded use, and remote request floods.

```bash
DeviceParamsApp --chaos-seed 1234    # replay a run (the seed is printed)
DeviceParamsApp --no-chaos           # unit tests only
```

## License

[MIT](LICENSE) © David Lafreniere
