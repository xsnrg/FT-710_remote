// Click-to-tune frequency rounding (band scope). Header-only so app/tests can test it on its own.
#pragma once
#include <cmath>

// hz: frequency under the mouse; span: scope span in Hz; snapKHz: "Snap to kHz" checkbox.
// Snap on: nearest whole kHz (3.788678 MHz -> 3.789000, 3.7884 -> 3.788; exact halves round up).
// Snap off: a step that suits the span (10 / 100 / 500 Hz), as before.
inline double clickTuneHz(double hz, double span, bool snapKHz) {
    if (!(hz > 0) || !std::isfinite(hz)) return 0;
    const double res = snapKHz ? 1000 : span <= 20000 ? 10 : span <= 200000 ? 100 : 500;
    // round in whole Hz first, so a value like 3788499.9999999995 (float error on 3.7885 MHz) still snaps up
    const double whole = std::round(hz);
    return std::floor(whole / res + 0.5) * res;
}
