/*
 * si5351_math_test.c — host-side unit test for Si5351 clock synthesis math.
 *
 * Issue #133: validates a best-rational-approximation (continued fraction)
 * algorithm for selecting Si5351 multisynth register values BEFORE any
 * firmware change is made.  No FX3/libusb/hardware dependency — pure
 * integer + double-precision arithmetic, the same types the firmware uses.
 *
 * It does three things for each candidate algorithm:
 *   1. computes the a + b/c PLL feedback value and the integer output
 *      multisynth divider for a requested frequency,
 *   2. packs them into the Si5351 P1/P2/P3 register fields and decodes
 *      them back, so the register packing itself is exercised (not just
 *      the math), and
 *   3. reconstructs the actual synthesized frequency and reports the
 *      error in Hz and ppm.
 *
 * Two algorithms are compared:
 *   - "current": the integer algorithm in SDDC_FX3/driver/Si5351.c today
 *     (integer-only output divider, fixed denom = 1048575).
 *   - "new": the continued-fraction algorithm ported from
 *     fventuri/DFC-transceiver fx3-firmware/si5351.c.
 *
 * Output is TAP (Test Anything Protocol), matching tests/fw_test.sh, so this
 * can later be folded into the suite.  Exit code is non-zero on any failure.
 *
 * Build:  make -C tests si5351_math_test   (or: cc -O2 -o si5351_math_test si5351_math_test.c -lm)
 * Run:    ./tests/si5351_math_test
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SI5351_REFERENCE      27000000.0  /* RX888 crystal, Si5351.c SI5351_FREQ */
#define SI5351_MAX_VCO_FREQ   900000000.0
#define SI5351_MAX_DENOM      1048575u    /* 2^20 - 1 */

/* ------------------------------------------------------------------ */
/* TAP bookkeeping                                                     */
/* ------------------------------------------------------------------ */
static int tap_count = 0;
static int tap_fails = 0;

static void ok(int cond, const char *desc)
{
    tap_count++;
    if (cond) {
        printf("ok %d - %s\n", tap_count, desc);
    } else {
        printf("not ok %d - %s\n", tap_count, desc);
        tap_fails++;
    }
}

/* ------------------------------------------------------------------ */
/* Register packing / decoding (Si5351 datasheet, P1/P2/P3 encoding)   */
/* ------------------------------------------------------------------ */

/* Fractional multiplier a + b/c -> P1,P2,P3 (matches SetupPLL in Si5351.c
 * and the data_clkin packing in the reference). */
static void pack_frac(uint32_t a, uint32_t b, uint32_t c,
                      uint32_t *p1, uint32_t *p2, uint32_t *p3)
{
    uint32_t b_over_c = 128u * b / c;
    *p1 = 128u * a + b_over_c - 512u;
    *p2 = 128u * b - c * b_over_c;
    *p3 = c;
}

/* Decode P1/P2/P3 back to the real multiplier value a + b/c.
 *   P1 + 512 + P2/P3 == 128*(a + b/c)   (P2/P3 is the fractional part of
 *   128b/c that P1 dropped via integer division), so dividing by 128
 *   recovers the multiplier the chip actually applies. */
static double unpack_frac(uint32_t p1, uint32_t p2, uint32_t p3)
{
    return ((double)p1 + 512.0 + (double)p2 / (double)p3) / 128.0;
}

/* ------------------------------------------------------------------ */
/* Continued-fraction best rational approximation                      */
/* (verbatim port of rational_approximation from the reference)        */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* "new" algorithm: returns synthesized frequency via the register     */
/* round-trip, or NAN if no solution within Si5351 constraints.        */
/* ------------------------------------------------------------------ */
static double synth_new(double reference, double frequency)
{
    double r_frequency = frequency;
    int rdiv = 0;
    while (r_frequency < 1e6 && rdiv <= 7) {
        r_frequency *= 2.0;
        rdiv += 1;
    }
    if (r_frequency < 1e6)
        return NAN; /* below R-divider reach */

    uint32_t output_ms = (uint32_t)(SI5351_MAX_VCO_FREQ / r_frequency);
    output_ms -= output_ms % 2;
    if (output_ms < 4 || output_ms > 900)
        return NAN; /* invalid output divider */

    double vco = r_frequency * output_ms;
    double feedback = vco / reference;

    uint32_t a, b, c;
    rational_approximation(feedback, SI5351_MAX_DENOM, &a, &b, &c);

    /* Pack and decode so the test exercises the actual register fields. */
    uint32_t p1, p2, p3;
    pack_frac(a, b, c, &p1, &p2, &p3);
    double actual_feedback = unpack_frac(p1, p2, p3);

    return reference * actual_feedback / (double)output_ms / (double)(1u << rdiv);
}

