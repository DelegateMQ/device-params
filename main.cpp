// main.cpp
// Runs the device-params examples and unit tests.
// @see https://github.com/DelegateMQ/DelegateMQ

#include "examples/PumpExample.h"
#include "examples/SqliteExample.h"
#include "unit-tests/ParamStoreTests.h"
#include "unit-tests/ParamServiceTests.h"
#include "unit-tests/BackendTests.h"
#include "unit-tests/ChaosTests.h"
#include "delegate-mq/DelegateMQ.h"
#include <cstdlib>
#include <cstring>
#include <iostream>

// Options:
//   --chaos-seed N    replay chaos tests with seed N (default: random)
//   --chaos-scale N   chaos iteration multiplier (default 1)
//   --no-chaos        skip chaos tests
int main(int argc, char* argv[])
{
    uint32_t chaosSeed = 0;
    int chaosScale = 1;
    bool chaos = true;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--chaos-seed") == 0 && i + 1 < argc)
            chaosSeed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--chaos-scale") == 0 && i + 1 < argc)
            chaosScale = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--no-chaos") == 0)
            chaos = false;
    }

    Example::RunPumpExample();
    Example::RunSqliteExample();

    int failures = RunParamStoreTests();
    failures += RunParamServiceTests();
    failures += RunBackendTests();
    if (chaos)
        failures += RunChaosTests(chaosSeed, chaosScale);
    return failures == 0 ? 0 : 1;
}
