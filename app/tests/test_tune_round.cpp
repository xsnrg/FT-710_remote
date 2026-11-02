// Unit test for click-to-tune rounding (tune.h). Built by CMake as test_tune_round; run: ctest or ./test_tune_round
#include "../tune.h"
#include <cstdio>
static int fails = 0;
static void eq(const char *what, double got, double want) {
    const bool ok = std::fabs(got - want) < 0.5;
    if (!ok) fails++;
    std::printf("%s %-44s got %.6f MHz, want %.6f MHz\n", ok ? "PASS" : "FAIL", what, got / 1e6, want / 1e6);
}
int main() {
    eq("snap: 3.788678 MHz -> 3.789", clickTuneHz(3788678, 100000, true), 3789000);
    eq("snap: 3.7884 MHz -> 3.788", clickTuneHz(3788400, 100000, true), 3788000);
    eq("snap: 3.7885 MHz (exact half) -> 3.789", clickTuneHz(3788500, 100000, true), 3789000);
    eq("snap: 3.7885 MHz with float error", clickTuneHz(3.7885e6 - 1e-7, 100000, true), 3789000);
    eq("snap: 14.074 stays 14.074", clickTuneHz(14074000, 50000, true), 14074000);
    eq("snap: 7.0749999 -> 7.075", clickTuneHz(7074999.9, 20000, true), 7075000);
    eq("snap: 50.3134 MHz -> 50.313", clickTuneHz(50313400, 1000000, true), 50313000);
    eq("snap: narrow span still whole kHz", clickTuneHz(3788678, 5000, true), 3789000);
    eq("no snap, 20 kHz span: 10 Hz steps", clickTuneHz(3788678, 20000, false), 3788680);
    eq("no snap, 100 kHz span: 100 Hz steps", clickTuneHz(3788678, 100000, false), 3788700);
    eq("no snap, 1 MHz span: 500 Hz steps", clickTuneHz(3788678, 1000000, false), 3788500);
    eq("invalid -> 0", clickTuneHz(-5, 100000, true), 0);
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
