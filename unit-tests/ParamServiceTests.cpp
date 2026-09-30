#include "ParamServiceTests.h"

#ifdef PARAM_REMOTE

#include "examples/PumpParams.h"
#include "params/ParamService.h"
#include "params/ParamClient.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

using namespace param;
using dmq::databus::DataBus;

static int failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond "\n"; failures++; } } while (0)

static bool WaitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
{
    auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

namespace {
    const Key<int32_t> Visible1{ 1 };
    const Key<int32_t> Secret{ 2 };
    const Key<bool>    Visible2{ 3 };

    constexpr Def kHiddenTable[] = {
        Int (Key<int32_t>{1}, "a.visible", 1, 0, 10),
        Int (Key<int32_t>{2}, "a.secret",  2, 0, 10, HIDDEN),
        Bool(Key<bool>{3},    "a.flag",    false),
    };

    // Collects everything a client receives
    struct Recorder {
        std::vector<ParamDescMsg>  descs;
        std::vector<ParamValueMsg> values;
        std::vector<ParamValueMsg> changes;
        dmq::ScopedConnection c1, c2, c3;

        explicit Recorder(ParamClient& client) {
            c1 = client.OnDesc.Connect(dmq::MakeDelegate(std::function<void(const ParamDescMsg&)>(
                [this](const ParamDescMsg& m) { descs.push_back(m); })));
            c2 = client.OnValue.Connect(dmq::MakeDelegate(std::function<void(const ParamValueMsg&)>(
                [this](const ParamValueMsg& m) { values.push_back(m); })));
            c3 = client.OnChanged.Connect(dmq::MakeDelegate(std::function<void(const ParamValueMsg&)>(
                [this](const ParamValueMsg& m) { changes.push_back(m); })));
        }
    };
}

// Synchronous dispatch throughout: service and client both Start(nullptr), so
// every reply has arrived by the time the Request*() call returns.
static void TestListGetSet()
{
    DataBus::ResetForTesting();
    ParamStore store(kPumpParams);
    store.Init();
    ParamService service(store, "pump");
    service.Start();
    ParamClient client("pump");
    client.Start();
    Recorder rec(client);

    // list
    uint32_t rid = client.RequestList();
    CHECK(rec.descs.size() == 5);
    if (rec.descs.size() == 5) {
        const ParamDescMsg& d = rec.descs[0];
        CHECK(d.requestId == rid && d.index == 0 && d.count == 5);
        CHECK(d.id == P::MaxRpm.id && d.type == static_cast<uint8_t>(TypeTag::Int32));
        CHECK(std::string(d.name) == "motor.max_rpm" && std::string(d.units) == "rpm");
        CHECK(d.maxBits == 6000 && d.valueBits == 3000);
        CHECK(rec.descs[4].index == 4);
    }

    // get
    rid = client.RequestGet(P::PidKp.id);
    CHECK(rec.values.size() == 1);
    bool ok = false;
    CHECK(rec.values.back().requestId == rid);
    CHECK(rec.values.back().GetValue(ok).f == 1.2f && ok);

    rid = client.RequestGet(999);
    CHECK(rec.values.back().result == static_cast<uint8_t>(SetResult::UNKNOWN_ID));

    // set: disabled by default
    client.RequestSet(P::MaxRpm.id, Value(int32_t(5000)));
    CHECK(rec.values.back().result == static_cast<uint8_t>(SetResult::REMOTE_DISABLED));
    CHECK(store.Get(P::MaxRpm) == 3000);
    CHECK(rec.changes.empty());

    service.AllowRemoteSet(true);
    rid = client.RequestSet(P::MaxRpm.id, Value(int32_t(5000)));
    CHECK(rec.values.back().requestId == rid);
    CHECK(rec.values.back().result == static_cast<uint8_t>(SetResult::OK));
    CHECK(store.Get(P::MaxRpm) == 5000);
    CHECK(rec.changes.size() == 1);
    if (!rec.changes.empty()) {
        CHECK(rec.changes[0].requestId == 0);
        CHECK(rec.changes[0].source == static_cast<uint8_t>(Source::Remote));
        CHECK(rec.changes[0].GetValue(ok).i == 5000);
    }

    // store rules still apply to remote sets
    client.RequestSet(P::MaxRpm.id, Value(int32_t(99999)));
    CHECK(rec.values.back().result == static_cast<uint8_t>(SetResult::OUT_OF_RANGE));
    CHECK(rec.values.back().GetValue(ok).i == 5000);    // reply carries current value
    client.RequestSet(P::SerialNo.id, Value(int32_t(7)));
    CHECK(rec.values.back().result == static_cast<uint8_t>(SetResult::READ_ONLY));
    client.RequestSet(P::MaxRpm.id, Value(1.0f));
    CHECK(rec.values.back().result == static_cast<uint8_t>(SetResult::TYPE_MISMATCH));

    // local change is published too
    store.Set(P::Telemetry, false);
    CHECK(rec.changes.size() == 2);
    if (rec.changes.size() == 2)
        CHECK(rec.changes[1].source == static_cast<uint8_t>(Source::Local));

    // after Stop, no more replies
    service.Stop();
    size_t before = rec.values.size();
    client.RequestGet(P::MaxRpm.id);
    CHECK(rec.values.size() == before);
}

static void TestHidden()
{
    DataBus::ResetForTesting();
    ParamStore store(kHiddenTable);
    store.Init();
    ParamService service(store, "dev");
    service.AllowRemoteSet(true);
    service.Start();
    ParamClient client("dev");
    client.Start();
    Recorder rec(client);

    client.RequestList();
    CHECK(rec.descs.size() == 2);
    for (const auto& d : rec.descs)
        CHECK(d.id != Secret.id && d.count == 2);

    client.RequestGet(Secret.id);
    CHECK(rec.values.back().result == static_cast<uint8_t>(SetResult::UNKNOWN_ID));
    client.RequestSet(Secret.id, Value(int32_t(3)));
    CHECK(rec.values.back().result == static_cast<uint8_t>(SetResult::UNKNOWN_ID));
    CHECK(store.Get(Secret) == 2);

    store.Set(Secret, 4);           // hidden change is not published
    CHECK(rec.changes.empty());
    store.Set(Visible2, true);
    CHECK(rec.changes.size() == 1);
    (void)Visible1;
}

static void TestServiceOnThread()
{
    DataBus::ResetForTesting();
    ParamStore store(kPumpParams);
    store.Init();

    dmq::os::Thread serviceThread("ParamServiceThread");
    serviceThread.CreateThread();

    ParamService service(store, "pump");
    service.AllowRemoteSet(true);
    service.Start(&serviceThread);
    ParamClient client("pump");
    client.Start();

    std::atomic<int> replies{ 0 };
    std::atomic<bool> onServiceThread{ false };
    auto conn = client.OnValue.Connect(dmq::MakeDelegate(std::function<void(const ParamValueMsg&)>(
        [&](const ParamValueMsg& m) {
            onServiceThread = serviceThread.IsCurrentThread();
            if (m.result == static_cast<uint8_t>(SetResult::OK)) replies++;
        })));

    client.RequestSet(P::MaxRpm.id, Value(int32_t(1234)));
    CHECK(WaitFor([&] { return replies.load() == 1; }));
    CHECK(onServiceThread.load());      // request handled (and reply published) on the service thread
    CHECK(store.Get(P::MaxRpm) == 1234);

    service.Stop();
    serviceThread.ExitThread();
}

template <class T>
static T RoundTrip(T& in)
{
    serialize ms;
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    in.write(ms, ss);
    T out;
    out.read(ms, ss);
    return out;
}

static void TestSerialization()
{
    ParamDescMsg d;
    d.requestId = 7; d.index = 2; d.count = 5; d.id = 42; d.type = 3;
    d.flags = PERSIST | READ_ONLY; d.defBits = 1; d.minBits = 2; d.maxBits = 3; d.valueBits = 0xDEADBEEF;
    CopyStr(d.name, NAME_LEN, "motor.max_rpm");
    CopyStr(d.units, UNITS_LEN, "rpm");
    ParamDescMsg d2 = RoundTrip(d);
    CHECK(d2.requestId == 7 && d2.index == 2 && d2.count == 5 && d2.id == 42 && d2.type == 3);
    CHECK(d2.flags == d.flags && d2.valueBits == 0xDEADBEEF && d2.maxBits == 3);
    CHECK(std::string(d2.name) == "motor.max_rpm" && std::string(d2.units) == "rpm");

    ParamValueMsg v;
    v.requestId = 9; v.id = 1; v.result = 3; v.source = 1;
    v.SetValue(Value(-2.5f));
    ParamValueMsg v2 = RoundTrip(v);
    bool ok = false;
    CHECK(v2.requestId == 9 && v2.id == 1 && v2.result == 3 && v2.source == 1);
    CHECK(v2.GetValue(ok).f == -2.5f && ok);

    ParamSetReq s;
    s.requestId = 11; s.id = 4; s.SetValue(Value::FromEnum(Mode::Service));
    ParamSetReq s2 = RoundTrip(s);
    CHECK(s2.requestId == 11 && s2.id == 4);
    CHECK(s2.GetValue(ok).type == TypeTag::Enum && s2.GetValue(ok).i == 2);

    ParamGetReq g; g.requestId = 12; g.id = 5;
    ParamGetReq g2 = RoundTrip(g);
    CHECK(g2.requestId == 12 && g2.id == 5);
}

int RunParamServiceTests()
{
    failures = 0;
    TestListGetSet();
    TestHidden();
    TestServiceOnThread();
    TestSerialization();
    DataBus::ResetForTesting();
    std::cout << "ParamServiceTests: " << (failures ? "FAILED" : "passed") << "\n";
    return failures;
}

#else

int RunParamServiceTests() { return 0; }

#endif
