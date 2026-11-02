// Unit test for click-to-tune (tune.h): scope click -> dial frequency per mode, snap 0.5 / 1 kHz / off.
// Built by CMake as test_tune_round; run: ctest or ./test_tune_round
#include "../tune.h"
#include <cstdio>
static int fails = 0;
static void eq(const char *what, double got, double want) {
    const bool ok = std::fabs(got - want) < 0.5;
    if (!ok) fails++;
    std::printf("%s %-58s got %.6f MHz, want %.6f MHz\n", ok ? "PASS" : "FAIL", what, got / 1e6, want / 1e6);
}
enum { LSB = 0, USB = 1, CWL = 2, CWU = 3, AM = 4, AMN = 5, FM = 6, FMN = 7, DATAL = 8, DATAU = 9, DFM = 10, RTTYL = 12, RTTYU = 13, PSK = 14 };
int main() {
    const double S = 100000, O = 1500;   // 100 kHz span, 1500 Hz offset
    // LSB: dial = click + 1.5 kHz
    eq("LSB 3.788678, snap 0.5k -> 3.790178 -> 3.790000", clickDialHz(3788678, LSB, O, S, 500), 3790000);
    eq("LSB 3.788678, snap 1k   -> 3.790000", clickDialHz(3788678, LSB, O, S, 1000), 3790000);
    eq("LSB 3.788678, no snap   -> 3.790200 (100 Hz step)", clickDialHz(3788678, LSB, O, S, 0), 3790200);
    eq("LSB 3.788200, snap 0.5k -> 3.789700 -> 3.789500", clickDialHz(3788200, LSB, O, S, 500), 3789500);
    eq("LSB 3.788200, snap 1k   -> 3.789700 -> 3.790000", clickDialHz(3788200, LSB, O, S, 1000), 3790000);
    eq("LSB centre of a signal on 3.840 (3.8385) -> 3.840", clickDialHz(3838500, LSB, O, S, 500), 3840000);
    eq("LSB 3.8386 (100 Hz off-centre) snap 0.5k -> 3.840", clickDialHz(3838600, LSB, O, S, 500), 3840000);
    eq("LSB exact half 3.78775 -> 3.78925 -> 3.7895 (rounds up)", clickDialHz(3787750, LSB, O, S, 500), 3789500);
    eq("DATA-L like LSB, snap 1k", clickDialHz(7071500, DATAL, O, S, 1000), 7073000);
    // USB: dial = click - 1.5 kHz
    eq("USB 14.2265 -> 14.225 (snap 0.5k)", clickDialHz(14226500, USB, O, S, 500), 14225000);
    eq("USB 14.226678, snap 0.5k -> 14.225178 -> 14.225", clickDialHz(14226678, USB, O, S, 500), 14225000);
    eq("USB 14.226678, snap 1k   -> 14.225", clickDialHz(14226678, USB, O, S, 1000), 14225000);
    eq("USB 14.226678, no snap   -> 14.225200", clickDialHz(14226678, USB, O, S, 0), 14225200);
    eq("USB 14.2268, snap 0.5k -> 14.2253 -> 14.2255", clickDialHz(14226800, USB, O, S, 500), 14225500);
    eq("DATA-U FT8 at 14.0755 -> dial 14.074 (snap 1k)", clickDialHz(14075500, DATAU, O, S, 1000), 14074000);
    eq("DATA-U no snap, 20 kHz span (10 Hz step)", clickDialHz(14075512, DATAU, O, 20000, 0), 14074010);
    // custom offset
    eq("LSB offset 1000 Hz: 3.8389 -> 3.8399 -> 3.840 (0.5k)", clickDialHz(3838900, LSB, 1000, S, 500), 3840000);
    eq("LSB offset 0 = old behaviour (3.788678 -> 3.789 1k)", clickDialHz(3788678, LSB, 0, S, 1000), 3789000);
    // direct modes
    eq("CW-U direct: 7.030340 -> 7.0305 (0.5k)", clickDialHz(7030340, CWU, O, S, 500), 7030500);
    eq("CW-L direct, no snap, 20 kHz span", clickDialHz(7030343, CWL, O, 20000, 0), 7030340);
    eq("AM direct: 3.885 stays (1k)", clickDialHz(3885200, AM, O, S, 1000), 3885000);
    eq("AM-N direct", clickDialHz(5000300, AMN, O, S, 500), 5000500);
    eq("FM direct: 29.6 (1k)", clickDialHz(29600400, FM, O, 1000000, 1000), 29600000);
    eq("FM-N direct", clickDialHz(29600400, FMN, O, 1000000, 0), 29600500);
    eq("DATA-FM direct", clickDialHz(29600400, DFM, O, 1000000, 1000), 29600000);
    eq("RTTY-L direct", clickDialHz(14080400, RTTYL, O, S, 500), 14080500);
    eq("PSK direct", clickDialHz(14070400, PSK, O, S, 1000), 14070000);
    eq("unknown mode (-1) direct", clickDialHz(3788678, -1, O, S, 1000), 3789000);
    eq("invalid click -> 0", clickDialHz(-5, LSB, O, S, 500), 0);
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
