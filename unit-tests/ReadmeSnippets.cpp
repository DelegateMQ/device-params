// Compile check for the README examples (not run). Keep in sync with README.md.

#include "params/ParamStore.h"
#include "params/backends/FlashLogBackend.h"
#include "params/backends/FileBackend.h"
#ifdef PARAM_REMOTE
#include "params/ParamService.h"
#include "params/ParamClient.h"
#endif

namespace readme {

// --- Quick Start: define parameters ---
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

struct Motor { void SetLimit(int32_t) {} } motor;

void QuickStart()
{
    param::FileBackend backend("settings.params");
    param::ParamStore store(kParams, &backend);
    store.Init();                               // defaults, then saved values

    int32_t rpm = store.Get(P::MaxRpm);         // typed
    (void)rpm;
    store.Set(P::MaxRpm, 4500);                 // returns SetResult::OK
    store.Set(P::MaxRpm, 9000);                 // returns SetResult::OUT_OF_RANGE
    store.Commit();                             // save to the backend
}

void Notification(param::ParamStore& store, dmq::IThread& motorThread)
{
    auto conn = store.Subscribe(P::MaxRpm, [](int32_t rpm, param::Source) {
        motor.SetLimit(rpm);                    // runs on motorThread
    }, &motorThread);
}

void Persistence(param::ParamStore& store, dmq::IThread& saveThread)
{
    store.SetSaveMode(param::SaveMode::Deferred, std::chrono::milliseconds(2000), &saveThread);
}

class MyFlash : public param::IFlash {
public:
    size_t SectorSize() const override { return 4096; }
    size_t WriteSize() const override { return 8; }
    bool Read(size_t, size_t, void*, size_t) override { return true; }
    bool Program(size_t, size_t, const void*, size_t) override { return true; }
    bool EraseSector(size_t) override { return true; }
};

void Flash()
{
    MyFlash flash;
    param::FlashLogBackend backend(flash);
    (void)backend;
}

void BareMetal(param::ParamStore& store)
{
    store.SetSaveMode(param::SaveMode::Deferred, std::chrono::milliseconds(2000));
    dmq::util::Timer::ProcessTimers();          // SysTick_Handler
    store.Poll();                               // main loop: saves when due
}

#ifdef PARAM_REMOTE
void Remote(param::ParamStore& store, dmq::IThread& paramThread)
{
    // Device
    param::ParamService service(store, "pump");
    service.AllowRemoteSet(true);               // off by default
    service.Start(&paramThread);

    // Tool
    param::ParamClient client("pump");
    client.Start();
    client.RequestList();
    client.RequestSet(P::MaxRpm.id, param::Value(int32_t(4500)));
}
#endif

} // namespace readme
