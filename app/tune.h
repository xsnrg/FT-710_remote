// Click-to-tune: scope click -> VFO dial frequency. Header-only so app/tests can test it on its own.
#pragma once
#include <cmath>
#include <cstdint>
#include <utility>

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

// Approximate receive passband relative to the dial (Hz) for a mode byte. Used for BOTH the grey stripe
// at the current dial and the hover preview at the dial a click would tune to, so they are identical.
// LSB/USB 300-2700 Hz: its middle is 1500 Hz from the dial = the default SSB click offset.
inline std::pair<double, double> passbandHz(int mode) {
    switch (mode) {
        case 0: case 8: return {-2700, -300};
        case 1: case 9: case 0xE: return {300, 2700};
        case 2: case 3: case 0xC: case 0xD: return {-250, 250};
        case 4: return {-3000, 3000};
        case 5: return {-2000, 2000};
        case 6: case 0xA: return {-6000, 6000};
        case 7: case 0xB: return {-3000, 3000};
        default: return {0, 0};
    }
}

// FT-710 tuning range (Hz): guards both stepDialHz() and the app's tuneTo().
inline constexpr double kMinDialHz = 30e3, kMaxDialHz = 75e6;

// Keyboard step tuning (Main's arrow keys): Left/Right coarse, Up/Down fine. The arrows move the
// dial itself, so mode and passband do not matter (like the VFO-UP/DOWN keys on the radio).
enum class TuneKey { None, Left, Right, Up, Down };
inline double keyStepHz(TuneKey k) {
    switch (k) {
        case TuneKey::Left:  return -5000;
        case TuneKey::Right: return  5000;
        case TuneKey::Up:    return   500;
        case TuneKey::Down:  return  -500;
        case TuneKey::None:  return  0;
    }
    return 0;
}
// New dial frequency (whole Hz) at dialHz + deltaHz, clamped to the FT-710's tuning range;
// 0 = the current dial is unknown/invalid, so the caller must refuse to tune.
inline double stepDialHz(double dialHz, double deltaHz, double loHz = kMinDialHz, double hiHz = kMaxDialHz) {
    if (!(dialHz > 0) || !std::isfinite(dialHz) || !(hiHz > loHz)) return 0;
    const double d = std::round(dialHz) + std::round(deltaHz);
    return d < loHz ? loHz : d > hiHz ? hiHz : d;
}

// Left edge of the scope (Hz). The FT-710 does NOT centre its scope on the dial in SSB: the frame's
// parameter block carries the real centre at meta[140:144] (big-endian Hz), e.g. dial 3.821000 LSB ->
// centre 3.819500 (meta[130:132] = 1500 = the offset). Assuming centre = dial drew every signal
// 1.5 kHz too high. Use the radio's centre when present and sane (within 10 kHz of the dial);
// otherwise (old/short meta, fixed mode) keep the start the streamer computed.
inline int64_t scopeStartHz(const unsigned char *meta, int metaLen, int64_t vfo, int64_t span, int64_t hdrStart, bool fixed) {
    if (fixed || !meta || metaLen < 144 || span <= 0 || vfo <= 0) return hdrStart;
    const int64_t c = (int64_t(meta[140]) << 24) | (int64_t(meta[141]) << 16) | (int64_t(meta[142]) << 8) | meta[143];
    if (c <= 0 || c - vfo > 10000 || vfo - c > 10000) return hdrStart;
    const int64_t st = c - span / 2;
    return st > 0 ? st : 0;
}