/* ------------------------------------------------------------------ */
/* "current" algorithm: faithful reproduction of si5351aSetFrequencyA  */
/* (integer-only output divider, fixed denom).  Integer freq only.     */
/* ------------------------------------------------------------------ */
static double synth_current(uint32_t freq)
{
    if (freq == 0)
        return NAN;
    const uint32_t xtal = 27000000u;
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

    /* PLL packing is lossless for the multiplier, so use a+b/c directly. */
    double pll_mult = (double)mult + (double)num / (double)denom;
    double vco = (double)xtal * pll_mult;
    return vco / (double)divider / (double)(1u << shifts);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static double ppm_err(double actual, double want)
{
    return (actual - want) / want * 1e6;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* Common ADC clock rates the firmware is exercised at (see fx3_cmd.c). */
static const double common_rates[] = {
    2000000, 16000000, 32000000, 48000000, 50000000,
    64000000, 100000000, 122880000, 128000000, 129600000
};

/* Frequencies constructed to be EXACTLY synthesizable: freq = ref*N/ms,
 * integer feedback (b=0), even output_ms in range. New algo must nail these. */
static const double exact_rates[] = {
    64800000.0,   /* ref*24/10  -> VCO 648 MHz */
    54000000.0,   /* ref*20/10  -> VCO 540 MHz... ms=16 -> VCO 864 */
    27000000.0,   /* ref*16/16  -> VCO 432... ms=32 VCO 864 */
    13500000.0,   /* ref*32/64 */
};

int main(void)
{
    printf("# Si5351 continued-fraction synthesis math (issue #133)\n");
    printf("# reference = %.0f Hz\n", SI5351_REFERENCE);

    /* Plan is emitted at the end (TAP allows a trailing plan). */

    /* --- 1. Common rates: new algo finds a solution and is no worse
     *        than the current integer algorithm. --- */
    printf("# --- common ADC rates: new vs current ---\n");
    for (size_t i = 0; i < sizeof(common_rates) / sizeof(common_rates[0]); ++i) {
        double want = common_rates[i];
        double an = synth_new(SI5351_REFERENCE, want);
        double ac = synth_current((uint32_t)want);

        char d[160];
        snprintf(d, sizeof(d), "new finds a solution for %.0f Hz", want);
        ok(!isnan(an), d);
        if (isnan(an))
            continue;

        double en = fabs(an - want);
        double ec = fabs(ac - want);
        printf("#   %.0f Hz: new err=%+.6f Hz (%+.4f ppm) | current err=%+.6f Hz (%+.4f ppm)\n",
               want, an - want, ppm_err(an, want), ac - want, ppm_err(ac, want));

        snprintf(d, sizeof(d), "new error <= current error for %.0f Hz", want);
        ok(en <= ec + 1e-6, d);
    }

    /* --- 2. Exactly-synthesizable rates: error must be ~0. --- */
    printf("# --- exactly synthesizable rates ---\n");
    for (size_t i = 0; i < sizeof(exact_rates) / sizeof(exact_rates[0]); ++i) {
        double want = exact_rates[i];
        double an = synth_new(SI5351_REFERENCE, want);
        char d[160];
        snprintf(d, sizeof(d), "new is exact (<1e-3 Hz) for %.0f Hz", want);
        printf("#   %.0f Hz: new err=%+.9f Hz\n", want, isnan(an) ? NAN : an - want);
        ok(!isnan(an) && fabs(an - want) < 1e-3, d);
    }

    /* --- 3. Fractional / ppb reference adjustment: the new algo can
     *        express sub-Hz targets that the integer command cannot. --- */
    printf("# --- fractional (sub-Hz) targets ---\n");
    {
        double base = 64000000.0;
        double offsets[] = {0.5, 1.0 / 16.0, 64.0 /* 1 ppm */, 0.064 /* 1 ppb */};
        for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
            double want = base + offsets[i];
            double an = synth_new(SI5351_REFERENCE, want);
            char d[160];
            snprintf(d, sizeof(d), "new resolves fractional target %.3f Hz to <0.05 Hz", want);
            printf("#   %.3f Hz: new err=%+.6f Hz (%+.5f ppm)\n",
                   want, isnan(an) ? NAN : an - want, isnan(an) ? NAN : ppm_err(an, want));
            ok(!isnan(an) && fabs(an - want) < 0.05, d);
        }
    }

    /* --- 4. Random sweep: a solution is ALWAYS found across the
     *        supported window, and accuracy beats the integer algo on
     *        average.  This is the "always comes up with a solution"
     *        guarantee David asked for in the thread. --- */
    printf("# --- random sweep over [2 MHz, 129.6 MHz] ---\n");
    {
        const int N = 20000;
        const double lo = 2000000.0, hi = 129600000.0;
        int new_solved = 0;
        double new_worst_ppm = 0.0, cur_worst_ppm = 0.0;
        double new_sum_ppm = 0.0, cur_sum_ppm = 0.0;
        srand(1234567u); /* fixed seed -> deterministic CI runs */
        for (int i = 0; i < N; ++i) {
            double frac = (double)rand() / (double)RAND_MAX;
            double want = lo + frac * (hi - lo);
            double an = synth_new(SI5351_REFERENCE, want);
            if (!isnan(an)) {
                new_solved++;
                double p = fabs(ppm_err(an, want));
                new_sum_ppm += p;
                if (p > new_worst_ppm) new_worst_ppm = p;
            }
            double ac = synth_current((uint32_t)(want + 0.5));
            if (!isnan(ac)) {
                double p = fabs(ppm_err(ac, want));
                cur_sum_ppm += p;
                if (p > cur_worst_ppm) cur_worst_ppm = p;
            }
        }
        printf("#   new:     solved %d/%d, worst |err|=%.5f ppm, mean=%.5f ppm\n",
               new_solved, N, new_worst_ppm, new_sum_ppm / N);
        printf("#   current: worst |err|=%.5f ppm, mean=%.5f ppm\n",
               cur_worst_ppm, cur_sum_ppm / N);
        ok(new_solved == N, "new finds a solution for every frequency in the sweep");
        ok(new_worst_ppm <= cur_worst_ppm + 1e-6,
           "new worst-case error <= current worst-case error");
        ok(new_sum_ppm <= cur_sum_ppm + 1e-6,
           "new mean error <= current mean error");
    }

    printf("1..%d\n", tap_count);
    printf("# %d/%d passed\n", tap_count - tap_fails, tap_count);
    return tap_fails ? 1 : 0;
}
