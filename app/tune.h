// Click-to-tune: scope click -> VFO dial frequency. Header-only so app/tests can test it on its own.
#pragma once
#include <cmath>

// FT-710 mode byte (scope frame meta[60]; same codes as CAT MD):
// 0 LSB 1 USB 2 CW-L 3 CW-U 4 AM 5 AM-N 6 FM 7 FM-N 8 DATA-L 9 DATA-U A DATA-FM B DATA-FM-N C RTTY-L D RTTY-U E PSK
// Which side of the dial (suppressed carrier) the received audio lies on:
// -1 below (LSB, DATA-L), +1 above (USB, DATA-U), 0 = the click is the dial frequency
// (CW: the FT-710 shows the received carrier frequency, so a click on a CW signal is already right;
// AM/FM: carrier; RTTY/PSK and unknown: left direct).
inline int tuneSide(int mode) {
    switch (mode) {
        case 0: case 8: return -1;
        case 1: case 9: return +1;
        default: return 0;
    }
}
inline const char *tuneSideName(int mode) {
    switch (mode) {
        case 0: return "LSB"; case 8: return "DATA-L"; case 1: return "USB"; case 9: return "DATA-U";
        default: return "";
    }
}

// clickedHz: frequency under the mouse (the middle of the signal you want);
// mode: FT-710 mode byte; offsetHz: where the middle of the signal should sit in the passband
// (1500 Hz = middle of 300-2700 Hz), so LSB dial = click + offset, USB dial = click - offset;
// snapHz > 0: round the resulting dial frequency to that step (500 or 1000 Hz);
// snapHz <= 0: a fine step that suits the scope span (10 / 100 / 500 Hz).
inline double clickDialHz(double clickedHz, int mode, double offsetHz, double span, double snapHz) {
    if (!(clickedHz > 0) || !std::isfinite(clickedHz)) return 0;
    const double dial = clickedHz - tuneSide(mode) * offsetHz;
    const double res = snapHz > 0 ? snapHz : span <= 20000 ? 10 : span <= 200000 ? 100 : 500;
    // round in whole Hz first, so float error on an exact half (3788499.9999999995) still rounds up
    const double r = std::floor(std::round(dial) / res + 0.5) * res;
    return r > 0 ? r : 0;
}
