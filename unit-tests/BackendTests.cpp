#include "BackendTests.h"
#include "SimFlash.h"
#include "examples/PumpParams.h"
#include "params/ParamStore.h"
#include "params/Record.h"
#include "params/backends/FlashLogBackend.h"
#include "params/backends/FileBackend.h"
#include "params/backends/RamBackend.h"
#ifdef PARAM_BACKEND_SQLITE
#include "params/backends/SqliteBackend.h"
#endif
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>
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

// -----------------------------------------------------------------------------
// FileBackend
// -----------------------------------------------------------------------------

namespace {

const std::string kFile = "device-params-filetest.params";
const std::string kTmp = kFile + ".tmp";

void RemoveTestFiles()
{
    std::remove(kFile.c_str());
    std::remove(kTmp.c_str());
}

bool FileExists(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

/// Bytes of a packed record file holding `recs`.
std::vector<uint8_t> Packed(std::initializer_list<Record> recs)
{
    std::vector<uint8_t> out;
    uint8_t buf[PACKED_RECORD_SIZE];
    for (const Record& r : recs) {
        PackRecord(r, buf);
        out.insert(out.end(), buf, buf + sizeof(buf));
    }
    return out;
}

void WriteBytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!bytes.empty())
        std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

int32_t LoadMaxRpm()
{
    FileBackend backend(kFile);
    ParamStore store(kPumpParams, &backend);
    store.Init();
    return store.Get(P::MaxRpm);
}

Record Rpm(int32_t v) { return EncodeRecord(P::MaxRpm.id, Value(v)); }
Record Kp(float v)    { return EncodeRecord(P::PidKp.id, Value(v)); }

} // namespace

static void TestFileRoundTrip()
{
    RemoveTestFiles();
    CHECK(LoadMaxRpm() == 3000);                 // no file: defaults
    {
        FileBackend backend(kFile);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.Set(P::MaxRpm, 4321);
        store.Set(P::PidKp, 6.5f);
        CHECK(store.Commit());
        store.Set(P::MaxRpm, 4322);              // update in place
        CHECK(store.Commit());
    }
    CHECK(!FileExists(kTmp));                    // temp file swapped in, not left behind
    FileBackend backend(kFile);
    ParamStore store(kPumpParams, &backend);
    store.Init();
    CHECK(store.Get(P::MaxRpm) == 4322);
    CHECK(store.Get(P::PidKp) == 6.5f);
    RemoveTestFiles();
}

static void TestFileTruncatedAndGarbage()
{
    // A torn final record (partial write) is ignored; whole records load
    RemoveTestFiles();
    std::vector<uint8_t> bytes = Packed({ Rpm(2500), Kp(3.5f) });
    bytes.resize(bytes.size() - 5);
    WriteBytes(kFile, bytes);
    {
        FileBackend backend(kFile);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        CHECK(store.Get(P::MaxRpm) == 2500);
        CHECK(store.Get(P::PidKp) == 1.2f);      // torn: default
    }

    // Random bytes: no crash, defaults (CRC), and the file is writable again
    std::vector<uint8_t> junk(97);
    for (size_t i = 0; i < junk.size(); i++)
        junk[i] = static_cast<uint8_t>(i * 37 + 11);
    WriteBytes(kFile, junk);
    {
        FileBackend backend(kFile);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        CHECK(store.Get(P::MaxRpm) == 3000);
        store.Set(P::MaxRpm, 1111);
        CHECK(store.Commit());
    }
    CHECK(LoadMaxRpm() == 1111);
    RemoveTestFiles();
}

