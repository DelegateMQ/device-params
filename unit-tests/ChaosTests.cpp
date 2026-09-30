#include "ChaosTests.h"
#include "SimFlash.h"
#include "examples/PumpParams.h"
#include "params/ParamStore.h"
#include "params/Record.h"
#include "params/backends/FlashLogBackend.h"
#include "params/backends/RamBackend.h"
#ifdef PARAM_REMOTE
#include "params/ParamService.h"
#include "params/ParamClient.h"
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace param;
using namespace std::chrono_literals;

static int failures = 0;
static std::mutex s_failMutex;

#define CHECK(cond) \
    do { if (!(cond)) { std::lock_guard<std::mutex> _l(s_failMutex); \
         std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond "\n"; failures++; } } while (0)

namespace {

constexpr size_t N = sizeof(kPumpParams) / sizeof(kPumpParams[0]);

struct Rng {
    std::mt19937 gen;
    explicit Rng(uint32_t seed) : gen(seed) {}
    uint32_t Next(uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(gen); }
    bool Chance(double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(gen) < p; }
};

bool Same(const Value& a, const Value& b)
{
    return a.type == b.type && ToBits(a) == ToBits(b);
}

bool InRange(const Def& d, const Value& v)
{
    if (v.type != d.type) return false;
    switch (d.type) {
    case TypeTag::Bool:   return true;
    case TypeTag::UInt32: return v.u >= d.minValue.u && v.u <= d.maxValue.u;
    case TypeTag::Float:  return v.f >= d.minValue.f && v.f <= d.maxValue.f;
    default:              return v.i >= d.minValue.i && v.i <= d.maxValue.i;
    }
}

/// Random in-range value for a definition; with outOfRange, sometimes an invalid one.
Value RandomValue(const Def& d, Rng& r, bool outOfRange = false)
{
    switch (d.type) {
    case TypeTag::Bool:
        return Value(r.Next(2) == 1);
    case TypeTag::UInt32:
        return Value(d.minValue.u + r.Next(d.maxValue.u - d.minValue.u + 1));
    case TypeTag::Float: {
        if (outOfRange) return Value(d.maxValue.f + 1.0f);
        const float t = std::uniform_real_distribution<float>(0.0f, 1.0f)(r.gen);
        return Value(d.minValue.f + t * (d.maxValue.f - d.minValue.f));
    }
    case TypeTag::Int32:
    case TypeTag::Enum: {
        Value v;
        if (outOfRange && d.maxValue.i < INT32_MAX)
            v = Value(static_cast<int32_t>(d.maxValue.i + 1));
        else {
            const uint32_t span = static_cast<uint32_t>(d.maxValue.i - d.minValue.i);
            const uint32_t off = (span == UINT32_MAX) ? r.gen() : r.Next(span + 1);
            v = Value(static_cast<int32_t>(d.minValue.i + static_cast<int32_t>(off)));
        }
        v.type = d.type;
        return v;
    }
    }
    return Value();
}

/// Aborts the process if a test runs longer than its budget (deadlock detector).
class Watchdog {
public:
    Watchdog(const char* name, std::chrono::seconds budget) : m_thread([this, name, budget] {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, budget, [this] { return m_done; })) {
            std::cout << "CHAOS WATCHDOG: " << name << " exceeded " << budget.count()
                      << " s (deadlock?)" << std::endl;
            std::abort();
        }
    }) {}
    ~Watchdog() {
        { std::lock_guard<std::mutex> lock(m_mutex); m_done = true; }
        m_cv.notify_one();
        m_thread.join();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_done = false;
    std::thread m_thread;
};

