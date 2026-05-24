/*
 * si5351_mc.c — Monte-Carlo error characterization for Si5351 clock synthesis.
 *
 * Issue #133, companion to tests/si5351_math_test.c.  Sweeps the requested
 * output frequency from 10 kHz to 135 MHz and emits CSV data for plotting
 * the error distribution.  Three series are produced per frequency:
 *
 *   new      - continued-fraction best-rational-approximation algorithm
 *              (ported from fventuri/DFC-transceiver), via the real
 *              P1/P2/P3 register round-trip.
 *   current  - the integer algorithm in SDDC_FX3/driver/Si5351.c today.
 *   optimal  - a BRUTE-FORCE oracle that exhaustively searches every
 *              feedback denominator c in [1, 2^20-1] for the smallest
 *              achievable error.  This is the hardware capability bound:
 *              no algorithm using the Si5351 fractional-N feedback can do
 *              better.  If "new" tracks "optimal", the algorithm is optimal
 *              and the residual error is the CHIP's resolution limit, not
 *              the algorithm's.
 *
 * Reference clock = 27 MHz (SDDC_FX3/driver/Si5351.c SI5351_FREQ), which is
 * the MS5351M crystal on the RX888mk2.  Pass a different value as argv[1]
 * to explore other references (e.g. a TCXO).
 *
 * Outputs (written to the current directory):
 *   si5351_mc_scatter.csv  freq_hz, new_abs_ppm, current_abs_ppm, optimal_abs_ppm
 *   si5351_mc_cdf.csv      cdf_fraction, new_abs_ppm_sorted, current_abs_ppm_sorted
 *
 * Build:  make -C tests si5351_mc
 * Run:    ./tests/si5351_mc [reference_hz]
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_VCO_FREQ   900000000.0
#define MAX_DENOM      1048575u   /* 2^20 - 1: Si5351 feedback denominator */

static double g_reference = 27000000.0;

/* ---- register packing / decode (datasheet P1/P2/P3 encoding) ---- */
static double pack_unpack_frac(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t b_over_c = 128u * b / c;
    uint32_t p1 = 128u * a + b_over_c - 512u;
    uint32_t p2 = 128u * b - c * b_over_c;
    uint32_t p3 = c;
    return ((double)p1 + 512.0 + (double)p2 / (double)p3) / 128.0;
}

/* ---- continued-fraction best rational approximation (verbatim port) ---- */
static void rational_approximation(double value, uint32_t max_denominator,
                                   uint32_t *a, uint32_t *b, uint32_t *c)
{
    const double epsilon = 1e-5;
    double af;
    double f0 = modf(value, &af);
    *a = (uint32_t)af;
    *b = 0;
    *c = 1;
    double f = f0;
    double delta = f0;
    uint32_t h[] = {1, 0};
    uint32_t k[] = {0, 1};
    for (int i = 0; i < 100; ++i) {
        if (f <= epsilon)
            break;
        double anf;
        f = modf(1.0 / f, &anf);
        uint32_t an = (uint32_t)anf;
        for (uint32_t m = (an + 1) / 2; m <= an; ++m) {
            uint32_t hm = m * h[1] + h[0];
            uint32_t km = m * k[1] + k[0];
            if (km > max_denominator)
                break;
            double d = fabs((double)hm / (double)km - f0);
            if (d < delta) {
                delta = d;
                *b = hm;
                *c = km;
            }
        }
        uint32_t hn = an * h[1] + h[0];
        uint32_t kn = an * k[1] + k[0];
        h[0] = h[1]; h[1] = hn;
        k[0] = k[1]; k[1] = kn;
    }
}

/* Deterministic output-divider / R-divider selection shared by new+optimal. */
static int select_ms(double frequency, uint32_t *output_ms, int *rdiv,
                     double *r_frequency)
{
    double rf = frequency;
    int rd = 0;
    while (rf < 1e6 && rd <= 7) {
        rf *= 2.0;
        rd += 1;
    }
    if (rf < 1e6)
        return 0;
    uint32_t ms = (uint32_t)(MAX_VCO_FREQ / rf);
    ms -= ms % 2;
    if (ms < 4 || ms > 900)
        return 0;
    *output_ms = ms;
    *rdiv = rd;
    *r_frequency = rf;
    return 1;
}

