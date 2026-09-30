// Compile check for the README examples (not run). Keep in sync with README.md.

#include "params/ParamStore.h"
#include "params/backends/FlashLogBackend.h"
#include "params/backends/FileBackend.h"
#ifdef PARAM_REMOTE
#include "params/ParamService.h"
#include "params/ParamClient.h"
#endif
#include <climits>
#include <cstdio>
#include <functional>

namespace readme {

// --- Define parameters ---
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

struct Motor { void SetLimit(int32_t) {} } motor;

void Basics()
{
    // --- Quick start ---
    param::FileBackend backend("settings.params");
    param::ParamStore store(kParams, &backend);
    store.Init();                               // defaults, then saved values

    int32_t rpm = store.Get(P::MaxRpm);         // typed: int32_t
    (void)rpm;
    if (store.Set(P::MaxRpm, 9000) == param::SetResult::OUT_OF_RANGE)
        std::printf("rejected\n");
    store.Set(P::RunMode, Mode::Manual);
    store.Commit();                             // write changes to the backend
}

void Subscribe(param::ParamStore& store, dmq::IThread& motorThread)
{
    // --- Change notification ---
    auto conn = store.Subscribe(P::MaxRpm, [](int32_t rpm, param::Source) {
        motor.SetLimit(rpm);                    // runs on motorThread
    }, &motorThread);

    auto any = store.SubscribeAny([](param::ParamId id, param::Value, param::Source src) {
        std::printf("param %u changed (source %d)\n", id, static_cast<int>(src));
    });
}

void Validation(param::ParamStore& store)
{
    // --- Validation ---
    dmq::UnicastDelegate<bool(param::ParamId, const param::Value&)> rule;
    rule = dmq::MakeDelegate(std::function<bool(param::ParamId, const param::Value&)>(
        [&store](param::ParamId id, const param::Value& v) {
            // Telemetry must stay on in Service mode
            return !(id == P::Telemetry.id && !v.b && store.Get(P::RunMode) == Mode::Service);
        }));
    store.SetValidator(rule);

    auto failed = store.OnCommitFailed.Connect(dmq::MakeDelegate(std::function<void()>([] {
        std::printf("settings not saved; retrying\n");
    })));
}

void Persistence(param::ParamStore& store, dmq::IThread& saveThread)
{
    // --- Deferred saves ---
    store.SetSaveMode(param::SaveMode::Deferred, std::chrono::milliseconds(2000), &saveThread);
}

// --- Flash driver ---
class MyFlash : public param::IFlash {
public:
    size_t SectorSize() const override { return 4096; }
    size_t WriteSize() const override { return 8; }       // program granularity
    bool Read(size_t, size_t, void*, size_t) override { return true; }
    bool Program(size_t, size_t, const void*, size_t) override { return true; }
    bool EraseSector(size_t) override { return true; }
};

void Flash()
{
    MyFlash flash;
    param::FlashLogBackend backend(flash);
    param::ParamStore store(kParams, &backend);
    store.Init();
}

#ifdef PARAM_REMOTE
void Remote(param::ParamStore& store, dmq::IThread& paramThread)
{
    // --- Device side ---
    param::ParamService service(store, "pump");
    service.AllowRemoteSet(true);               // off by default
    service.Start(&paramThread);                // a FullPolicy::DROP thread

    // --- Tool side ---
    param::ParamClient client("pump");
    client.Start();
    auto c1 = client.OnDesc.Connect(dmq::MakeDelegate(std::function<void(const param::ParamDescMsg&)>(
        [](const param::ParamDescMsg& d) { std::printf("%u %s\n", d.id, d.name); })));
    auto c2 = client.OnValue.Connect(dmq::MakeDelegate(std::function<void(const param::ParamValueMsg&)>(
        [](const param::ParamValueMsg& m) { std::printf("reply %u result %u\n", m.requestId, m.result); })));

    client.RequestList();
    client.RequestSet(P::MaxRpm.id, param::Value(int32_t(4500)));
}
#endif

} // namespace readme