bool WaitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 3000ms)
{
    auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

/// Services dmq::util::Timer while alive (deferred saves).
class TimerPump {
public:
    TimerPump() : m_thread([this] {
        while (!m_stop) { dmq::util::Timer::ProcessTimers(); std::this_thread::sleep_for(1ms); }
    }) {}
    ~TimerPump() { m_stop = true; m_thread.join(); }
private:
    std::atomic<bool> m_stop{ false };
    std::thread m_thread;
};

/// Wraps a backend and fails writes at random (thread-safe).
class FlakyBackend : public IBackend {
public:
    FlakyBackend(IBackend& inner, uint32_t seed) : m_inner(inner), m_rng(seed) {}
    void SetFailRate(double p) { std::lock_guard<std::mutex> l(m_mutex); m_failRate = p; }
    size_t Failures() const { return m_failures; }

    bool ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink) override { return m_inner.ReadAll(sink); }
    bool Write(const Record* recs, size_t count) override {
        {
            std::lock_guard<std::mutex> l(m_mutex);
            if (m_rng.Chance(m_failRate)) { m_failures++; return false; }
        }
        return m_inner.Write(recs, count);
    }
    bool Erase() override { return m_inner.Erase(); }

private:
    IBackend& m_inner;
    std::mutex m_mutex;
    Rng m_rng;
    double m_failRate = 0.0;
    std::atomic<size_t> m_failures{ 0 };
};

} // namespace

// -----------------------------------------------------------------------------
// 1. Flash power-loss fuzzing
// Invariant after every reboot: each persisted parameter holds either its last
// durable value or the value that was being committed when power was cut.
// A commit with no power cut must be fully durable.
// -----------------------------------------------------------------------------
static void ChaosFlashPowerLoss(uint32_t seed, int scale)
{
    Watchdog wd("ChaosFlashPowerLoss", 120s);
    const size_t writeSizes[] = { 1, 4, 8, 16 };
    size_t reboots = 0, cuts = 0, compactions = 0;

    for (size_t ws : writeSizes) {
        Rng r(seed ^ static_cast<uint32_t>(ws * 7919));
        SimFlash flash(128, ws);    // small sectors: frequent compaction
        std::array<Value, N> durable;
        std::array<std::optional<Value>, N> inflight;
        for (size_t i = 0; i < N; i++) durable[i] = kPumpParams[i].defValue;

        for (int it = 0; it < 250 * scale; it++, flash.RestorePower()) {
            FlashLogBackend backend(flash);
            ParamStore store(kPumpParams, &backend);
            store.Init();
            reboots++;

            for (size_t i = 0; i < N; i++) {
                const Def& d = kPumpParams[i];
                const Value v = store.GetValue(d.id);
                if (!(d.flags & PERSIST)) {
                    CHECK(Same(v, d.defValue));
                    continue;
                }
                const bool ok = Same(v, durable[i]) || (inflight[i] && Same(v, *inflight[i]));
                if (!ok)
                    std::cout << "  power-loss: ws=" << ws << " it=" << it << " param " << d.id
                              << " bits=" << ToBits(v) << " durable=" << ToBits(durable[i]) << "\n";
                CHECK(ok);
                durable[i] = v;         // what loaded is now the durable truth
                inflight[i].reset();
            }

            // Change 1..3 persisted parameters
            const int changes = 1 + static_cast<int>(r.Next(3));
            for (int c = 0; c < changes; c++) {
                size_t i;
                do { i = r.Next(N); } while (!(kPumpParams[i].flags & PERSIST));
                const Value v = RandomValue(kPumpParams[i], r);
                if (store.SetValue(kPumpParams[i].id, v, Source::Local) == SetResult::OK)
                    inflight[i] = v;
            }

            const bool cut = r.Chance(0.6);
            if (cut) {
                flash.CutPowerAfter(static_cast<int>(r.Next(5)), r.Chance(0.5));
                cuts++;
            }
            const bool committed = store.Commit();
            if (!cut) {
                CHECK(committed);
                for (size_t i = 0; i < N; i++)
                    if (inflight[i]) { durable[i] = *inflight[i]; inflight[i].reset(); }
            }
            compactions += backend.CompactionCount();
            // Store destructor may retry the commit with whatever power budget
            // is left; power returns (loop increment) only after it has run.
        }
        CHECK(flash.Violations() == 0);
    }
    std::cout << "  flash power loss: " << reboots << " reboots, " << cuts << " power cuts, "
              << compactions << " compactions\n";
}

// -----------------------------------------------------------------------------
// 2. Flash bit rot: random bit flips must never crash or load invalid values
// -----------------------------------------------------------------------------
class RottingFlash : public SimFlash {
public:
    using SimFlash::SimFlash;
    void Flip(size_t sector, size_t byte, uint8_t mask) { RawSector(sector)[byte] ^= mask; }
};