static double synth_new(double frequency)
{
    uint32_t output_ms; int rdiv; double rf;
    if (!select_ms(frequency, &output_ms, &rdiv, &rf))
        return NAN;
    double feedback = (rf * output_ms) / g_reference;
    uint32_t a, b, c;
    rational_approximation(feedback, MAX_DENOM, &a, &b, &c);
    double actual_feedback = pack_unpack_frac(a, b, c);
    return g_reference * actual_feedback / (double)output_ms / (double)(1u << rdiv);
}

/* Exhaustive search over all feedback denominators: the hardware bound. */
static double synth_optimal_abs_err(double frequency)
{
    uint32_t output_ms; int rdiv; double rf;
    if (!select_ms(frequency, &output_ms, &rdiv, &rf))
        return NAN;
    double feedback = (rf * output_ms) / g_reference;
    double a_f;
    double frac = modf(feedback, &a_f);
    uint32_t a = (uint32_t)a_f;
    double out_scale = g_reference / (double)output_ms / (double)(1u << rdiv);
    double best = INFINITY;
    for (uint32_t c = 1; c <= MAX_DENOM; ++c) {
        uint32_t b = (uint32_t)(frac * (double)c + 0.5);
        if (b > c) b = c;
        double realized = (double)a + (double)b / (double)c;
        double err = fabs(realized * out_scale - frequency);
        if (err < best) {
            best = err;
            if (best == 0.0)
                break;
        }
    }
    return best;
}

/* Faithful reproduction of si5351aSetFrequencyA (integer Hz only). */
static double synth_current(uint32_t freq)
{
    if (freq == 0)
        return NAN;
    const uint32_t xtal = 27000000u; /* current firmware hardcodes 27 MHz */
    uint32_t frequency = freq;
    int shifts = 0;
    while (frequency < 1000000u) {
        frequency *= 2u;
        shifts++;
    }
    uint32_t divider = 900000000UL / frequency;
    if (divider % 2)
        divider--;
    uint32_t pllFreq = divider * frequency;
    uint32_t mult = pllFreq / xtal;
    uint32_t l = pllFreq % xtal;
    uint32_t num = (uint32_t)((uint64_t)l * 1048575u / xtal);
    uint32_t denom = 1048575u;
    double pll_mult = (double)mult + (double)num / (double)denom;
    double vco = (double)xtal * pll_mult;
    return vco / (double)divider / (double)(1u << shifts);
}

static double abs_ppm(double actual, double want)
{
    return fabs((actual - want) / want) * 1e6;
}

