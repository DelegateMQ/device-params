#ifndef _CHAOS_TESTS_H
#define _CHAOS_TESTS_H

// ChaosTests.h
// Randomized stress and fault-injection tests: flash power loss and bit rot,
// concurrent store access, remote access under load and a flaky backend.
//
// @param seed   RNG seed (0 = pick one; the seed is printed so a failure can
//               be replayed with --chaos-seed).
// @param scale  Iteration multiplier (1 = a few seconds).
// Returns the number of failed checks (0 = pass).

#include <cstdint>

int RunChaosTests(uint32_t seed, int scale);

#endif
