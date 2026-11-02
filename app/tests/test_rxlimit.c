// Offline test of the RX gain + limiter LADSPA plugin: full-scale and nasty signals at the maximum
// gain must never exceed the ceiling, and quiet signals must get the full gain.
// Build + run: app/tests/test_rxlimit.sh [path/to/ft710_rxlimit.so]
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
typedef float D;
typedef struct _Desc {
    unsigned long id; const char *label; int props; const char *name, *maker, *copy; unsigned long pc;
    const int *pd; const char *const *pn; const void *ph; void *impl;
    void *(*instantiate)(const struct _Desc *, unsigned long); void (*connect)(void *, unsigned long, D *);
    void (*activate)(void *); void (*run)(void *, unsigned long); void *ra, *srg; void (*deactivate)(void *); void (*cleanup)(void *);
} Desc;
static int fails = 0;
#define SR 48000
#define N (SR * 4)
static D in[N], out[N];
static double test(const Desc *d, const char *name, double gain, double ceilDb, int quantum, int stepAt, double gain2) {
    void *h = d->instantiate(d, SR);
    D g = (D)gain, c = (D)ceilDb, rel = 150, lat = 0, gr = 0;
    d->connect(h, 2, &g); d->connect(h, 3, &c); d->connect(h, 4, &rel); d->connect(h, 5, &lat); d->connect(h, 6, &gr);
    d->activate(h);
    double peak = 0;
    for (int o = 0; o < N; o += quantum) {
        int n = o + quantum > N ? N - o : quantum;
        if (stepAt && o >= stepAt) g = (D)gain2;
        d->connect(h, 0, in + o); d->connect(h, 1, out + o); d->run(h, n);
    }
    for (int i = 0; i < N; i++) if (fabs(out[i]) > peak) peak = fabs(out[i]);
    d->cleanup(h);
    const double pdb = peak > 0 ? 20 * log10(peak) : -200, lim = ceilDb + 1e-4;
    const int ok = pdb <= lim;
    if (!ok) fails++;
    printf("%s %-44s gain %5.2fx  peak %7.3f dBFS (ceiling %.1f)  latency %d samples\n", ok ? "PASS" : "FAIL", name, gain, pdb, ceilDb, (int)lat);
    return pdb;
}
int main(int argc, char **argv) {
    void *so = dlopen(argc > 1 ? argv[1] : "./ft710_rxlimit.so", RTLD_NOW);
    if (!so) { printf("dlopen: %s\n", dlerror()); return 2; }
    const Desc *(*get)(unsigned long) = (const Desc *(*)(unsigned long))dlsym(so, "ladspa_descriptor");
    const Desc *d = get(0);
    srand(1);
    const double gains[] = {0.5, 3.0, 10.0, 20.0};
    for (unsigned gi = 0; gi < sizeof gains / sizeof *gains; gi++) {
        double G = gains[gi];
        for (int i = 0; i < N; i++) in[i] = (D)sin(2 * M_PI * 1000 * i / SR);
        test(d, "0 dBFS 1 kHz sine", G, -1, 1024, 0, 0);
        for (int i = 0; i < N; i++) in[i] = (i / 24) % 2 ? 1.f : -1.f;
        test(d, "0 dBFS 1 kHz square", G, -1, 256, 0, 0);
        for (int i = 0; i < N; i++) in[i] = (D)(2.0 * rand() / RAND_MAX - 1.0);
        test(d, "full-scale white noise", G, -1, 333, 0, 0);
        for (int i = 0; i < N; i++) in[i] = (i % 4800 == 0) ? 1.f : (i % 4800 == 1 ? -1.f : 0.f);
        test(d, "full-scale clicks", G, -1, 64, 0, 0);
        for (int i = 0; i < N; i++) in[i] = (D)((i < N / 2 ? 0.01 : 1.0) * sin(2 * M_PI * 700 * i / SR));
        test(d, "-40 dBFS then 0 dBFS (sudden burst)", G, -1, 480, 0, 0);
    }
    for (int i = 0; i < N; i++) in[i] = (D)(0.9 * sin(2 * M_PI * 1500 * i / SR));
    test(d, "gain step 0.5x -> 10x while running", 0.5, -1, 1024, N / 2, 10.0);
    // quiet signal gets exactly the gain (no limiting below the ceiling)
    for (int i = 0; i < N; i++) in[i] = (D)(0.01 * sin(2 * M_PI * 1000 * i / SR));
    double p = test(d, "-40 dBFS sine (should be +20 dB, unlimited)", 10.0, -1, 1024, 0, 0);
    if (fabs(p - (-20.0)) > 0.05) { printf("FAIL quiet sine level %.3f dBFS, expected -20.0\n", p); fails++; } else printf("PASS quiet sine gets the full +20 dB\n");
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
