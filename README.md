# device-params

Typed device parameters for embedded C++: range checks, change notification,
flash/SQLite persistence and remote get/set. Built on
[DelegateMQ](https://github.com/DelegateMQ/DelegateMQ).

**Status:** early. Working and tested:

- `ParamStore`: typed get/set, range checks, validator, change notification
  (synchronous or on a thread), Manual/Immediate/Deferred saves, with a
  `Poll()` mode for bare metal.
- Backends: RAM, file, NOR flash log (`FlashLogBackend`, tested against a
  simulated flash including power loss) and SQLite.
- `ParamService`/`ParamClient`: remote list/get/set and change notifications
  over the DataBus, tested in-process. Not yet tested across processes.
- The core (`ParamStore` + `FlashLogBackend`) compiles for Cortex-M4 bare
  metal: about 6.6 KB + 1.9 KB of code at `-Os`.

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
