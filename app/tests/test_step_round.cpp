// Unit test for keyboard step tuning (tune.h): arrow-key deltas and clamping to the FT-710 range.
// Built by CMake as test_step_round; run: ctest or ./test_step_round
#include "../tune.h"
#include <cstdio>
static int fails = 0;
static void eq(const char *what, double got, double want) {
    const bool ok = std::fabs(got - want) < 0.5;
    if (!ok) fails++;
    std::printf("%s %-58s got %.6f MHz, want %.6f MHz\n", ok ? "PASS" : "FAIL", what, got / 1e6, want / 1e6);
}
int main() {
    // key deltas: Left/Right coarse (5 kHz), Up/Down fine (500 Hz)
    eq("Left  = -5000",     keyStepHz(TuneKey::Left),  -5000);
    eq("Right = +5000",     keyStepHz(TuneKey::Right),  5000);
    eq("Up    = +500",      keyStepHz(TuneKey::Up),      500);
    eq("Down  = -500",      keyStepHz(TuneKey::Down),   -500);
    eq("None  = 0",         keyStepHz(TuneKey::None),      0);
    // stepping the dial
    eq("14.074000 Right -> 14.079000", stepDialHz(14074000, keyStepHz(TuneKey::Right)), 14079000);
    eq("14.074000 Left  -> 14.069000", stepDialHz(14074000, keyStepHz(TuneKey::Left)),  14069000);
    eq("14.074000 Up    -> 14.074500", stepDialHz(14074000, keyStepHz(TuneKey::Up)),    14074500);
    eq("14.074000 Down  -> 14.073500", stepDialHz(14074000, keyStepHz(TuneKey::Down)),  14073500);
    eq("non-whole dial rounds: 7.0740004 Up -> 7.074500", stepDialHz(7074000.4, keyStepHz(TuneKey::Up)), 7074500);
    eq("exact .5 rounds away from zero: 7.0740005 Down -> 7.073501", stepDialHz(7074000.5, keyStepHz(TuneKey::Down)), 7073501);
    // clamped to the FT-710 range (30 kHz .. 75 MHz), like tuneTo's guard
    eq("Down at 30001 -> 30000 (clamped low)",  stepDialHz(30001, keyStepHz(TuneKey::Down)), 30000);
    eq("Left at 32000 -> 30000 (clamped low)",  stepDialHz(32000, keyStepHz(TuneKey::Left)), 30000);
    eq("Up at 74999750 -> 75000000 (clamped high)", stepDialHz(74999750, keyStepHz(TuneKey::Up)), 75000000);
    eq("Right at 74996000 -> 75000000 (clamped high)", stepDialHz(74996000, keyStepHz(TuneKey::Right)), 75000000);
    eq("1.800 MHz Left -> 1.795 MHz (band edges are the radio's problem, not ours)", stepDialHz(1800000, keyStepHz(TuneKey::Left)), 1795000);
    // unknown / invalid dial -> 0: the caller must refuse (the app shows the VFO the poll reported)
    eq("dial 0 (VFO unknown) -> 0", stepDialHz(0, keyStepHz(TuneKey::Right)), 0);
    eq("dial -1 -> 0", stepDialHz(-1, keyStepHz(TuneKey::Right)), 0);
    eq("dial NaN -> 0", stepDialHz(0.0 / 0.0, keyStepHz(TuneKey::Right)), 0);
    eq("dial inf -> 0", stepDialHz(1.0 / 0.0, keyStepHz(TuneKey::Right)), 0);
    eq("delta 0 -> dial unchanged (caller sees no change and does nothing)", stepDialHz(14074000, 0), 14074000);
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