static void ChaosFlashBitRot(uint32_t seed, int scale)
{
    Watchdog wd("ChaosFlashBitRot", 60s);
    Rng r(seed ^ 0xB17B07u);
    size_t rejected = 0;

    for (int it = 0; it < 200 * scale; it++) {
        RottingFlash flash(256, 4);
        {
            FlashLogBackend backend(flash);
            ParamStore store(kPumpParams, &backend);
            store.Init();
            const int commits = 1 + static_cast<int>(r.Next(30));
            for (int c = 0; c < commits; c++) {
                const Def& d = kPumpParams[r.Next(N)];
                store.SetValue(d.id, RandomValue(d, r), Source::Local);
                store.Commit();
            }
        }

        const int flips = 1 + static_cast<int>(r.Next(8));
        for (int f = 0; f < flips; f++)
            flash.Flip(r.Next(2), r.Next(256), static_cast<uint8_t>(1u << r.Next(8)));

        FlashLogBackend backend(flash);
        ParamStore store(kPumpParams, &backend);
        auto conn = store.OnRejected.Connect(dmq::MakeDelegate(
            std::function<void(ParamId, SetResult, Source)>([&](ParamId, SetResult, Source) { rejected++; })));
        store.Init();
        for (size_t i = 0; i < N; i++)
            CHECK(InRange(kPumpParams[i], store.GetValue(kPumpParams[i].id)));

        // The log must stay writable after corruption
        store.Set(P::MaxRpm, 1234);
        store.Commit();
    }
    std::cout << "  flash bit rot: " << 200 * scale << " corrupted images, " << rejected << " records rejected\n";
}

// -----------------------------------------------------------------------------
// 3. Concurrent store: setters, getters, resets, commits, subscriber churn,
//    thread-dispatched subscribers and deferred saves all at once.
// -----------------------------------------------------------------------------
static void ChaosConcurrentStore(uint32_t seed, int scale)
{
    Watchdog wd("ChaosConcurrentStore", 120s);
    SimFlash flash(1024, 8);
    FlashLogBackend backend(flash);
    TimerPump timers;

    dmq::os::Thread saveThread("ChaosSave");
    dmq::os::Thread subA("ChaosSubA");
    dmq::os::Thread subB("ChaosSubB");
    saveThread.CreateThread();
    subA.CreateThread();
    subB.CreateThread();

    std::array<Value, N> finalValues;
    std::atomic<size_t> ops{ 0 }, notifications{ 0 }, churns{ 0 };
    {
        ParamStore store(kPumpParams, &backend);
        store.Init();
        store.SetSaveMode(SaveMode::Deferred, 3ms, &saveThread);

        // Validator re-enters the store (recursive lock) and vetoes some sets
        dmq::UnicastDelegate<bool(ParamId, const Value&)> validator;
        validator = dmq::MakeDelegate(std::function<bool(ParamId, const Value&)>([&](ParamId id, const Value& v) {
            return !(id == P::Telemetry.id && !v.b && store.Get(P::RunMode) == Mode::Service);
        }));
        store.SetValidator(validator);

        std::atomic<int32_t> lastA{ -1 }, lastB{ -1 };
        auto connA = store.Subscribe(P::MaxRpm, [&](int32_t rpm, Source) { lastA = rpm; notifications++; }, &subA);
        auto connB = store.SubscribeAny([&](ParamId id, Value v, Source) {
            if (id == P::MaxRpm.id) lastB = v.i;
            notifications++;
        }, &subB);
        auto connSync = store.SubscribeAny([&](ParamId, Value, Source) { notifications++; });

        // Subscribe/unsubscribe as fast as possible while the writers run
        // (sleep granularity is too coarse on Windows, so just yield)
        std::atomic<bool> stop{ false };
        std::thread churn([&] {
            Rng r(seed ^ 0xC4u);
            while (!stop || churns < 1000u * static_cast<size_t>(scale)) {
                auto c = store.Subscribe(P::PidKp, [&](float, Source) { notifications++; },
                                         r.Chance(0.5) ? &subB : nullptr);
                std::this_thread::yield();
                churns++;
            }
        });

        std::vector<std::thread> writers;
        for (int w = 0; w < 6; w++) {
            writers.emplace_back([&, w] {
                Rng r(seed + static_cast<uint32_t>(w) * 101u);
                for (int k = 0; k < 3000 * scale; k++) {
                    const Def& d = kPumpParams[r.Next(N)];
                    const uint32_t op = r.Next(100);
                    if (op < 60)
                        store.SetValue(d.id, RandomValue(d, r, r.Chance(0.1)), r.Chance(0.2) ? Source::Remote : Source::Local);
                    else if (op < 95)
                        CHECK(InRange(d, store.GetValue(d.id)));
                    else if (op < 98)
                        store.Commit();
                    else
                        store.ResetToDefaults(r.Chance(0.5) ? ALL : PERSIST);
                    ops++;
                }
            });
        }
        for (auto& t : writers) t.join();
        stop = true;
        churn.join();

        // Subscribers must converge on the final value once queues drain
        const int32_t finalRpm = store.Get(P::MaxRpm);
        const bool convergedA = WaitFor([&] { return lastA == finalRpm; });
        const bool convergedB = WaitFor([&] { return lastB == finalRpm; });
        if (!convergedA || !convergedB)
            std::cout << "  concurrent: final MaxRpm " << finalRpm << ", subscriber A saw " << lastA
                      << ", B saw " << lastB << "\n";
        CHECK(convergedA);
        CHECK(convergedB);

        for (size_t i = 0; i < N; i++)
            finalValues[i] = store.GetValue(kPumpParams[i].id);
    }   // destructor drains the save thread and commits the rest

    FlashLogBackend reloadBackend(flash);
    ParamStore reloaded(kPumpParams, &reloadBackend);
    reloaded.Init();
    for (size_t i = 0; i < N; i++)
        if (kPumpParams[i].flags & PERSIST)
            CHECK(Same(reloaded.GetValue(kPumpParams[i].id), finalValues[i]));
    CHECK(flash.Violations() == 0);

    saveThread.ExitThread();
    subA.ExitThread();
    subB.ExitThread();
    std::cout << "  concurrent store: " << ops << " ops, " << notifications << " notifications, "
              << churns << " subscribe/unsubscribe cycles\n";
}

