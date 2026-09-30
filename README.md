![License MIT](https://img.shields.io/badge/license-MIT-blue)
[![CI](https://github.com/DelegateMQ/device-params/actions/workflows/ci.yml/badge.svg)](https://github.com/DelegateMQ/device-params/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20RTOS%20%7C%20Bare--Metal-informational)
![Status](https://img.shields.io/badge/status-early-orange)

# device-params

**Typed device parameters for embedded C++.** Declare your device's settings once, in a table. device-params gives you range-checked access from any thread, change notification on the thread you choose, power-loss-safe persistence to flash, a file or SQLite, and remote get/set over any transport.

Every firmware project ends up writing this: a motor limit, a PID gain, a mode, a serial number, each with a default, a valid range, a place in flash and a way for a service tool to change it. device-params is that layer, done once and tested hard. It is built on [DelegateMQ](https://github.com/DelegateMQ/DelegateMQ).

```cpp
inline constexpr param::Def kParams[] = {
    param::Int  (P::MaxRpm, "motor.max_rpm", 3000, 0, 6000, param::PERSIST).Units("rpm"),
    param::Float(P::PidKp,  "pid.kp",        1.2f, 0.0f, 10.0f, param::PERSIST),
};

param::ParamStore store(kParams, &flashBackend);
store.Init();                                    // defaults, then saved values

store.Set(P::MaxRpm, 4500);                      // typed, range-checked
auto conn = store.Subscribe(P::MaxRpm, [](int32_t rpm, param::Source) {
    motor.SetLimit(rpm);                         // runs on motorThread
}, &motorThread);
```

## Contents

- [Features](#features)
- [How It Compares](#how-it-compares)
- [Quick Start](#quick-start)
- [Examples](#examples)
- [Architecture](#architecture)
- [Building](#building)
- [Testing](#testing)
- [Project Layout](#project-layout)
- [Status and Roadmap](#status-and-roadmap)
- [License](#license)

## Features

| Feature | Description |
| :--- | :--- |
| **Declarative table** | Parameters are a `constexpr` table in flash: stable ID, name, type, default, min/max, units and flags. |
| **Compile-time typing** | `Key<T>` handles make `Get`/`Set` type-safe; a wrong type doesn't compile. Types: `bool`, `int32_t`, `uint32_t`, `float` and enums. |
| **Validation** | Range checks on every set, `READ_ONLY` for remote writers, and an optional validator for rules that span parameters. Rejections are reported, never silent. |
| **Change notification** | Subscribe to one parameter or all of them, synchronously or on a thread of your choice. Thread subscribers are coalesced, so a burst of changes can't overflow an RTOS queue. |
| **Persistence** | Manual, immediate or deferred (coalesced) saves, with automatic retry after a write failure. |
| **Pluggable storage** | NOR flash log, file, SQLite, RAM, or your own `IBackend`. |
| **Power-loss safe** | The flash backend survives power cuts during writes and compaction, and rejects corrupted records by CRC. |
| **Remote access** | `ParamService` answers list/get/set requests and publishes changes over the DelegateMQ DataBus. Parameters describe themselves, so tools need no per-project code. |
| **Embedded first** | No threads required (bare-metal poll mode), fixed-size storage for values and records, about 9 KB of code for the core on Cortex-M4. |
| **Stress tested** | Randomized chaos tests for power loss, bit rot, concurrency, flooding and teardown, also run under AddressSanitizer. |

## How It Compares

Parameter storage is usually either a raw key-value store or part of a larger platform:

| | device-params | Flash KV stores<br>(e.g. FlashDB) | Platform settings<br>(Zephyr settings, ESP-IDF NVS) | Autopilot params<br>(PX4, ArduPilot) |
| :--- | :---: | :---: | :---: | :---: |
| Typed values with min/max | ✅ | ❌ | ❌ | ✅ |
| Change notification on your thread | ✅ | ❌ | ❌ | Partial |
| Remote get/set, self-describing | ✅ any transport | ❌ | ❌ | ✅ MAVLink |
| Pluggable storage | ✅ | Own format | Own format | Own format |
| Portable across OS / bare metal | ✅ | ✅ | ❌ | ❌ |

FlashDB is an excellent flash store, and device-params can sit on top of one like it: storage is an interface.

## Quick Start

**1. Build and run the example and tests.**

```bash
git clone https://github.com/DelegateMQ/device-params.git
cd device-params
cmake -B build .
cmake --build build
./build/DeviceParamsApp          # Windows: build\Debug\DeviceParamsApp.exe
```

**2. Define your parameters.** IDs are stored in flash and on the wire, so never reuse or renumber one.

```cpp
enum class Mode : uint8_t { Manual, Auto, Service };

namespace P {
    inline constexpr param::Key<int32_t> MaxRpm   {1};
    inline constexpr param::Key<float>   PidKp    {2};
    inline constexpr param::Key<bool>    Telemetry{3};
    inline constexpr param::Key<Mode>    RunMode  {4};
    inline constexpr param::Key<int32_t> SerialNo {5};
}

inline constexpr param::Def kParams[] = {
    param::Int  (P::MaxRpm,    "motor.max_rpm", 3000, 0, 6000, param::PERSIST).Units("rpm"),
    param::Float(P::PidKp,     "pid.kp",        1.2f, 0.0f, 10.0f, param::PERSIST),
    param::Bool (P::Telemetry, "telemetry.on",  true),
    param::Enum (P::RunMode,   "run.mode",      Mode::Auto, Mode::Service, param::PERSIST),
    param::Int  (P::SerialNo,  "sys.serial",    0, 0, INT_MAX, param::PERSIST | param::READ_ONLY),
};
```

**3. Use them.**

```cpp
param::FileBackend backend("settings.params");
param::ParamStore store(kParams, &backend);
store.Init();                               // defaults, then saved values

int32_t rpm = store.Get(P::MaxRpm);         // typed: int32_t
if (store.Set(P::MaxRpm, 9000) == param::SetResult::OUT_OF_RANGE)
    std::printf("rejected\n");
store.Set(P::RunMode, Mode::Manual);
store.Commit();                             // write changes to the backend
```

Flags: `PERSIST` (saved to the backend), `READ_ONLY` (remote sets rejected), `REBOOT_REQUIRED` (informational, for tools) and `HIDDEN` (not visible remotely).

## Examples

### Change notification

```cpp
// One parameter, delivered on motorThread
auto conn = store.Subscribe(P::MaxRpm, [](int32_t rpm, param::Source) {
    motor.SetLimit(rpm);
}, &motorThread);

// Every parameter, delivered synchronously on the setting thread
auto any = store.SubscribeAny([](param::ParamId id, param::Value, param::Source src) {
    std::printf("param %u changed (source %d)\n", id, static_cast<int>(src));
});
```

Keep the returned `dmq::ScopedConnection`; letting it go out of scope unsubscribes. `Source` tells you who made the change: `Local`, `Remote`, `Load` or `Reset`.

A thread subscriber has at most one message queued on its thread and receives the **latest** value of each changed parameter, in order. Intermediate values of a fast-changing parameter may be skipped, which is what you want for state: a subscriber never falls behind, and a burst of changes can't overflow a small RTOS queue.

### Validation and errors

```cpp
// Rules that span parameters: return false to reject (SetResult::REJECTED)
dmq::UnicastDelegate<bool(param::ParamId, const param::Value&)> rule;
rule = dmq::MakeDelegate(std::function<bool(param::ParamId, const param::Value&)>(
    [&store](param::ParamId id, const param::Value& v) {
        // Telemetry must stay on in Service mode
        return !(id == P::Telemetry.id && !v.b && store.Get(P::RunMode) == Mode::Service);
    }));
store.SetValidator(rule);

// Know when a save fails (deferred saves retry automatically)
auto failed = store.OnCommitFailed.Connect(dmq::MakeDelegate(std::function<void()>([] {
    std::printf("settings not saved; retrying\n");
})));
```

`OnRejected` reports every rejected set, and every saved record rejected at load (bad CRC, wrong type, out of range). A rejected record keeps its default, so a firmware update that narrows a range can't load an invalid value.

### Persistence

| Save mode | Behavior |
| :--- | :--- |
| `Manual` | Only `Commit()` writes. |
| `Immediate` | Every change to a `PERSIST` parameter is written before `Set()` returns. |
| `Deferred` | Changes are coalesced and written once after a delay, which saves flash wear. Failed writes are retried. |

```cpp
store.SetSaveMode(param::SaveMode::Deferred, std::chrono::milliseconds(2000), &saveThread);
```

| Backend | Target | Notes |
| :--- | :--- | :--- |
| `FlashLogBackend` | Bare metal, RTOS | Append-only log over two NOR sectors with compaction. Power-loss safe; any program size from 1 to 32 bytes. |
| `FileBackend` | Desktop, embedded Linux | Atomic replace via a temp file. |
| `SqliteBackend` | Desktop, embedded Linux | One row per parameter; each commit is a transaction. |
| `RamBackend` | Tests | Includes fault-injection hooks. |

### Flash driver

`FlashLogBackend` needs only a small driver for two dedicated sectors of your part:

```cpp
class MyFlash : public param::IFlash {
public:
    size_t SectorSize() const override { return 4096; }
    size_t WriteSize() const override { return 8; }       // program granularity
    bool Read(size_t sector, size_t offset, void* buf, size_t len) override;
    bool Program(size_t sector, size_t offset, const void* buf, size_t len) override;
    bool EraseSector(size_t sector) override;
};

MyFlash flash;
param::FlashLogBackend backend(flash);
param::ParamStore store(kParams, &backend);
```

### Bare metal

With DelegateMQ's `DMQ_THREAD_NONE` there are no threads. Subscribers are called synchronously, and deferred saves run in **poll mode**: the timer only marks a save as due, and the main loop does the flash write. Flash writes never happen in an interrupt.

```cpp
store.SetSaveMode(param::SaveMode::Deferred, std::chrono::milliseconds(2000));

void SysTick_Handler() { dmq::util::Timer::ProcessTimers(); }

int main() {
    // ...
    for (;;) {
        store.Poll();                           // commits when a save is due
        // ...
    }
}
```

### Remote access

The device exposes its parameters; any tool can list, read and change them.

```cpp
// Device
param::ParamService service(store, "pump");
service.AllowRemoteSet(true);               // off by default
service.Start(&paramThread);                // give paramThread FullPolicy::DROP

// Tool
param::ParamClient client("pump");
client.Start();
auto c1 = client.OnDesc.Connect(dmq::MakeDelegate(std::function<void(const param::ParamDescMsg&)>(
    [](const param::ParamDescMsg& d) { std::printf("%u %s\n", d.id, d.name); })));

client.RequestList();                       // one description per parameter
client.RequestSet(P::MaxRpm.id, param::Value(int32_t(4500)));
```

Remote sets go through the same checks as local ones, with `Source::Remote`. Requests come from outside the device, so run the service on a thread with `FullPolicy::DROP`: a request flood then drops requests instead of faulting the device. Across processes or boards, register the topics on a DelegateMQ `NetworkNode` with `ExposeParamsOnNetwork(net, "pump", baseRid)` on the device and `AttachParamClientToNetwork(...)` on the tool.

| Topic | Direction | Payload |
| :--- | :--- | :--- |
| `<prefix>/param/list`, `get`, `set` | tool → device | Requests |
| `<prefix>/param/desc` | device → tool | One description per parameter (fits small frames) |
| `<prefix>/param/value` | device → tool | Get/set replies, with the request ID and current value |
| `<prefix>/param/changed` | device → tool | Every change |

## Architecture

```
 Application threads                          Tools (dmq-param, GUIs)
         │                                             │
         │ Get / Set / Subscribe                       │ ParamClient
         ▼                                             │
 ┌─────────────────────────────┐                       │
 │ ParamStore                  │                DataBus over any
 │  typed table · range checks │                DelegateMQ transport
 │  validator · change signals │                       │
 │  save modes · retry         │◄──── ParamService ◄───┘
 └──────────────┬──────────────┘   list / get / set, change notifications
                │ IBackend (records + CRC)
     ┌──────────┼──────────────┬───────────────┐
     ▼          ▼              ▼               ▼
 FlashLog     File          SQLite         your backend
 Backend      Backend       Backend
     │
 IFlash (your part's driver)
```

`ParamStore` has no templates in its core; `Get`/`Set`/`Subscribe` are thin typed wrappers. Backends store only records (ID, type, value, CRC) and know nothing about ranges or defaults, so a new storage medium is a small class.

## Building

**Requirements:** a C++17 compiler (MSVC, GCC or Clang) and CMake 3.10 or later. DelegateMQ is included under `delegate-mq/`; SQLite is optional under `third-party/sqlite/`.

| Option | Default | Effect |
| :--- | :--- | :--- |
| `PARAM_REMOTE` | `ON` | Build `ParamService`/`ParamClient` (enables the DelegateMQ DataBus) |
| `PARAM_BACKEND_SQLITE` | `ON` if `third-party/sqlite/sqlite3.c` exists | Build `SqliteBackend` |

```bash
cmake -B build -DPARAM_REMOTE=OFF -DPARAM_BACKEND_SQLITE=OFF .   # minimal build
```

Compile-time settings in `params/ParamConfig.h`:

| Macro | Default | Meaning |
| :--- | :--- | :--- |
| `PARAM_MAX_COUNT` | 64 | Maximum parameters per store |
| `PARAM_SAVE_DELAY_MS` | 1000 | Default deferred save delay |
| `PARAM_NO_NAMES` | off | Compile parameter names out to save flash; tools then use IDs |

**Using it in your project:** add `params/` (plus the backends you need) and DelegateMQ to your build, with the repository root and `delegate-mq/` on the include path. On an MCU the core is `params/ParamStore.cpp` plus a backend such as `params/backends/FlashLogBackend.cpp`, built with your DelegateMQ thread port or `DMQ_THREAD_NONE`.

## Testing

`DeviceParamsApp` runs the example, the unit tests and randomized chaos tests, and exits non-zero on any failure.

```bash
DeviceParamsApp                      # everything; chaos with a random seed
DeviceParamsApp --chaos-seed 1234    # replay a chaos run (the seed is printed)
DeviceParamsApp --chaos-scale 10     # longer chaos run
DeviceParamsApp --no-chaos           # skip chaos tests
```

The chaos tests check invariants under faults and load:

| Test | Invariant |
| :--- | :--- |
| Flash power loss | Random power cuts (including torn writes) during commits and compaction. After every reboot each parameter holds its last durable value or the one being written, and flash is never programmed illegally. |
| Flash bit rot | Random bit flips anywhere in flash never crash the loader or produce an out-of-range value, and the log stays writable. |
| Concurrent store | Setters, getters, resets, commits, subscribe/unsubscribe churn and deferred saves on many threads: no deadlock, subscribers converge on the final value, a reboot restores the final state. |
| Remote under load | Several clients flood a `FullPolicy::DROP` service: no request answered twice or to the wrong client, valid values only, normal service afterwards. |
| Remote teardown | Services and clients destroyed while requests and replies are still queued or in flight. |
| Flaky backend | Random write failures; once storage recovers, the pending save lands without further changes. |

CI builds and tests on Windows (MSVC) and Linux (GCC, Clang, and Clang with AddressSanitizer running a longer chaos pass), and compiles the core for Cortex-M4 bare metal.

## Project Layout

| Path | Contents |
| :--- | :--- |
| `params/` | The library: `Param.h`, `ParamConfig.h`, `ParamStore`, `Record.h`, `ParamMessages.h`, `ParamService`, `ParamClient` |
| `params/backends/` | `IBackend` and the flash log, file, SQLite and RAM backends |
| `examples/` | Example parameter table and usage |
| `unit-tests/` | Unit and chaos tests, run from `main.cpp` |
| `docs/design.md` | Design notes and background |
| `delegate-mq/` | Copy of DelegateMQ's `src/delegate-mq` |
| `third-party/sqlite/` | SQLite amalgamation (optional) |
| `tools/dmq-param/` | Planned command-line tool |

## Status and Roadmap

device-params is early and its API may still change. Working and tested today: `ParamStore`, all four backends, and `ParamService`/`ParamClient` in-process.

Next:

- `dmq-param` command-line tool (`list`, `get`, `set`, `dump`, `load`) for any device running `ParamService`
- Remote access tested across processes and boards
- String and array parameters
- Optional Cyphal register (`uavcan.register.*`) compatibility

## Related Projects

- [DelegateMQ](https://github.com/DelegateMQ/DelegateMQ): the messaging library device-params is built on
- [Async-SQLite](https://github.com/DelegateMQ/Async-SQLite): asynchronous SQLite wrapper on DelegateMQ
- [active-fsm](https://github.com/DelegateMQ/active-fsm): active-object state machines on DelegateMQ

## License

[MIT](LICENSE) © David Lafreniere
