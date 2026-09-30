#include "BackendTests.h"
#include "SimFlash.h"
#include "examples/PumpParams.h"
#include "params/ParamStore.h"
#include "params/backends/FlashLogBackend.h"
#ifdef PARAM_BACKEND_SQLITE
#include "params/backends/SqliteBackend.h"
#endif
#include <cstdio>
#include <iostream>

using namespace param;

static int failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond "\n"; failures++; } } while (0)

// Load a fresh store from the flash, as after a reboot
static int32_t ReloadMaxRpm(SimFlash& flash)
{
    FlashLogBackend backend(flash);
    ParamStore store(kPumpParams, &backend);
    store.Init();
    return store.Get(P::MaxRpm);
}

static void TestFlashEmptyAndRoundTrip()
{
    for (size_t writeSize : { 1u, 4u, 8u, 16u, 32u }) {
        SimFlash flash(512, writeSize);
        {
            FlashLogBackend backend(flash);
            ParamStore store(kPumpParams, &backend);
            store.Init();                        // empty flash: defaults
            CHECK(store.Get(P::MaxRpm) == 3000);

            store.Set(P::MaxRpm, 4100);
            store.Set(P::PidKp, 2.5f);
            store.Set(P::RunMode, Mode::Manual);
            CHECK(store.Commit());
        }
        FlashLogBackend backend(flash);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        CHECK(store.Get(P::MaxRpm) == 4100);
        CHECK(store.Get(P::PidKp) == 2.5f);
        CHECK(store.Get(P::RunMode) == Mode::Manual);
        CHECK(flash.Violations() == 0);
    }
}

static void TestFlashCompaction()
{
    // 256-byte sectors, 16-byte slots: header + 15 records per sector
    SimFlash flash(256, 16);
    FlashLogBackend backend(flash);
    {
        ParamStore store(kPumpParams, &backend);
        store.Init();
        for (int32_t i = 1; i <= 100; i++) {
            store.Set(P::MaxRpm, i);
            if (i % 10 == 0)
                store.Set(P::PidKp, static_cast<float>(i) / 100.0f);
            CHECK(store.Commit());
        }
    }
    CHECK(backend.CompactionCount() >= 5);
    CHECK(flash.Violations() == 0);

    FlashLogBackend reloadBackend(flash);
    ParamStore store(kPumpParams, &reloadBackend);
    store.Init();
    CHECK(store.Get(P::MaxRpm) == 100);
    CHECK(store.Get(P::PidKp) == 1.0f);
}

static void TestFlashTornAppend()
{
    SimFlash flash(512, 1);
    {
        FlashLogBackend backend(flash);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.Set(P::MaxRpm, 1000);
        CHECK(store.Commit());

        flash.CutPowerAfter(1, true);            // next record is torn
        store.Set(P::MaxRpm, 2000);
        store.Commit();
    }
    flash.RestorePower();
    CHECK(ReloadMaxRpm(flash) == 1000);          // torn record rejected, previous kept

    // The log keeps working after the torn slot
    {
        FlashLogBackend backend(flash);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.Set(P::MaxRpm, 3000 - 1);
        CHECK(store.Commit());
    }
    CHECK(ReloadMaxRpm(flash) == 2999);
}

static void TestFlashPowerLossDuringCompaction()
{
    // Try cutting power at every program step of a compaction; the reloaded
    // value must always be either the old or the new one.
    for (int cut = 0; cut < 8; cut++) {
        SimFlash flash(64, 16);                  // header + 3 records per sector
        {
            FlashLogBackend backend(flash);
            ParamStore store(kPumpParams, &backend);
            store.Init();
            store.Set(P::MaxRpm, 100); store.Commit();
            store.Set(P::MaxRpm, 200); store.Commit();
            store.Set(P::MaxRpm, 300); store.Commit();   // active sector now full

            flash.CutPowerAfter(cut);
            store.Set(P::MaxRpm, 400);
            store.Commit();                               // needs compaction
        }
        flash.RestorePower();
        const int32_t v = ReloadMaxRpm(flash);
        CHECK(v == 300 || v == 400);
        if (cut >= 2)                                     // record + header programmed
            CHECK(v == 400);
    }
}

static void TestFlashErase()
{
    SimFlash flash(512, 4);
    FlashLogBackend backend(flash);
    {
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.Set(P::MaxRpm, 1234);
        CHECK(store.Commit());
    }
    CHECK(backend.Erase());
    CHECK(ReloadMaxRpm(flash) == 3000);
}

#ifdef PARAM_BACKEND_SQLITE
static void TestSqlite()
{
    const char* path = "device-params-test.db";
    std::remove(path);
    {
        SqliteBackend backend(path);
        CHECK(backend.IsOpen());
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.Set(P::MaxRpm, 4321);
        store.Set(P::PidKp, 6.5f);
        store.Set(P::RunMode, Mode::Service);
        CHECK(store.Commit());
        store.Set(P::MaxRpm, 4322);              // upsert
        CHECK(store.Commit());
    }
    {
        SqliteBackend backend(path);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        CHECK(store.Get(P::MaxRpm) == 4322);
        CHECK(store.Get(P::PidKp) == 6.5f);
        CHECK(store.Get(P::RunMode) == Mode::Service);
        CHECK(backend.Erase());
    }
    {
        SqliteBackend backend(path);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        CHECK(store.Get(P::MaxRpm) == 3000);
    }
    std::remove(path);
}
#endif

int RunBackendTests()
{
    failures = 0;
    TestFlashEmptyAndRoundTrip();
    TestFlashCompaction();
    TestFlashTornAppend();
    TestFlashPowerLossDuringCompaction();
    TestFlashErase();
#ifdef PARAM_BACKEND_SQLITE
    TestSqlite();
#endif
    std::cout << "BackendTests: " << (failures ? "FAILED" : "passed") << "\n";
    return failures;
}
