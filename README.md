# device-params

Typed device parameters for embedded C++: range checks, change notification,
flash/SQLite persistence and remote get/set. Built on
[DelegateMQ](https://github.com/DelegateMQ/DelegateMQ).

**Status:** early. `ParamStore` (typed get/set, range checks, validator,
change notification, Manual/Immediate/Deferred saves) and the RAM and file
backends work and are tested. `ParamService`/`ParamClient` (remote
list/get/set and change notifications over the DataBus) work and are tested
in-process; the network helpers are not yet tested across processes.
`FlashLogBackend`, `SqliteBackend` and `dmq-param` are still stubs.
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
| `PARAM_BACKEND_SQLITE` | `OFF` | Build `SqliteBackend` (needs `third-party/sqlite`) |

Compile-time tunables (`PARAM_MAX_COUNT`, `PARAM_SAVE_DELAY_MS`,
`PARAM_NO_NAMES`) are in `params/ParamConfig.h`.

## Updating DelegateMQ

`delegate-mq/` is a copy of `src/delegate-mq` from the DelegateMQ repo.
Replace the folder and commit as "Update DelegateMQ library".

## License

MIT
