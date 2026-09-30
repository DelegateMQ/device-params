# device-params

Typed device parameters for embedded C++: range checks, change notification,
flash/SQLite persistence and remote get/set. Built on
[DelegateMQ](https://github.com/DelegateMQ/DelegateMQ).

**Status:** early. Working and tested:

- `ParamStore`: typed get/set, range checks, validator, change notification
  (synchronous, or coalesced on a thread so a burst of changes can't overflow
  a subscriber's queue), Manual/Immediate/Deferred saves with automatic retry,
  and a `Poll()` mode for bare metal.
- Backends: RAM, file, NOR flash log (`FlashLogBackend`, fuzzed against
  simulated power loss and bit rot) and SQLite.
- `ParamService`/`ParamClient`: remote list/get/set and change notifications
  over the DataBus, tested in-process under load. Not yet tested across
  processes.
- The core (`ParamStore` + `FlashLogBackend`) compiles for Cortex-M4 bare
  metal: about 7.3 KB + 1.9 KB of code at `-Os`.

Not started: the `dmq-param` CLI.
See [docs/design.md](docs/design.md) for the design.

## Layout

| Path | Contents |
| :--- | :--- |
| `params/` | The library: `Param.h`, `ParamConfig.h`, `Record.h`, `ParamStore`, `ParamMessages.h`, `ParamService`, `ParamClient` |
| `params/backends/` | `IBackend` and the Ram / File / FlashLog / SQLite backends |
| `delegate-mq/` | Copy of DelegateMQ `src/delegate-mq` |
| `examples/` | Example parameter table and usage |
| `unit-tests/` | Tests run from `main.cpp` |
| `tools/dmq-param/` | Planned CLI |
| `third-party/sqlite/` | SQLite amalgamation (optional) |

## Build

```
cmake -B build .
cmake --build build
```

Options:

| Option | Default | Effect |
| :--- | :--- | :--- |
| `PARAM_REMOTE` | `ON` | Build `ParamService`/`ParamClient`; enables the DelegateMQ DataBus and `serialize` serializer |
| `PARAM_BACKEND_SQLITE` | `ON` if `third-party/sqlite/sqlite3.c` exists | Build `SqliteBackend` |

Compile-time tunables (`PARAM_MAX_COUNT`, `PARAM_SAVE_DELAY_MS`,
`PARAM_NO_NAMES`) are in `params/ParamConfig.h`.

## Tests

`DeviceParamsApp` runs the example, the unit tests and randomized chaos tests,
and exits non-zero on any failure.

```
DeviceParamsApp                      # everything; chaos with a random seed
DeviceParamsApp --chaos-seed 1234    # replay a chaos run (the seed is printed)
DeviceParamsApp --chaos-scale 10     # longer chaos run
DeviceParamsApp --no-chaos           # skip chaos tests
```

The chaos tests (`unit-tests/ChaosTests.cpp`) check invariants under faults
and load:

| Test | Invariant |
| :--- | :--- |
| Flash power loss | Random power cuts (including torn writes) during commits and compaction; after every reboot each parameter holds its last durable value or the one being written. No illegal flash programming. |
| Flash bit rot | Random bit flips anywhere in flash never crash the loader or produce an out-of-range value, and the log stays writable. |
| Concurrent store | Setters, getters, resets, commits, subscribe/unsubscribe churn and deferred saves on many threads: no deadlock (watchdog), subscribers converge on the final value, a reboot restores the final state. |
| Remote under load | Several clients flood a `FullPolicy::DROP` service thread: no request answered twice or to the wrong client, valid values only, normal service afterwards. |
| Remote teardown | Services and clients destroyed while requests and replies are still queued or in flight. |
| Flaky backend | Random write failures; once storage recovers, the pending deferred save lands without further changes. |

Run them under AddressSanitizer to catch lifetime bugs (MSVC:
`/fsanitize=address`; GCC/Clang: `-fsanitize=address`).

## Bare metal

With `DMQ_THREAD_NONE` there are no threads: subscriptions call back
synchronously, and Deferred saves use poll mode. The save timer only marks the
save due; call `store.Poll()` from the main loop to write it.

```cpp
store.SetSaveMode(param::SaveMode::Deferred, std::chrono::milliseconds(2000));
// SysTick / timer ISR:  dmq::util::Timer::ProcessTimers();
// main loop:            store.Poll();
```

## Updating DelegateMQ

`delegate-mq/` is a copy of `src/delegate-mq` from the DelegateMQ repo.
Replace the folder and commit as "Update DelegateMQ library".

## License

MIT
