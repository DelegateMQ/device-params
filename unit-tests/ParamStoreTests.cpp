#include "ParamStoreTests.h"
#include "examples/PumpParams.h"
#include "params/ParamStore.h"
#include "params/Record.h"
#include "params/backends/RamBackend.h"
#include "params/backends/FileBackend.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <thread>

using namespace param;

// The definition table is constexpr; check it at compile time.
static_assert(kPumpParams[0].id == 1, "table order");
static_assert(kPumpParams[0].type == TypeTag::Int32, "MaxRpm type");
static_assert(kPumpParams[3].type == TypeTag::Enum, "RunMode type");

static int failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond "\n"; failures++; } } while (0)

// Poll until pred is true or timeout, servicing timers meanwhile.
static bool WaitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
{
    auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        dmq::util::Timer::ProcessTimers();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

static void TestDefaults()
{
    ParamStore store(kPumpParams);
    store.Init();

    CHECK(store.Count() == 5);
    CHECK(store.Get(P::MaxRpm) == 3000);
    CHECK(store.Get(P::PidKp) == 1.2f);
    CHECK(store.Get(P::Telemetry) == true);
    CHECK(store.Get(P::RunMode) == Mode::Auto);
    CHECK(store.Find(P::PidKp.id) != nullptr);
    CHECK(store.Find(static_cast<ParamId>(999)) == nullptr);
    CHECK(store.Find("pid.kp") == store.Find(P::PidKp.id));
    CHECK(store.Find("nope") == nullptr);
}

static void TestSetAndRanges()
{
    ParamStore store(kPumpParams);
    store.Init();

    int rejected = 0;
    SetResult lastReject = SetResult::OK;
    auto conn = store.OnRejected.Connect(dmq::MakeDelegate(
        std::function<void(ParamId, SetResult, Source)>([&](ParamId, SetResult r, Source) { rejected++; lastReject = r; })));

    CHECK(store.Set(P::MaxRpm, 4500) == SetResult::OK);
    CHECK(store.Get(P::MaxRpm) == 4500);

    CHECK(store.Set(P::MaxRpm, 7000) == SetResult::OUT_OF_RANGE);
    CHECK(store.Get(P::MaxRpm) == 4500);
    CHECK(rejected == 1 && lastReject == SetResult::OUT_OF_RANGE);

    CHECK(store.Set(P::PidKp, std::nanf("")) == SetResult::OUT_OF_RANGE);
    CHECK(store.Set(P::RunMode, Mode::Service) == SetResult::OK);
    CHECK(store.Get(P::RunMode) == Mode::Service);

    // READ_ONLY blocks remote sets only
    CHECK(store.Set(P::SerialNo, 42, Source::Remote) == SetResult::READ_ONLY);
    CHECK(store.Set(P::SerialNo, 42) == SetResult::OK);

    // Untyped path
    CHECK(store.SetValue(999, Value(int32_t(1)), Source::Remote) == SetResult::UNKNOWN_ID);
    CHECK(store.SetValue(P::MaxRpm.id, Value(1.0f), Source::Remote) == SetResult::TYPE_MISMATCH);
    CHECK(store.GetValue(P::MaxRpm.id).i == 4500);
}

static void TestValidator()
{
    ParamStore store(kPumpParams);
    store.Init();

    // Rule: telemetry can't be turned off while in Auto mode (uses Get under the lock)
    dmq::UnicastDelegate<bool(ParamId, const Value&)> v;
    v = dmq::MakeDelegate(std::function<bool(ParamId, const Value&)>([&](ParamId id, const Value& val) {
        return !(id == P::Telemetry.id && !val.b && store.Get(P::RunMode) == Mode::Auto);
    }));
    store.SetValidator(v);

    CHECK(store.Set(P::Telemetry, false) == SetResult::REJECTED);
    CHECK(store.Set(P::RunMode, Mode::Manual) == SetResult::OK);
    CHECK(store.Set(P::Telemetry, false) == SetResult::OK);
}

static void TestNotifySync()
{
    ParamStore store(kPumpParams);
    store.Init();

    int rpmCalls = 0, anyCalls = 0;
    int32_t lastRpm = 0;
    Source lastSrc = Source::Load;
    auto c1 = store.Subscribe(P::MaxRpm, [&](int32_t rpm, Source s) { rpmCalls++; lastRpm = rpm; lastSrc = s; });
    auto c2 = store.SubscribeAny([&](ParamId, Value, Source) { anyCalls++; });

    store.Set(P::MaxRpm, 1000, Source::Remote);
    store.Set(P::MaxRpm, 1000);            // same value: no notification
    store.Set(P::PidKp, 2.0f);             // other key: only SubscribeAny
    store.Set(P::MaxRpm, 9999);            // rejected: no notification

    CHECK(rpmCalls == 1 && lastRpm == 1000 && lastSrc == Source::Remote);
    CHECK(anyCalls == 2);

    c1.Disconnect();
    store.Set(P::MaxRpm, 2000);
    CHECK(rpmCalls == 1);
}

static void TestNotifyAsync()
{
    ParamStore store(kPumpParams);
    store.Init();

    dmq::os::Thread worker("ParamTestWorker");
    worker.CreateThread();

    std::atomic<int> calls{ 0 };
    std::atomic<bool> onWorker{ false };
    auto conn = store.Subscribe(P::MaxRpm, [&](int32_t rpm, Source) {
        onWorker = worker.IsCurrentThread();
        if (rpm == 1234) calls++;
    }, &worker);

    store.Set(P::PidKp, 3.0f);     // filtered before dispatch
    store.Set(P::MaxRpm, 1234);
    CHECK(WaitFor([&] { return calls.load() == 1; }));
    CHECK(onWorker.load());

    worker.ExitThread();
}

static void TestManualPersistAndReload()
{
    RamBackend backend;
    {
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.Set(P::MaxRpm, 5000);
        store.Set(P::Telemetry, false);     // not PERSIST
        store.Set(P::RunMode, Mode::Manual);
        CHECK(store.HasUnsavedChanges());
        CHECK(backend.RecordCount() == 0);   // Manual: nothing written yet
        CHECK(store.Commit());
        CHECK(!store.HasUnsavedChanges());
        CHECK(backend.RecordCount() == 2);
    }

    ParamStore reloaded(kPumpParams, &backend);
    reloaded.Init();
    CHECK(reloaded.Get(P::MaxRpm) == 5000);
    CHECK(reloaded.Get(P::RunMode) == Mode::Manual);
    CHECK(reloaded.Get(P::Telemetry) == true);  // default, was never persisted
    CHECK(!reloaded.HasUnsavedChanges());
}

static void TestImmediate()
{
    RamBackend backend;
    ParamStore store(kPumpParams, &backend);
    store.Init();
    store.SetSaveMode(SaveMode::Immediate);

    store.Set(P::MaxRpm, 100);
    store.Set(P::MaxRpm, 200);
    store.Set(P::Telemetry, false);          // not PERSIST: no write
    CHECK(backend.WriteCount() == 2);
    CHECK(!store.HasUnsavedChanges());
}

static void TestDeferred()
{
    RamBackend backend;
    dmq::os::Thread saveThread("ParamSaveThread");
    saveThread.CreateThread();
    {
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.SetSaveMode(SaveMode::Deferred, std::chrono::milliseconds(50), &saveThread);

        store.Set(P::MaxRpm, 100);
        store.Set(P::MaxRpm, 200);
        store.Set(P::PidKp, 0.5f);
        CHECK(backend.WriteCount() == 0);    // coalesced, not written yet

        CHECK(WaitFor([&] { return backend.WriteCount() == 1; }));
        CHECK(backend.RecordCount() == 2);
        CHECK(!store.HasUnsavedChanges());

        // Unsaved change at destruction is flushed by the destructor
        store.Set(P::MaxRpm, 300);
    }
    CHECK(backend.WriteCount() == 2);

    ParamStore reloaded(kPumpParams, &backend);
    reloaded.Init();
    CHECK(reloaded.Get(P::MaxRpm) == 300);
    saveThread.ExitThread();
}

static void TestCommitFailure()
{
    RamBackend backend;
    ParamStore store(kPumpParams, &backend);
    store.Init();

    store.Set(P::MaxRpm, 1111);
    backend.FailWrites(true);
    CHECK(!store.Commit());
    CHECK(store.HasUnsavedChanges());       // kept for retry

    backend.FailWrites(false);
    CHECK(store.Commit());
    CHECK(!store.HasUnsavedChanges());
    CHECK(backend.RecordCount() == 1);
}

static void TestLoadRejections()
{
    RamBackend backend;
    backend.Inject(EncodeRecord(P::MaxRpm.id, Value(int32_t(2500))));      // good
    backend.Inject(EncodeRecord(P::PidKp.id, Value(int32_t(5))));          // wrong type
    backend.Inject(EncodeRecord(P::SerialNo.id, Value(int32_t(-1))));      // out of range
    backend.Inject(EncodeRecord(P::RunMode.id, Value::FromEnum(Mode::Manual)));
    backend.Corrupt(P::RunMode.id);                                        // bad CRC
    backend.Inject(EncodeRecord(500, Value(int32_t(1))));                  // removed param

    ParamStore store(kPumpParams, &backend);
    int mismatch = 0, range = 0, bad = 0;
    auto conn = store.OnRejected.Connect(dmq::MakeDelegate(
        std::function<void(ParamId, SetResult, Source)>([&](ParamId, SetResult r, Source s) {
            if (s != Source::Load) return;
            if (r == SetResult::TYPE_MISMATCH) mismatch++;
            if (r == SetResult::OUT_OF_RANGE) range++;
            if (r == SetResult::REJECTED) bad++;
        })));
    store.Init();

    CHECK(store.Get(P::MaxRpm) == 2500);
    CHECK(store.Get(P::PidKp) == 1.2f);
    CHECK(store.Get(P::SerialNo) == 0);
    CHECK(store.Get(P::RunMode) == Mode::Auto);
    CHECK(mismatch == 1 && range == 1 && bad == 1);
}

static void TestResetToDefaults()
{
    ParamStore store(kPumpParams);
    store.Init();
    store.Set(P::MaxRpm, 10);
    store.Set(P::Telemetry, false);

    int resets = 0;
    auto conn = store.SubscribeAny([&](ParamId, Value, Source s) { if (s == Source::Reset) resets++; });

    store.ResetToDefaults();                 // PERSIST only
    CHECK(store.Get(P::MaxRpm) == 3000);
    CHECK(store.Get(P::Telemetry) == false);
    CHECK(resets == 1);

    store.ResetToDefaults(ALL);
    CHECK(store.Get(P::Telemetry) == true);
}

static void TestRecordCodec()
{
    const Value values[] = { Value(true), Value(int32_t(-7)), Value(uint32_t(0xDEADBEEF)), Value(3.25f), Value::FromEnum(Mode::Service) };
    for (const Value& v : values) {
        Record rec = EncodeRecord(9, v);
        uint8_t buf[PACKED_RECORD_SIZE];
        PackRecord(rec, buf);
        Value out;
        CHECK(DecodeRecord(UnpackRecord(buf), out));
        CHECK(out.type == v.type);
        CHECK(EncodeRecord(9, out).crc == rec.crc);
    }
}

static void TestFileBackend()
{
    const std::string path = "device-params-test.params";
    std::remove(path.c_str());
    {
        FileBackend backend(path);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.Set(P::MaxRpm, 4321);
        store.Set(P::PidKp, 6.5f);
        CHECK(store.Commit());
        store.Set(P::MaxRpm, 4322);          // update existing record
        CHECK(store.Commit());
    }
    {
        FileBackend backend(path);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        CHECK(store.Get(P::MaxRpm) == 4322);
        CHECK(store.Get(P::PidKp) == 6.5f);
    }
    std::remove(path.c_str());
}

int RunParamStoreTests()
{
    failures = 0;

    TestDefaults();
    TestSetAndRanges();
    TestValidator();
    TestNotifySync();
    TestNotifyAsync();
    TestManualPersistAndReload();
    TestImmediate();
    TestDeferred();
    TestCommitFailure();
    TestLoadRejections();
    TestResetToDefaults();
    TestRecordCodec();
    TestFileBackend();

    std::cout << "ParamStoreTests: " << (failures ? "FAILED" : "passed") << "\n";
    return failures;
}
