// FT-710 Remote RX gain + lookahead peak limiter: a tiny LADSPA plugin the network-audio receiver
// loads into a PipeWire filter-chain in front of "FT-710 RX (network)". Gain is applied first, then a
// brick-wall lookahead limiter keeps every output sample at or below the ceiling (default -1 dBFS),
// so no gain setting can clip what WSJT-X, fldigi or the listen monitor get.
//
// Limiter: required gain per sample = min(1, ceiling / |x * gain|); a sliding minimum over the
// lookahead window (L samples), a release smoother that can only rise slowly, then an L-sample moving
// average. Every value in the averaging window is <= the required gain of the sample leaving the delay
// line, so the applied gain never exceeds what that sample needs (plus a final safety clamp).
// The gain control is smoothed (20 ms), so live changes (pw-cli set-param) don't click.
//
// SPDX-License-Identifier: Apache-2.0
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Minimal LADSPA 1.1 ABI (same layout as ladspa.h, which Fedora doesn't ship by default)
typedef float LADSPA_Data;
typedef void *LADSPA_Handle;
typedef int LADSPA_Properties;
typedef int LADSPA_PortDescriptor;
typedef int LADSPA_PortRangeHintDescriptor;
typedef struct { LADSPA_PortRangeHintDescriptor HintDescriptor; LADSPA_Data LowerBound, UpperBound; } LADSPA_PortRangeHint;
typedef struct _LADSPA_Descriptor {
    unsigned long UniqueID; const char *Label; LADSPA_Properties Properties; const char *Name, *Maker, *Copyright;
    unsigned long PortCount; const LADSPA_PortDescriptor *PortDescriptors; const char *const *PortNames;
    const LADSPA_PortRangeHint *PortRangeHints; void *ImplementationData;
    LADSPA_Handle (*instantiate)(const struct _LADSPA_Descriptor *, unsigned long);
    void (*connect_port)(LADSPA_Handle, unsigned long, LADSPA_Data *);
    void (*activate)(LADSPA_Handle);
    void (*run)(LADSPA_Handle, unsigned long);
    void (*run_adding)(LADSPA_Handle, unsigned long);
    void (*set_run_adding_gain)(LADSPA_Handle, LADSPA_Data);
    void (*deactivate)(LADSPA_Handle);
    void (*cleanup)(LADSPA_Handle);
} LADSPA_Descriptor;
#define LADSPA_PROPERTY_HARD_RT_CAPABLE 0x4
#define LADSPA_PORT_INPUT 0x1
#define LADSPA_PORT_OUTPUT 0x2
#define LADSPA_PORT_CONTROL 0x4
#define LADSPA_PORT_AUDIO 0x8
#define LADSPA_HINT_BOUNDED_BELOW 0x1
#define LADSPA_HINT_BOUNDED_ABOVE 0x2
#define LADSPA_HINT_DEFAULT_MIDDLE 0x100
#define LADSPA_HINT_DEFAULT_HIGH 0x140
#define LADSPA_HINT_DEFAULT_MAXIMUM 0x180   /* unused */
#define LADSPA_HINT_DEFAULT_LOW 0xC0
#define LADSPA_HINT_DEFAULT_0 0x200
#define LADSPA_HINT_DEFAULT_1 0x240

enum { P_IN, P_OUT, P_GAIN, P_CEIL, P_RELEASE, P_LATENCY, P_GR, P_COUNT };
#define LOOKAHEAD_S 0.003

typedef struct {
    LADSPA_Data *port[P_COUNT];
    unsigned long L;            // lookahead / delay in samples
    float *delay;               // L samples of gained input
    float *req;                 // L+1 required gains (ring), for the sliding minimum
    unsigned long *dq; unsigned long dqHead, dqLen;   // monotonic deque of indices into req (by absolute sample)
    float *avg; double avgSum;  // L smoothed gains (ring) for the moving average
    unsigned long n;            // absolute sample counter
    double sr, gain, rel, gr;
} Lim;

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long sr) {
    (void)d;
    Lim *l = calloc(1, sizeof(Lim));
    if (!l) return NULL;
    l->sr = (double)sr;
    l->L = (unsigned long)(LOOKAHEAD_S * sr + 0.5); if (l->L < 1) l->L = 1;
    l->delay = calloc(l->L, sizeof(float)); l->req = calloc(l->L + 1, sizeof(float));
    l->dq = calloc(l->L + 1, sizeof(unsigned long)); l->avg = calloc(l->L, sizeof(float));
    if (!l->delay || !l->req || !l->dq || !l->avg) { free(l->delay); free(l->req); free(l->dq); free(l->avg); free(l); return NULL; }
    return l;
}
static void connect_port(LADSPA_Handle h, unsigned long p, LADSPA_Data *d) { if (p < P_COUNT) ((Lim *)h)->port[p] = d; }
static void activate(LADSPA_Handle h) {
    Lim *l = h;
    memset(l->delay, 0, l->L * sizeof(float));
    for (unsigned long i = 0; i < l->L; i++) l->avg[i] = 1.f;
    l->avgSum = (double)l->L; l->dqHead = l->dqLen = 0; l->n = 0; l->rel = 1.0; l->gr = 0;
    l->gain = l->port[P_GAIN] ? *l->port[P_GAIN] : 3.0;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void run(LADSPA_Handle h, unsigned long ns) {
    Lim *l = h;
    const float *in = l->port[P_IN]; float *out = l->port[P_OUT];
    const double target = clampf(l->port[P_GAIN] ? *l->port[P_GAIN] : 3.f, 0.f, 100.f);
    const double cdb = clampf(l->port[P_CEIL] ? *l->port[P_CEIL] : -1.f, -30.f, 0.f);
    const float ceil = (float)pow(10.0, cdb / 20.0);
    const double relMs = clampf(l->port[P_RELEASE] ? *l->port[P_RELEASE] : 150.f, 5.f, 5000.f);
    const double kg = 1.0 - exp(-1.0 / (0.020 * l->sr)), kr = 1.0 - exp(-1.0 / (relMs * 0.001 * l->sr));
    const unsigned long L = l->L, R = L + 1;
    double minGr = 1.0;
    if (l->port[P_LATENCY]) *l->port[P_LATENCY] = (LADSPA_Data)L;
    if (!in || !out) return;
    for (unsigned long i = 0; i < ns; i++) {
        l->gain += (target - l->gain) * kg;
        float x = in[i];
        if (!isfinite(x)) x = 0.f;
        const float y = x * (float)l->gain, a = fabsf(y);
        const float rq = a > ceil ? ceil / a : 1.f;
        // sliding minimum of rq over the last L+1 samples (monotonic deque)
        const unsigned long n = l->n;
        l->req[n % R] = rq;
        while (l->dqLen && l->req[l->dq[(l->dqHead + l->dqLen - 1) % R] % R] >= rq) l->dqLen--;
        l->dq[(l->dqHead + l->dqLen) % R] = n; l->dqLen++;
        while (n - l->dq[l->dqHead % R] > L) { l->dqHead = (l->dqHead + 1) % R; l->dqLen--; }
        const float held = l->req[l->dq[l->dqHead % R] % R];
        // release: fall at once, rise slowly; never above held
        if (held < l->rel) l->rel = held; else l->rel += (held - l->rel) * kr;
        // moving average over L samples
        const unsigned long ai = n % L;
        l->avgSum += l->rel - l->avg[ai]; l->avg[ai] = (float)l->rel;
        double g = l->avgSum / (double)L; if (g > 1.0) g = 1.0;
        // delayed sample out
        const float dly = l->delay[ai]; l->delay[ai] = y;
#ifdef FT710_NO_SAFETY_CLAMP   /* test build: prove the limiter alone holds the ceiling */
        out[i] = dly * (float)g;
#else
        out[i] = clampf(dly * (float)g, -ceil, ceil);
#endif
        if (g < minGr) minGr = g;
        l->n = n + 1;
        if ((l->n & 0xFFFF) == 0) { double s = 0; for (unsigned long k = 0; k < L; k++) s += l->avg[k]; l->avgSum = s; }   // no drift
    }
    l->gr = minGr < 1.0 ? -20.0 * log10(minGr) : 0.0;
    if (l->port[P_GR]) *l->port[P_GR] = (LADSPA_Data)l->gr;
}
static void cleanup(LADSPA_Handle h) { Lim *l = h; free(l->delay); free(l->req); free(l->dq); free(l->avg); free(l); }

static const LADSPA_PortDescriptor pd[P_COUNT] = {
    LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO, LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL };
static const char *const pn[P_COUNT] = { "In", "Out", "Gain", "Ceiling", "Release", "latency", "Reduction" };
static const LADSPA_PortRangeHint ph[P_COUNT] = {
    {0, 0, 0}, {0, 0, 0},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.f, 20.f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_HIGH, -30.f, 0.f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 5.f, 5000.f},
    {0, 0, 0}, {0, 0, 0} };
static const LADSPA_Descriptor desc = {
    0x46543731UL, "ft710_rxlimit", LADSPA_PROPERTY_HARD_RT_CAPABLE, "FT-710 Remote RX gain + lookahead limiter",
    "FT-710_remote", "Apache-2.0", P_COUNT, pd, pn, ph, NULL,
    instantiate, connect_port, activate, run, NULL, NULL, NULL, cleanup };
const LADSPA_Descriptor *ladspa_descriptor(unsigned long i) { return i == 0 ? &desc : NULL; }
