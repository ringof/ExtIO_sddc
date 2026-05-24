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

/* ---- exact integer/rational best approximation (continued fraction +
 *      semiconvergent), no floating point.  Models the approach used by
 *      ka9q-radio's solver.  Approximates num/den (0 <= num < den) by b/c
 *      with c <= max_den, choosing the closest fraction.  Implemented from
 *      the standard CF/semiconvergent method. ---- */
static uint64_t gcd_u64(uint64_t a, uint64_t b)
{
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

static void best_rational_exact(uint64_t num, uint64_t den, uint64_t max_den,
                                uint64_t *out_b, uint64_t *out_c)
{
    if (num == 0) { *out_b = 0; *out_c = 1; return; }
    uint64_t h2 = 0, h1 = 1;   /* numerators of convergents */
    uint64_t k2 = 1, k1 = 0;   /* denominators of convergents */
    uint64_t a = num, b = den;
    while (b != 0) {
        uint64_t ai = a / b, rem = a % b;
        uint64_t h = ai * h1 + h2;
        uint64_t k = ai * k1 + k2;
        if (k > max_den) {
            if (k1 == 0) break;
            uint64_t t = (max_den - k2) / k1;        /* largest legal semiconvergent */
            uint64_t hs = t * h1 + h2;
            uint64_t ks = t * k1 + k2;
            /* pick whichever of the last convergent (h1/k1) or the
             * semiconvergent (hs/ks) is closer to num/den */
            __int128 d_prev = (__int128)num * (__int128)k1 - (__int128)h1 * (__int128)den;
            if (d_prev < 0) d_prev = -d_prev;
            __int128 d_semi = (__int128)num * (__int128)ks - (__int128)hs * (__int128)den;
            if (d_semi < 0) d_semi = -d_semi;
            /* compare d_prev/k1 vs d_semi/ks */
            if ((__int128)d_prev * (__int128)ks <= (__int128)d_semi * (__int128)k1) {
                *out_b = h1; *out_c = k1;
            } else {
                *out_b = hs; *out_c = ks;
            }
            return;
        }
        h2 = h1; h1 = h;
        k2 = k1; k1 = k;
        a = b; b = rem;
    }
    *out_b = h1; *out_c = k1;
}

/* Solve target feedback ratio P/Q (exact) into a + b/c (c <= MAX_DENOM). */
static void solve_feedback_exact(uint64_t P, uint64_t Q,
                                 uint64_t *a, uint64_t *b, uint64_t *c)
{
    *a = P / Q;
    uint64_t rem = P % Q;
    if (rem == 0) { *b = 0; *c = 1; return; }
    uint64_t g = gcd_u64(rem, Q);
    best_rational_exact(rem / g, Q / g, MAX_DENOM, b, c);
}

static int select_ms(double frequency, uint32_t *output_ms, int *rdiv,
                     double *r_frequency);   /* defined below */

/* Exact-rational synthesis, single (auto) divider — apples-to-apples with
 * synth_new.  Reports chosen feedback denominator c and integer-PLL flag. */
static double synth_exact(double frequency, uint64_t *out_c, int *out_intpll)
{
    uint32_t output_ms; int rdiv; double rf;
    if (!select_ms(frequency, &output_ms, &rdiv, &rf)) {
        if (out_c) *out_c = 0;
        if (out_intpll) *out_intpll = 0;
        return NAN;
    }
    uint64_t W = (uint64_t)(frequency + 0.5);          /* integer Hz target */
    uint64_t P = W * (1ull << rdiv) * (uint64_t)output_ms;
    uint64_t Q = (uint64_t)g_reference;
    uint64_t a, b, c;
    solve_feedback_exact(P, Q, &a, &b, &c);
    if (out_c) *out_c = c;
    if (out_intpll) *out_intpll = (b == 0);
    double fb = pack_unpack_frac((uint32_t)a, (uint32_t)b, (uint32_t)c);
    return g_reference * fb / (double)output_ms / (double)(1u << rdiv);
}

/* Spur-aware: search even output dividers keeping the VCO in [600,900] MHz,
 * prefer (smallest error, then integer-PLL, then smallest denominator).
 * Models ka9q's spur-aware candidate scoring. */
static double synth_spuraware(double frequency, uint64_t *out_c, int *out_intpll)
{
    double rf = frequency; int rdiv = 0;
    while (rf < 1e6 && rdiv <= 7) { rf *= 2.0; rdiv += 1; }
    if (rf < 1e6) {
        if (out_c) *out_c = 0;
        if (out_intpll) *out_intpll = 0;
        return NAN;
    }

    uint64_t W = (uint64_t)(frequency + 0.5);
    uint64_t Q = (uint64_t)g_reference;
    uint32_t ms_lo = (uint32_t)ceil(600e6 / rf);
    uint32_t ms_hi = (uint32_t)floor(900e6 / rf);
    if (ms_lo < 6) ms_lo = 6;
    if (ms_hi > 900) ms_hi = 900;

    double best_real = NAN, best_err = INFINITY;
    int best_rank = 999; uint64_t best_c = 0; int best_int = 0;
    for (uint32_t ms = ms_lo + (ms_lo & 1); ms <= ms_hi; ms += 2) {   /* even only */
        uint64_t P = W * (1ull << rdiv) * (uint64_t)ms;
        uint64_t a, b, c;
        solve_feedback_exact(P, Q, &a, &b, &c);
        double fb = pack_unpack_frac((uint32_t)a, (uint32_t)b, (uint32_t)c);
        double real = g_reference * fb / (double)ms / (double)(1u << rdiv);
        double err = fabs(real - frequency);
        int rank = (b == 0) ? 0 : (c <= 1000 ? 1 : (c <= 100000 ? 2 : 3));
        if (err < best_err - 1e-6 ||
            (fabs(err - best_err) <= 1e-6 && rank < best_rank)) {
            best_err = err; best_rank = rank;
            best_real = real; best_c = c; best_int = (b == 0);
        }
    }
    if (out_c) *out_c = best_c;
    if (out_intpll) *out_intpll = best_int;
    return best_real;
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

static int cmp_u64(const void *x, const void *y)
{
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return (a > b) - (a < b);
}

/* Exhaustive best error (in ratio units) approximating fractional target
 * frac (0<frac<1) by b/c with c <= MAX_DENOM.  Hardware bound at the
 * feedback-ratio level. */
static double opt_ratio_err(double frac)
{
    double best = INFINITY;
    for (uint32_t c = 1; c <= MAX_DENOM; ++c) {
        uint32_t b = (uint32_t)(frac * (double)c + 0.5);
        if (b > c) b = c;
        double e = fabs((double)b / (double)c - frac);
        if (e < best) { best = e; if (best == 0.0) break; }
    }
    return best;
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
    fprintf(fs, "freq_hz,new_abs_ppm,current_abs_ppm,optimal_abs_ppm,exact_abs_ppm\n");
    int new_nan = 0;
    for (int i = 0; i < NS; ++i) {
        double f = FLO * pow(FHI / FLO, (double)i / (NS - 1));
        double want = floor(f + 0.5);            /* integer Hz: fair to current */
        double an = synth_new(want);
        double ac = synth_current((uint32_t)want);
        double ae = synth_exact(want, NULL, NULL);
        double opt_hz = synth_optimal_abs_err(want);
        if (isnan(an)) { new_nan++; continue; }
        double opt_ppm = opt_hz / want * 1e6;
        fprintf(fs, "%.0f,%.9g,%.9g,%.9g,%.9g\n",
                want, abs_ppm(an, want), abs_ppm(ac, want), opt_ppm,
                abs_ppm(ae, want));
    }
    fclose(fs);

    /* ---- 2. dense uniform-random set: error distribution + denominator
     *        / integer-PLL distribution (single divider vs spur-aware) ---- */
    const int ND = 60000;
    double *dn = malloc(sizeof(double) * ND);   /* new (float CF) abs ppm */
    double *dc = malloc(sizeof(double) * ND);   /* current integer abs ppm */
    double *de = malloc(sizeof(double) * ND);   /* exact rational abs ppm */
    uint64_t *cs = malloc(sizeof(uint64_t) * ND); /* chosen denom, single divider */
    uint64_t *cw = malloc(sizeof(uint64_t) * ND); /* chosen denom, spur-aware */
    srand(20260524u);                            /* fixed seed: deterministic */
    double new_sum = 0, cur_sum = 0, exa_sum = 0;
    double new_worst = 0, cur_worst = 0, exa_worst = 0;
    int intpll_single = 0, intpll_spur = 0;
    for (int i = 0; i < ND; ++i) {
        double frac = (double)rand() / (double)RAND_MAX;
        double want = floor(FLO + frac * (FHI - FLO) + 0.5);
        double an = synth_new(want);
        double ac = synth_current((uint32_t)want);
        uint64_t c_s, c_w; int ip_s, ip_w;
        double ae = synth_exact(want, &c_s, &ip_s);
        (void)synth_spuraware(want, &c_w, &ip_w);
        dn[i] = isnan(an) ? INFINITY : abs_ppm(an, want);
        dc[i] = isnan(ac) ? INFINITY : abs_ppm(ac, want);
        de[i] = isnan(ae) ? INFINITY : abs_ppm(ae, want);
        cs[i] = c_s; cw[i] = c_w;
        intpll_single += ip_s; intpll_spur += ip_w;
        new_sum += dn[i]; cur_sum += dc[i]; exa_sum += de[i];
        if (dn[i] > new_worst) new_worst = dn[i];
        if (dc[i] > cur_worst) cur_worst = dc[i];
        if (de[i] > exa_worst) exa_worst = de[i];
    }
    qsort(dn, ND, sizeof(double), cmp_double);
    qsort(dc, ND, sizeof(double), cmp_double);
    qsort(de, ND, sizeof(double), cmp_double);
    qsort(cs, ND, sizeof(uint64_t), cmp_u64);
    qsort(cw, ND, sizeof(uint64_t), cmp_u64);

    FILE *fc = fopen("si5351_mc_cdf.csv", "w");
    fprintf(fc, "# reference_hz=%.0f  samples=%d\n", g_reference, ND);
    fprintf(fc, "cdf,new_abs_ppm,current_abs_ppm,exact_abs_ppm\n");
    for (int i = 0; i < ND; ++i)
        fprintf(fc, "%.6f,%.9g,%.9g,%.9g\n",
                (double)(i + 1) / ND, dn[i], dc[i], de[i]);
    fclose(fc);

    FILE *fd = fopen("si5351_mc_cdist.csv", "w");
    fprintf(fd, "# chosen feedback denominator c: single divider vs spur-aware\n");
    fprintf(fd, "cdf,single_c,spuraware_c\n");
    for (int i = 0; i < ND; ++i)
        fprintf(fd, "%.6f,%llu,%llu\n", (double)(i + 1) / ND,
                (unsigned long long)cs[i], (unsigned long long)cw[i]);
    fclose(fd);

    /* ---- 3. feedback-level epsilon probe: float CF vs exact vs optimum ----
     * Drive the fractional part of the PLL feedback ratio from 1e-8 to 1e-2
     * and measure the approximation error of each method against the
     * exhaustive optimum.  Isolates the float routine's epsilon=1e-5
     * early-out from the exact-rational routine. */
    FILE *fe = fopen("si5351_mc_epsilon.csv", "w");
    fprintf(fe, "# feedback fractional part vs approximation error (a=33)\n");
    fprintf(fe, "delta,new_ppm,exact_ppm,optimal_ppm\n");
    const uint64_t A_FIXED = 33;     /* VCO = 33*27 MHz = 891 MHz, in range */
    double new_probe_worst = 0, exact_probe_worst = 0;
    const int NP = 240;
    for (int i = 0; i < NP; ++i) {
        double lg = -8.0 + 6.0 * ((double)i / (NP - 1));   /* delta 1e-8..1e-2 */
        uint64_t M = (uint64_t)(pow(10.0, -lg) + 0.5);
        if (M < 2) M = 2;
        double delta = 1.0 / (double)M;
        double target = (double)A_FIXED + delta;

        /* float CF */
        uint32_t na, nb, nc;
        rational_approximation(target, MAX_DENOM, &na, &nb, &nc);
        double new_ratio = pack_unpack_frac(na, nb, nc);
        double new_ppm = fabs(new_ratio - target) / target * 1e6;

        /* exact rational */
        uint64_t ea, eb, ec;
        solve_feedback_exact(A_FIXED * M + 1, M, &ea, &eb, &ec);
        double exact_ratio = pack_unpack_frac((uint32_t)ea, (uint32_t)eb, (uint32_t)ec);
        double exact_ppm = fabs(exact_ratio - target) / target * 1e6;

        /* exhaustive optimum */
        double opt_ppm = opt_ratio_err(delta) / target * 1e6;

        fprintf(fe, "%.9g,%.9g,%.9g,%.9g\n", delta, new_ppm, exact_ppm, opt_ppm);
        if (new_ppm - opt_ppm > new_probe_worst) new_probe_worst = new_ppm - opt_ppm;
        if (exact_ppm - opt_ppm > exact_probe_worst) exact_probe_worst = exact_ppm - opt_ppm;
    }
    fclose(fe);

    /* ---- summary to stdout ---- */
    printf("# Si5351 Monte-Carlo (reference = %.0f Hz, %d-%.0f MHz)\n",
           g_reference, (int)(FLO / 1000), FHI / 1e6);
    printf("# scatter set: %d log-uniform points (%d unsynthesizable)\n", NS, new_nan);
    printf("# distribution set: %d uniform-random points\n", ND);
    printf("                  mean |err|     median       p99          worst\n");
    printf("  new (float CF): %.6f ppm  %.6f   %.6f   %.6f ppm\n",
           new_sum / ND, dn[ND / 2], dn[(int)(ND * 0.99)], new_worst);
    printf("  exact rational: %.6f ppm  %.6f   %.6f   %.6f ppm\n",
           exa_sum / ND, de[ND / 2], de[(int)(ND * 0.99)], exa_worst);
    printf("  current integer:%.6f ppm  %.6f   %.6f   %.6f ppm\n",
           cur_sum / ND, dc[ND / 2], dc[(int)(ND * 0.99)], cur_worst);
    printf("# --- algorithm vs hardware optimum (feedback epsilon probe) ---\n");
    printf("# worst float-CF gap vs optimum:  %.6f ppm (epsilon=1e-5 early-out)\n",
           new_probe_worst);
    printf("# worst exact-rational gap vs optimum: %.9f ppm\n", exact_probe_worst);
    printf("# --- spur preference (chosen feedback denominator c) ---\n");
    printf("# integer-PLL (b==0) rate:  single divider %.1f%%   spur-aware %.1f%%\n",
           100.0 * intpll_single / ND, 100.0 * intpll_spur / ND);
    printf("# median c:                 single divider %llu        spur-aware %llu\n",
           (unsigned long long)cs[ND / 2], (unsigned long long)cw[ND / 2]);
    printf("# p90 c:                    single divider %llu   spur-aware %llu\n",
           (unsigned long long)cs[(int)(ND * 0.90)], (unsigned long long)cw[(int)(ND * 0.90)]);
    printf("# wrote si5351_mc_scatter.csv, si5351_mc_cdf.csv, "
           "si5351_mc_cdist.csv, si5351_mc_epsilon.csv\n");

    free(dn); free(dc); free(de); free(cs); free(cw);
    return 0;
}
