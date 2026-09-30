#include "PumpExample.h"
#include "PumpParams.h"
#include "params/ParamStore.h"
#include "params/backends/RamBackend.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace Example {

void RunPumpExample()
{
    std::cout << "--- Pump Example ---\n";

    // Deferred saves need timers serviced; a real app does this in a tick task/ISR
    std::atomic<bool> exitTimers{ false };
    std::thread timerThread([&] {
        while (!exitTimers) {
            dmq::util::Timer::ProcessTimers();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    dmq::os::Thread motorThread("MotorThread");
    dmq::os::Thread saveThread("ParamSaveThread");
    motorThread.CreateThread();
    saveThread.CreateThread();

    param::RamBackend backend;
    {
        param::ParamStore store(kPumpParams, &backend);
        store.SetSaveMode(param::SaveMode::Deferred, std::chrono::milliseconds(100), &saveThread);
        store.Init();

        std::atomic<int> notified{ 0 };
        auto conn = store.Subscribe(P::MaxRpm, [&](int32_t rpm, param::Source) {
            std::cout << "  [MotorThread] MaxRpm -> " << rpm << "\n";
            notified++;
        }, &motorThread);

        // Wait for the MotorThread callback before printing, so the two
        // threads' output doesn't interleave.
        bool ok = store.Set(P::MaxRpm, 4500) == param::SetResult::OK;
        while (ok && notified == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::cout << "Set MaxRpm 4500: " << (ok ? "OK" : "failed") << "\n";

        bool rejected = store.Set(P::MaxRpm, 9000) == param::SetResult::OUT_OF_RANGE;
        std::cout << "Set MaxRpm 9000: " << (rejected ? "OUT_OF_RANGE" : "?") << "\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::cout << "Records saved after deferred commit: " << backend.RecordCount() << "\n";
    }

    // A new store on the same backend (as after a reboot) reloads the value
    param::ParamStore reloaded(kPumpParams, &backend);
    reloaded.Init();
    std::cout << "Reloaded MaxRpm: " << reloaded.Get(P::MaxRpm) << "\n";

    motorThread.ExitThread();
    saveThread.ExitThread();
    exitTimers = true;
    timerThread.join();
}

} // namespace Example