static void TestFileCrashStates()
{
    // Every state a crash during a save can leave behind. A save writes the
    // temp file, then replaces the main file with it in one atomic step.

    // Crash while writing the temp file: old main file + partial temp
    RemoveTestFiles();
    WriteBytes(kFile, Packed({ Rpm(1000) }));
    std::vector<uint8_t> partial = Packed({ Rpm(2000), Kp(9.0f) });
    partial.resize(PACKED_RECORD_SIZE + 3);
    WriteBytes(kTmp, partial);
    CHECK(LoadMaxRpm() == 1000);                 // old value; temp ignored
    {
        FileBackend backend(kFile);              // next save works and cleans up
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.Set(P::MaxRpm, 1500);
        CHECK(store.Commit());
    }
    CHECK(LoadMaxRpm() == 1500);
    CHECK(!FileExists(kTmp));

    // Crash after the temp file, before the replace: old main + complete temp
    RemoveTestFiles();
    WriteBytes(kFile, Packed({ Rpm(1000) }));
    WriteBytes(kTmp, Packed({ Rpm(2000) }));
    CHECK(LoadMaxRpm() == 1000);                 // the save never completed

    // Crash during the very first save: no main file, complete temp
    RemoveTestFiles();
    WriteBytes(kTmp, Packed({ Rpm(2000) }));
    CHECK(LoadMaxRpm() == 2000);                 // the only copy is used

    // ... or a partial temp: whole records load, the torn one keeps its default
    RemoveTestFiles();
    std::vector<uint8_t> firstTorn = Packed({ Rpm(2000), Kp(9.0f) });
    firstTorn.resize(PACKED_RECORD_SIZE + 6);
    WriteBytes(kTmp, firstTorn);
    {
        FileBackend backend(kFile);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        CHECK(store.Get(P::MaxRpm) == 2000);
        CHECK(store.Get(P::PidKp) == 1.2f);
        store.Set(P::PidKp, 4.0f);               // saving from here keeps both
        CHECK(store.Commit());
    }
    {
        FileBackend backend(kFile);
        ParamStore store(kPumpParams, &backend);
        store.Init();
        CHECK(store.Get(P::MaxRpm) == 2000);
        CHECK(store.Get(P::PidKp) == 4.0f);
    }
    CHECK(!FileExists(kTmp));
    RemoveTestFiles();
}

static void TestFileWriteFailure()
{
    // Directory doesn't exist: the save fails and is reported, nothing is lost
    FileBackend backend("device-params-no-such-dir/settings.params");
    ParamStore store(kPumpParams, &backend);
    store.Init();

    int failed = 0;
    auto conn = store.OnCommitFailed.Connect(dmq::MakeDelegate(std::function<void()>([&] { failed++; })));
    store.Set(P::MaxRpm, 1234);
    CHECK(!store.Commit());
    CHECK(failed == 1);
    CHECK(store.HasUnsavedChanges());
    CHECK(store.Get(P::MaxRpm) == 1234);
}

static void TestFileErase()
{
    RemoveTestFiles();
    FileBackend backend(kFile);
    CHECK(backend.Erase());                      // nothing to erase: still OK
    {
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.Set(P::MaxRpm, 777);
        CHECK(store.Commit());
    }
    WriteBytes(kTmp, Packed({ Rpm(888) }));      // stale temp too
    CHECK(backend.Erase());
    CHECK(!FileExists(kFile) && !FileExists(kTmp));
    CHECK(LoadMaxRpm() == 3000);
}

// -----------------------------------------------------------------------------
// RamBackend (test double used throughout; checked directly here)
// -----------------------------------------------------------------------------

static void TestRamBackend()
{
    RamBackend ram;
    std::vector<Record> seen;
    dmq::UnicastDelegate<void(const Record&)> sink;
    sink = dmq::MakeDelegate(std::function<void(const Record&)>([&](const Record& r) { seen.push_back(r); }));

    // Update in place by ID
    const Record a[] = { Rpm(1), Kp(2.0f) };
    CHECK(ram.Write(a, 2));
    const Record b[] = { Rpm(5) };
    CHECK(ram.Write(b, 1));
    CHECK(ram.RecordCount() == 2 && ram.WriteCount() == 2);
    CHECK(ram.ReadAll(sink) && seen.size() == 2);
    Value v;
    CHECK(DecodeRecord(seen[0], v) && v.i == 5);

    // A failed write is counted but changes nothing
    ram.FailWrites(true);
    const Record c[] = { Rpm(9) };
    CHECK(!ram.Write(c, 1));
    CHECK(ram.WriteCount() == 3);
    ram.FailWrites(false);
    seen.clear();
    ram.ReadAll(sink);
    CHECK(DecodeRecord(seen[0], v) && v.i == 5);

    // Corrupt breaks the CRC
    ram.Corrupt(P::MaxRpm.id);
    seen.clear();
    ram.ReadAll(sink);
    CHECK(!DecodeRecord(seen[0], v));

    // Erase empties it
    CHECK(ram.Erase());
    seen.clear();
    CHECK(ram.ReadAll(sink) && seen.empty() && ram.RecordCount() == 0);
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
    TestFileRoundTrip();
    TestFileTruncatedAndGarbage();
    TestFileCrashStates();
    TestFileWriteFailure();
    TestFileErase();
    TestRamBackend();
#ifdef PARAM_BACKEND_SQLITE
    TestSqlite();
#endif
    std::cout << "BackendTests: " << (failures ? "FAILED" : "passed") << "\n";
    return failures;
}
