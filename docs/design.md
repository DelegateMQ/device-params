# device-params

Design notes for a standalone parameter store built on DelegateMQ. Working
repo name: `device-params`.

## Pitch

Typed, named device parameters with range checks, defaults and change
notification, persisted to flash (or a file / SQLite on Linux), and readable and
writable remotely over any transport. Every firmware project reinvents this;
PX4 and ArduPilot each built their own. DelegateMQ provides the parts that are
hard to do well: change callbacks delivered on the subscriber's thread, remote
access over any transport, and heap-free storage.

It should be useful to someone who has never heard of DelegateMQ. DMQ is the
implementation, not the pitch.

## Landscape

GitHub search, September 2026. No established standalone library combines
typed parameters, change notification, persistence and remote access.

### Flash key-value storage (no parameter semantics)

| Repo | Stars | Notes |
| :--- | :--- | :--- |
| [armink/FlashDB](https://github.com/armink/FlashDB) | ~2.9k | Most popular. KV and time-series on flash, wear leveling, power-loss safety. Untyped bytes: no min/max, no notification, no remote access. |
| [armink/EasyFlash](https://github.com/armink/EasyFlash) | ~2.4k | FlashDB's predecessor. |

### Parameter libraries (closest matches, all tiny)

| Repo | Stars | Notes |
| :--- | :--- | :--- |
| [PonomarevDA/libparams](https://github.com/PonomarevDA/libparams) | 7 | Closest match. C99, STM32 plus Linux simulator. int32 / float / bool / 56-byte string, each with default, min/max and flags (mutable, required). O(1) access by index, O(n) by name for remote use. Backing store for the Cyphal/DroneCAN register interface. No notification or threading; persistence and visibility flags not implemented yet (per its README). |
| [dowhile98/microconf](https://github.com/dowhile98/microconf) | 0 | C99, schema-driven, CRC32, typed accessors, no heap, two-slot storage. Local config only. |
| [aikoschurmann/cfgsafe](https://github.com/aikoschurmann/cfgsafe) | 4 | C, little documentation. |
| [janscience/MicroConfig](https://github.com/janscience/MicroConfig) | 0 | Arduino/Teensy serial configuration menus, typed values in EEPROM. |
| [joaomelga/JsonParameters](https://github.com/joaomelga/JsonParameters) | 0 | JSON-backed parameters for embedded C++; inactive since 2022. |
| [107-systems/107-Arduino-Cyphal-Support](https://github.com/107-systems/107-Arduino-Cyphal-Support) | 0 | Cyphal register storage on Arduino. |

### Platform-bound systems

PX4 parameters, ArduPilot `AP_Param`, Zephyr settings subsystem, ESP-IDF NVS.
Mature, but tied to their platform; Zephyr settings and NVS are untyped
key-value storage without range metadata or cross-thread notification.

### What's common, and the gap

Everyone converges on the same core: typed values with defaults and min/max,
flags (read-only, reset-exempt), numeric IDs for fast access and names for
tools, and a HAL under the flash layer. libparams even reaches the same remote
model through Cyphal's register protocol.

None of them has:

1. Change notification delivered on the subscriber's thread.
2. Remote get/set over any transport with self-describing parameters.
3. A C++ API with compile-time typed keys (all are C with index/enum access,
   except the Arduino one).
4. Pluggable backends (flash log, LittleFS, FlashDB, SQLite).

### Takeaways

- **Don't write a new flash layer.** A `FlashDbBackend` reuses FlashDB's wear
  leveling and power-loss handling.
- **Consider Cyphal compatibility.** Mapping `ParamService` onto
  `uavcan.register.Access` / `List`
  ([DSDL](https://github.com/OpenCyphal/public_regulated_data_types/blob/master/uavcan/register/384.Access.1.0.dsdl))
  gives the project an existing audience.

## API Sketch

The sketch below is the original design. The implemented API in `params/`
differs in small ways: units are set with `.Units("rpm")`, `SetSaveMode` takes
the save thread, `Commit()` returns `bool`, and `HasUnsavedChanges()`, `ALL`,
`HIDDEN` and `REMOTE_DISABLED` were added.

### Parameter definitions (application side)

Defined once in a `constexpr` table in flash. IDs are stable 16-bit numbers,
because flash records and wire messages use them. Names are for tools and can be
compiled out.

```cpp
// app_params.h
enum class Mode : uint8_t { Manual, Auto, Service };

namespace P {
    inline constexpr param::Key<int32_t> MaxRpm   {1};
    inline constexpr param::Key<float>   PidKp    {2};
    inline constexpr param::Key<bool>    Telemetry{3};
    inline constexpr param::Key<Mode>    RunMode  {4};
    inline constexpr param::Key<int32_t> SerialNo {5};
}

inline constexpr param::Def kParams[] = {
    //            key          name             default     min   max    flags
    param::Int  (P::MaxRpm,    "motor.max_rpm", 3000,       0,    6000,  param::PERSIST | param::UNITS("rpm")),
    param::Float(P::PidKp,     "pid.kp",        1.2f,       0.0f, 10.0f, param::PERSIST),
    param::Bool (P::Telemetry, "telemetry.on",  true),
    param::Enum (P::RunMode,   "run.mode",      Mode::Auto, Mode::Service, param::PERSIST | param::REBOOT_REQUIRED),
    param::Int  (P::SerialNo,  "sys.serial",    0,          0,    INT32_MAX, param::PERSIST | param::READ_ONLY),
};
```

A `Def` holds a type tag, default, min/max, flags, name and units. Values live
in a small tagged union (`param::Value`: bool, int32, uint32, float, enum), so
the store itself has no templates. `Key<T>` gives compile-time typing; `Init()`
asserts each key's `T` matches its table entry.

### Store

```cpp
namespace param {

enum class SetResult : uint8_t { OK, UNKNOWN_ID, TYPE_MISMATCH, OUT_OF_RANGE, READ_ONLY, REJECTED };
enum class Source    : uint8_t { Local, Remote, Load, Reset };
enum class SaveMode  : uint8_t { Manual, Immediate, Deferred };   // Deferred: coalesce writes (flash wear)

class ParamStore {
public:
    template <size_t N>
    ParamStore(const Def (&defs)[N], IBackend* backend = nullptr);

    void Init();                              // defaults first, then overlay persisted records
    void SetSaveMode(SaveMode mode, dmq::Duration delay = dmq::PARAM_SAVE_DELAY);

    // Typed access: thin inline wrappers over the untyped calls below
    template <class T> T         Get(Key<T> key) const;
    template <class T> SetResult Set(Key<T> key, T value, Source src = Source::Local);

    // Untyped access, used by the remote service and tools
    SetResult   SetValue(ParamId id, const Value& v, Source src);
    Value       GetValue(ParamId id) const;
    const Def*  Find(ParamId id) const;
    const Def*  Find(const char* name) const;
    size_t      Count() const;
    const Def&  DefAt(size_t index) const;    // iterate for listings

    // Change notification; callback runs on `thread` (or synchronously if nullptr)
    template <class T, class F>
    [[nodiscard]] dmq::ScopedConnection Subscribe(Key<T> key, F&& fn, dmq::IThread* thread = nullptr);
    //   fn(T newValue, Source src)

    template <class F>
    [[nodiscard]] dmq::ScopedConnection SubscribeAny(F&& fn, dmq::IThread* thread = nullptr);
    //   fn(ParamId id, const Value& newValue, Source src)

    // Optional veto for rules spanning several parameters (e.g. min_rpm < max_rpm).
    // Runs under the store lock on the caller's thread; must not block.
    void SetValidator(dmq::UnicastDelegate<bool(ParamId, const Value&)> v);

    void Commit();                            // flush pending persistent changes now
    void ResetToDefaults(uint32_t flagMask = PERSIST);

    dmq::Signal<void(ParamId, SetResult, Source)> OnRejected;  // audit trail for remote sets
};

} // namespace param
```

Threading:

- `Get`/`Set` take a `dmq::Mutex`, copy at most 8 bytes, and release.
- Change signals fire after the lock is released, so a subscriber can call
  `Set` without deadlocking.
- `Get` is not ISR-safe. If needed later, scalar parameters could get a
  `CriticalSection` snapshot path.
- Typed templates stay thin wrappers over non-templated code, per the
  DelegateMQ code-size pattern.

### Usage

```cpp
param::ParamStore store(kParams, &flashBackend);
store.SetSaveMode(param::SaveMode::Deferred);
store.Init();

auto conn = store.Subscribe(P::MaxRpm, [](int32_t rpm, param::Source) {
    motor.SetLimit(rpm);                 // runs on motorThread
}, &motorThread);

if (store.Set(P::PidKp, 2.5f) != param::SetResult::OK) { /* ... */ }
```

## Persistence

```cpp
struct Record { ParamId id; TypeTag type; uint8_t len; uint8_t data[8]; uint16_t crc; };

class IBackend {
public:
    virtual ~IBackend() = default;
    virtual bool ReadAll(dmq::UnicastDelegate<void(const Record&)> sink) = 0;
    virtual bool Write(const Record* recs, size_t count) = 0;   // one batch per Commit()
    virtual bool Erase() = 0;
};
```

Loading rules (survive firmware updates):

- Record ID not in the table: ignored.
- Wrong type or out of range: default used, reported.
- Bad CRC: skipped.

Records can use `crc16.h` from `extras/util`.

### Backends

| Backend | Target | Notes |
| :--- | :--- | :--- |
| `RamBackend` | tests | |
| `FileBackend` | desktop | |
| `FlashLogBackend` | bare metal / RTOS | Append-only records across two sectors, compacted when full. The one that matters most on MCUs. |
| `FlashDbBackend` | bare metal / RTOS | Reuse FlashDB instead of writing a flash layer. |
| `LittleFsBackend` | RTOS | |
| `SqliteBackend` | desktop / embedded Linux | See below. Not for MCUs (VFS, a few hundred KB of flash/RAM). |

### SQLite backend

```sql
CREATE TABLE params (
    id         INTEGER PRIMARY KEY,   -- stable ParamId; key on this, not the name
    name       TEXT,                  -- informational only; renames don't orphan rows
    type       INTEGER NOT NULL,
    value      BLOB NOT NULL,
    updated_at INTEGER NOT NULL
);
```

- `Write(recs, count)`: one transaction with `INSERT ... ON CONFLICT(id) DO
  UPDATE` per record, so each `Commit()` is atomic. No record CRC needed.
- `ReadAll`: `SELECT *`, same loading rules.
- Extras a flash log can't easily do:
  - `param_history` table (id, old, new, `Source`, timestamp) as an audit
    trail. Regulated devices need this (possibly Cellutron).
  - Named profiles (factory / site / user).
  - Inspect or edit settings with any SQLite tool.
- Blocking: in `Deferred` mode commits already run on the store's thread, so a
  synchronous call is fine. For `Immediate`, run it on Async-SQLite's worker
  thread, which also links the two repos.

## Remote Access

Implemented in `ParamMessages.h`, `ParamService` (device) and `ParamClient`
(tool).

```cpp
param::ParamService svc(store, "pump");   // topic prefix
svc.AllowRemoteSet(true);                 // off by default
svc.Start(&paramThread);                  // nullptr = handle on the publisher's thread

// Over a network, on the node's NetworkNode:
param::ExposeParamsOnNetwork(net, "pump", /*baseRid=*/100);
```

| Topic | Type | Direction |
| :--- | :--- | :--- |
| `pump/param/list` | `ParamListReq` | tool → device |
| `pump/param/get` | `ParamGetReq` | tool → device |
| `pump/param/set` | `ParamSetReq` | tool → device |
| `pump/param/desc` | `ParamDescMsg` | device → tool, one per listed parameter (index/count) |
| `pump/param/value` | `ParamValueMsg` | device → tool, get/set reply |
| `pump/param/changed` | `ParamValueMsg` | device → tool, every change (requestId 0) |

- One `desc` message per parameter (MAVLink style) instead of one large list,
  so each message fits small transport frames (serial).
- Values travel as a type tag plus 32 raw bits (`ToBits`/`FromBits`).
- Replies carry the client's `requestId` and the current value, successful or
  not.
- Remote set disabled → `REMOTE_DISABLED`. `HIDDEN` parameters are not listed;
  get/set on them → `UNKNOWN_ID`, and their changes are not published.
- Remote sets go through the store with `Source::Remote`, so `READ_ONLY`,
  range and validator checks apply.
- Six remote IDs from an application-chosen base (`ParamRid`).
  `ExposeParamsOnNetwork` / `AttachParamClientToNetwork` register them on a
  `NetworkNode`.
- `list` returns full descriptors, so the parameter set describes itself:
  - A `dmq-param` CLI (`list` / `get` / `set` / `dump` / `load`) built on
    `ParamClient` works against any node with no per-project code.
  - Registering these topics with `JsonTopics` exposes parameters to MQTT and
    Home Assistant.
- Optional later: a Cyphal `uavcan.register.Access` / `List` mapping (see
  Landscape).

## Naming

"parameter-store" on GitHub is dominated by AWS SSM Parameter Store (17 of 20
repos on the [topic page](https://github.com/topics/parameter-store)), so avoid
`param-store` / `parameter-store`.

| Name | Take |
| :--- | :--- |
| **`device-params`** | Clearly devices, not cloud. Recommended. |
| `firmware-params` | Clearer still, but sounds MCU-only, and SQLite makes it useful on Linux. |
| `tunables` | Distinctive, no AWS collision; undersells persistence. |
| `settings-store` | Friendlier to app developers; sounds less embedded. |
| `dmq-params` | Clear in the ecosystem; limits reach outside it. |

GitHub topics: `embedded`, `firmware`, `rtos`, `configuration`, `cyphal`,
not `parameter-store`.

## Open Questions

- **String and array parameters** (Wi-Fi SSID, calibration tables). The fixed
  8-byte value slot would need a separate fixed-capacity pool.
- **Atomic multi-set** (`SetMany`). Without it, remote tools can briefly create
  min > max between two single sets.
- **Name storage.** Names can take a lot of flash on small MCUs.
  `PARAM_NO_NAMES` would drop them; tools then use IDs only.
- **Separate repo or `extras/param/`.** A separate repo reaches more people;
  `extras/` can use the DataBus without a dependency dance.
- **Cyphal compatibility.** First-class goal or later add-on.