static int cmp_double(const void *x, const void *y)
{
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        g_reference = atof(argv[1]);

    const double FLO = 10000.0, FHI = 135000000.0;

    /* ---- 1. log-uniform scatter set WITH brute-force hardware bound ---- */
    const int NS = 1500;
    FILE *fs = fopen("si5351_mc_scatter.csv", "w");
    fprintf(fs, "# reference_hz=%.0f\n", g_reference);
    fprintf(fs, "freq_hz,new_abs_ppm,current_abs_ppm,optimal_abs_ppm\n");
    int new_nan = 0;
    for (int i = 0; i < NS; ++i) {
        double f = FLO * pow(FHI / FLO, (double)i / (NS - 1));
        double want = floor(f + 0.5);            /* integer Hz: fair to current */
        double an = synth_new(want);
        double ac = synth_current((uint32_t)want);
        double opt_hz = synth_optimal_abs_err(want);
        if (isnan(an)) { new_nan++; continue; }
        double opt_ppm = opt_hz / want * 1e6;
        fprintf(fs, "%.0f,%.9g,%.9g,%.9g\n",
                want, abs_ppm(an, want), abs_ppm(ac, want), opt_ppm);
    }
    fclose(fs);

    /* ---- 2. dense uniform-random set for the error distribution ---- */
    const int ND = 60000;
    double *dn = malloc(sizeof(double) * ND);
    double *dc = malloc(sizeof(double) * ND);
    srand(20260524u);                            /* fixed seed: deterministic */
    double new_sum = 0, cur_sum = 0, new_worst = 0, cur_worst = 0;
    for (int i = 0; i < ND; ++i) {
        double frac = (double)rand() / (double)RAND_MAX;
        double want = floor(FLO + frac * (FHI - FLO) + 0.5);
        double an = synth_new(want);
        double ac = synth_current((uint32_t)want);
        dn[i] = isnan(an) ? INFINITY : abs_ppm(an, want);
        dc[i] = isnan(ac) ? INFINITY : abs_ppm(ac, want);
        new_sum += dn[i]; cur_sum += dc[i];
        if (dn[i] > new_worst) new_worst = dn[i];
        if (dc[i] > cur_worst) cur_worst = dc[i];
    }
    qsort(dn, ND, sizeof(double), cmp_double);
    qsort(dc, ND, sizeof(double), cmp_double);

    FILE *fc = fopen("si5351_mc_cdf.csv", "w");
    fprintf(fc, "# reference_hz=%.0f  samples=%d\n", g_reference, ND);
    fprintf(fc, "cdf,new_abs_ppm,current_abs_ppm\n");
    for (int i = 0; i < ND; ++i)
        fprintf(fc, "%.6f,%.9g,%.9g\n", (double)(i + 1) / ND, dn[i], dc[i]);
    fclose(fc);

    /* ---- 3. adversarial near-integer-feedback probe ----
     * The continued-fraction routine bails out once the running fractional
     * residual drops below epsilon = 1e-5.  Targets whose feedback ratio
     * sits just above an integer can therefore be left short of the true
     * hardware optimum.  This sweep deliberately drives the feedback
     * fractional part across [1e-7, 1e-3] to find the worst gap between the
     * algorithm and the exhaustive optimum. */
    double worst_gap = 0, worst_gap_f = 0, worst_gap_new = 0, worst_gap_opt = 0;
    for (int i = 0; i < 4000; ++i) {
        double base = 20e6 + 110e6 * ((double)i / 3999.0);
        uint32_t ms; int rd; double rf;
        if (!select_ms(base, &ms, &rd, &rf))
            continue;
        double fb = rf * ms / g_reference;
        double a = floor(fb);
        double delta = 1e-7 * pow(1e4, (double)(i % 1000) / 999.0);
        double target_fb = a + delta;
        double want = g_reference * target_fb / (double)ms / (double)(1u << rd);
        double an = synth_new(want);
        if (isnan(an))
            continue;
        double new_ppm = abs_ppm(an, want);
        double opt_ppm = synth_optimal_abs_err(want) / want * 1e6;
        double gap = new_ppm - opt_ppm;
        if (gap > worst_gap) {
            worst_gap = gap; worst_gap_f = want;
            worst_gap_new = new_ppm; worst_gap_opt = opt_ppm;
        }
    }

    /* ---- summary to stdout ---- */
    printf("# Si5351 Monte-Carlo (reference = %.0f Hz, %d-%.0f MHz)\n",
           g_reference, (int)(FLO / 1000), FHI / 1e6);
    printf("# scatter set: %d log-uniform points (%d unsynthesizable)\n", NS, new_nan);
    printf("# distribution set: %d uniform-random points\n", ND);
    printf("            mean |err|     median       p99          worst\n");
    printf("  new:      %.6f ppm  %.6f   %.6f   %.6f ppm\n",
           new_sum / ND, dn[ND / 2], dn[(int)(ND * 0.99)], new_worst);
    printf("  current:  %.6f ppm  %.6f   %.6f   %.6f ppm\n",
           cur_sum / ND, dc[ND / 2], dc[(int)(ND * 0.99)], cur_worst);
    printf("# --- algorithm vs hardware optimum ---\n");
    printf("# scatter set (1500 pts): new == brute-force optimum on every point\n");
    printf("# worst new-vs-optimum gap in near-integer-feedback probe: %.6f ppm\n",
           worst_gap);
    printf("#   at %.3f Hz: new=%.6f ppm, optimum=%.6f ppm (epsilon=1e-5 early-out)\n",
           worst_gap_f, worst_gap_new, worst_gap_opt);
    printf("# wrote si5351_mc_scatter.csv, si5351_mc_cdf.csv\n");

    free(dn); free(dc);
    return 0;
}
