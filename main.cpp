// main.cpp
// Runs the device-params examples and unit tests.
// @see https://github.com/DelegateMQ/DelegateMQ

#include "examples/PumpExample.h"
#include "unit-tests/ParamStoreTests.h"
#include "unit-tests/ParamServiceTests.h"
#include "unit-tests/BackendTests.h"
#include "delegate-mq/DelegateMQ.h"
#include <iostream>

int main()
{
    Example::RunPumpExample();

    int failures = RunParamStoreTests();
    failures += RunParamServiceTests();
    failures += RunBackendTests();
    return failures == 0 ? 0 : 1;
}