// -----------------------------------------------------------------------------
// 4. Remote access under load. The service thread uses FullPolicy::DROP, as a
//    device should: requests come from outside, so a flood must drop requests
//    rather than fault the device. Invariants: no request is answered twice
//    or attributed to the wrong client, replies carry valid values, and the
//    service answers normally once the flood ends.
// -----------------------------------------------------------------------------
#ifdef PARAM_REMOTE
static void ChaosRemote(uint32_t seed, int scale)
{
    Watchdog wd("ChaosRemote", 120s);
    dmq::databus::DataBus::ResetForTesting();

    ParamStore store(kPumpParams);
    store.Init();

    dmq::os::Thread serviceThread("ChaosService", 0, dmq::FullPolicy::DROP);
    serviceThread.CreateThread();
    ParamService service(store, "chaos");
    service.AllowRemoteSet(true);
    service.Start(&serviceThread);

    constexpr int CLIENTS = 4;
    const int requestsPerClient = 1500 * scale;
    std::atomic<size_t> answeredTwice{ 0 }, answered{ 0 }, badValues{ 0 };

    struct ClientState {
        std::mutex mutex;
        std::set<uint32_t> sent;
        std::set<uint32_t> answeredIds;
    };
    std::array<ClientState, CLIENTS> states;
    std::vector<std::unique_ptr<ParamClient>> clients;
    std::vector<dmq::ScopedConnection> conns;

    for (int c = 0; c < CLIENTS; c++) {
        clients.push_back(std::make_unique<ParamClient>("chaos"));
        clients.back()->Start();    // replies arrive on the service thread
        ClientState& st = states[c];
        conns.push_back(clients.back()->OnValue.Connect(dmq::MakeDelegate(std::function<void(const ParamValueMsg&)>(
            [&st, &answeredTwice, &answered, &badValues](const ParamValueMsg& m) {
                std::lock_guard<std::mutex> l(st.mutex);
                // Every client sees every reply; only this client's own count
                if (st.sent.count(m.requestId) == 0)
                    return;
                if (!st.answeredIds.insert(m.requestId).second) {
                    answeredTwice++;
                    return;
                }
                answered++;
                const Def* d = nullptr;
                for (const Def& def : kPumpParams) if (def.id == m.id) d = &def;
                bool ok = false;
                const Value v = m.GetValue(ok);
                if (d && (!ok || !InRange(*d, v))) badValues++;
            }))));
    }

    // Track requestIds across clients: each must belong to exactly one sender
    std::mutex idMutex;
    std::multiset<uint32_t> allIds;

    std::atomic<bool> stopLocal{ false };
    std::thread local([&] {
        Rng r(seed ^ 0x10CA1u);
        while (!stopLocal) {
            const Def& d = kPumpParams[r.Next(N)];
            store.SetValue(d.id, RandomValue(d, r), Source::Local);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    std::vector<std::thread> senders;
    for (int c = 0; c < CLIENTS; c++) {
        senders.emplace_back([&, c] {
            Rng r(seed + 7u * static_cast<uint32_t>(c));
            for (int k = 0; k < requestsPerClient; k++) {
                const Def& d = kPumpParams[r.Next(N)];
                {
                    // Hold the lock across the send: the reply can arrive before RequestX returns
                    std::lock_guard<std::mutex> l(states[c].mutex);
                    uint32_t id;
                    if (r.Chance(0.5))
                        id = clients[c]->RequestGet(d.id);
                    else
                        id = clients[c]->RequestSet(d.id, RandomValue(d, r, r.Chance(0.1)));
                    states[c].sent.insert(id);
                    std::lock_guard<std::mutex> il(idMutex);
                    allIds.insert(id);
                }
                // Clients 0-1 pause now and then, so the service works during
                // the flood; clients 2-3 burst, so its queue still overflows
                if (c < 2 && k % 64 == 63)
                    std::this_thread::sleep_for(1ms);
            }
        });
    }
    for (auto& t : senders) t.join();
    stopLocal = true;
    local.join();

    // Let the queue drain: wait until the answered count stops moving
    size_t last = SIZE_MAX;
    while (answered != last) {
        last = answered;
        std::this_thread::sleep_for(100ms);
    }

    const size_t total = static_cast<size_t>(CLIENTS) * static_cast<size_t>(requestsPerClient);
    const size_t answeredFlood = answered;
    CHECK(answeredFlood <= total);
    const size_t dropped = total - answeredFlood;

    // Recovery: after the flood, a request is answered normally
    uint32_t probe;
    {
        std::lock_guard<std::mutex> l(states[0].mutex);
        probe = clients[0]->RequestGet(P::MaxRpm.id);
        states[0].sent.insert(probe);
    }
    CHECK(WaitFor([&] {
        std::lock_guard<std::mutex> l(states[0].mutex);
        return states[0].answeredIds.count(probe) == 1;
    }));

    size_t collisions = 0;
    for (auto it = allIds.begin(); it != allIds.end(); it = allIds.upper_bound(*it))
        if (allIds.count(*it) > 1) collisions++;
    if (collisions)
        std::cout << "  remote: " << collisions << " requestIds used by more than one client\n";
    CHECK(collisions == 0);
    CHECK(answeredTwice == 0);
    CHECK(badValues == 0);

    service.Stop();
    conns.clear();
    clients.clear();
    serviceThread.ExitThread();
    dmq::databus::DataBus::ResetForTesting();
    std::cout << "  remote: " << total << " requests from " << CLIENTS << " clients, "
              << answeredFlood << " answered, " << dropped << " dropped by the service thread\n";
}

// Tear down services and clients while requests are still queued on their
// threads. A queued request must never run against a destroyed object (run
// under AddressSanitizer to see a use-after-free).
static void ChaosRemoteTeardown(uint32_t seed, int scale)
{
    Watchdog wd("ChaosRemoteTeardown", 120s);
    dmq::databus::DataBus::ResetForTesting();
    Rng r(seed ^ 0x7EA2u);

    ParamStore store(kPumpParams);
    store.Init();
    dmq::os::Thread serviceThread("ChaosTeardownService", 0, dmq::FullPolicy::DROP);
    dmq::os::Thread clientThread("ChaosTeardownClient", 0, dmq::FullPolicy::DROP);
    serviceThread.CreateThread();
    clientThread.CreateThread();

    std::atomic<size_t> replies{ 0 };
    for (int round = 0; round < 50 * scale; round++) {
        auto service = std::make_unique<ParamService>(store, "teardown");
        service->AllowRemoteSet(true);
        service->Start(&serviceThread);
        auto client = std::make_unique<ParamClient>("teardown");
        client->Start(&clientThread);
        auto conn = client->OnValue.Connect(dmq::MakeDelegate(std::function<void(const ParamValueMsg&)>(
            [&](const ParamValueMsg&) { replies++; })));

        const int burst = 50 + static_cast<int>(r.Next(200));
        for (int k = 0; k < burst; k++) {
            const Def& d = kPumpParams[r.Next(N)];
            if (r.Chance(0.5))
                client->RequestGet(d.id);
            else
                client->RequestSet(d.id, RandomValue(d, r));
        }
        // Destroy in random order with requests and replies still queued
        if (r.Chance(0.5)) { service.reset(); conn.Disconnect(); client.reset(); }
        else               { conn.Disconnect(); client.reset(); service.reset(); }
    }

    serviceThread.ExitThread();
    clientThread.ExitThread();
    dmq::databus::DataBus::ResetForTesting();
    std::cout << "  remote teardown: " << 50 * scale << " service/client lifetimes under load, "
              << replies << " replies delivered\n";
}

#endif

// -----------------------------------------------------------------------------
// 5. Flaky backend: deferred saves must get through once storage recovers,
//    with no further changes to trigger them.
// -----------------------------------------------------------------------------
static void ChaosFlakyBackend(uint32_t seed, int scale)
{
    Watchdog wd("ChaosFlakyBackend", 60s);
    RamBackend ram;
    FlakyBackend flaky(ram, seed ^ 0xF1A4u);
    TimerPump timers;
    dmq::os::Thread saveThread("ChaosFlakySave");
    saveThread.CreateThread();
    Rng r(seed ^ 0x5EEDu);

    std::array<Value, N> finalValues;
    {
        ParamStore store(kPumpParams, &flaky);
        store.Init();
        store.SetSaveMode(SaveMode::Deferred, 2ms, &saveThread);

        flaky.SetFailRate(0.7);
        for (int k = 0; k < 400 * scale; k++) {
            const Def& d = kPumpParams[r.Next(N)];
            store.SetValue(d.id, RandomValue(d, r), Source::Local);
            if (k % 20 == 0) std::this_thread::sleep_for(3ms);
        }

        // Make the final deferred save fail: storage fully down, one last
        // change, and wait for its save attempt to fail.
        flaky.SetFailRate(1.0);
        const size_t before = flaky.Failures();
        store.Set(P::MaxRpm, 4242);
        CHECK(WaitFor([&] { return flaky.Failures() > before; }));

        // Storage recovers; no more sets. The pending save must still land.
        flaky.SetFailRate(0.0);
        const bool saved = WaitFor([&] { return !store.HasUnsavedChanges(); }, 3000ms);
        if (!saved)
            std::cout << "  flaky: unsaved changes stuck after storage recovered ("
                      << flaky.Failures() << " failed writes)\n";
        CHECK(saved);

        for (size_t i = 0; i < N; i++)
            finalValues[i] = store.GetValue(kPumpParams[i].id);
    }
    saveThread.ExitThread();

    ParamStore reloaded(kPumpParams, &ram);
    reloaded.Init();
    for (size_t i = 0; i < N; i++)
        if (kPumpParams[i].flags & PERSIST)
            CHECK(Same(reloaded.GetValue(kPumpParams[i].id), finalValues[i]));
    std::cout << "  flaky backend: " << flaky.Failures() << " injected write failures\n";
}

int RunChaosTests(uint32_t seed, int scale)
{
    failures = 0;
    if (seed == 0)
        seed = std::random_device{}();
    if (scale < 1)
        scale = 1;
    std::cout << "ChaosTests: seed " << seed << ", scale " << scale << "\n";

    const auto start = std::chrono::steady_clock::now();
    ChaosFlashPowerLoss(seed, scale);
    ChaosFlashBitRot(seed, scale);
    ChaosConcurrentStore(seed, scale);
#ifdef PARAM_REMOTE
    ChaosRemote(seed, scale);
    ChaosRemoteTeardown(seed, scale);
#endif
    ChaosFlakyBackend(seed, scale);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::cout << "ChaosTests: " << (failures ? "FAILED" : "passed") << " (" << secs << " s, seed " << seed
              << "; replay with --chaos-seed " << seed << ")\n";
    return failures;
}
