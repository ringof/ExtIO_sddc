/*
 * si5351_bench.c — cost benchmark for Si5351 divider-search strategies.
 *
 * Issue #133.  Answers "how expensive is the ka9q-style full search to run
 * on the FX3 microcontroller?"  Measures three strategies over many random
 * frequencies, counting inner-loop iterations and expensive evaluations, and
 * timing wall-clock on this host:
 *
 *   full_scan    - faithful model of the ka9q solver: enumerate R (1..128),
 *                  integer MS {4,6,8}, then fractional MS by linearly
 *                  scanning D = 8..2048 against 5 PLL/VCO targets.
 *   full_directD - same search space but compute the straddling D directly
 *                  (D = floor(target/(fout*R))) instead of scanning 2041
 *                  values — the obvious O(1) replacement for the linear scan.
 *   single       - lightweight: one divider (output_ms = floor(900e6/f),
 *                  even), one exact best-rational feedback approximation.
 *
 * Everything is integer/rational; __int128 is used for intermediates (note:
 * the 32-bit FX3 has no native 128-bit type and no hardware divide, so the
 * FX3 cost is dominated by software 64/128-bit multiply and division — see
 * the printed estimate).
 *
 * Build:  make -C tests si5351_bench
 * Run:    ./tests/si5351_bench
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef unsigned long long u64;
typedef unsigned __int128   u128;
typedef __int128            i128;

#define DEN_MAX     1048575ull
#define FREF        27000000ull

static u64 g_inner;   /* inner-loop iterations (band checks etc.) */
static u64 g_eval;    /* expensive evaluations (CF + scoring)     */
static u64 g_cf;      /* total continued-fraction iterations      */

static u64 gcd_u64(u64 a, u64 b) { while (b) { u64 t = a % b; a = b; b = t; } return a; }

/* exact best rational approximation of num/den (num<den), denom<=max_den */
static void bra(u64 num, u64 den, u64 max_den, u64 *ob, u64 *oc)
{
    if (num == 0) { *ob = 0; *oc = 1; return; }
    u64 h2 = 0, h1 = 1, k2 = 1, k1 = 0, a = num, b = den;
    while (b != 0) {
        g_cf++;
        u64 ai = a / b, rem = a % b;
        u64 h = ai * h1 + h2, k = ai * k1 + k2;
        if (k > max_den) {
            if (k1 == 0) break;
            u64 t = (max_den - k2) / k1;
            u64 hs = t * h1 + h2, ks = t * k1 + k2;
            i128 dp = (i128)num * k1 - (i128)h1 * den; if (dp < 0) dp = -dp;
            i128 ds = (i128)num * ks - (i128)hs * den; if (ds < 0) ds = -ds;
            if ((i128)dp * ks <= (i128)ds * k1) { *ob = h1; *oc = k1; }
            else { *ob = hs; *oc = ks; }
            return;
        }
        h2 = h1; h1 = h; k2 = k1; k1 = k; a = b; b = rem;
    }
    *ob = h1; *oc = k1;
}

/* evaluate one (D,R,target-region) candidate: solve feedback, check legality,
 * score by abs error.  Returns updated best error (Hz, as double). */
static void eval_candidate(u64 P, u64 Q, u64 D, u64 E, u64 F, unsigned R, double *best)
{
    /* X = (P/Q) * ((D*F+E)/F) */
    u128 Yn = (u128)D * F + E, Yd = (u128)F;
    u128 Xn = (u128)P * Yn, Xd = (u128)Q * Yd;
    u64 A = (u64)(Xn / Xd);
    u128 rem = Xn % Xd;
    u64 B = 0, C = 1;
    if (rem != 0) {
        u64 r64 = (u64)rem, x64 = (u64)Xd, gg = gcd_u64(r64, x64);
        bra(r64 / gg, x64 / gg, DEN_MAX, &B, &C);
    }
    g_eval++;
    if (A < 15 || A > 90 || B >= C) return;
    /* VCO = fref*(A+B/C) in [600,900] MHz */
    u128 vn = (u128)FREF * ((u128)A * C + B), vd = (u128)C;
    if (vn < (u128)600000000ull * vd || vn > (u128)900000000ull * vd) return;
    double fb = (double)A + (double)B / (double)C;
    double fy = (double)D + (double)E / (double)F;
    double fout_got = (double)FREF * fb / fy / (double)R;
    double err = fabs(fout_got - ((double)P / (double)Q) * (double)FREF);
    if (err < *best) *best = err;
}

static const u64 PLL_TARGETS[] = {900000000ull, 864000000ull, 800000000ull, 750000000ull, 600000000ull};

static double solve_full(u64 fout, int directD)
{
    u64 P = fout, Q = FREF, g = gcd_u64(P, Q); P /= g; Q /= g;
    double best = INFINITY;
    for (unsigned R = 1; R <= 128; R <<= 1) {
        /* integer MS {4,6,8} */
        const u64 Dint[] = {4, 6, 8};
        for (int i = 0; i < 3; ++i) {
            g_inner++;
            eval_candidate(P, Q, Dint[i], 0, 1, R, &best);
        }
        /* fractional MS */
        u128 den = (u128)fout * R;
        for (unsigned t = 0; t < 5; ++t) {
            u128 numer = PLL_TARGETS[t];
            if (directD) {
                if (den == 0) continue;
                u64 D = (u64)(numer / den);
                g_inner++;
                if (D < 8 || D > 2048) continue;
                u128 Dden = (u128)D * den;
                u128 frac_num = numer - Dden;
                u64 E = 0, F = 1;
                if (frac_num != 0) {
                    u64 fn = (u64)frac_num, fd = (u64)den, gg = gcd_u64(fn, fd);
                    bra(fn / gg, fd / gg, DEN_MAX, &E, &F);
                }
                eval_candidate(P, Q, D, E, F, R, &best);
            } else {
                for (unsigned D = 8; D <= 2048; ++D) {
                    g_inner++;
                    u128 Dden = (u128)D * den, D1den = (u128)(D + 1) * den;
                    if (numer < Dden || numer >= D1den) continue;
                    u128 frac_num = numer - Dden;
                    u64 E = 0, F = 1;
                    if (frac_num != 0) {
                        u64 fn = (u64)frac_num, fd = (u64)den, gg = gcd_u64(fn, fd);
                        bra(fn / gg, fd / gg, DEN_MAX, &E, &F);
                    }
                    eval_candidate(P, Q, D, E, F, R, &best);
                }
            }
        }
    }
    return best;
}

/* lightweight: one even output divider near 900 MHz VCO, one exact feedback CF */
static double solve_single(u64 fout)
{
    double rf = (double)fout; int rdiv = 0;
    while (rf < 1e6 && rdiv <= 7) { rf *= 2.0; rdiv++; }
    if (rf < 1e6) return INFINITY;
    u64 ms = (u64)(900000000.0 / rf); ms -= ms % 2;
    if (ms < 4 || ms > 900) return INFINITY;
    g_inner++;
    u64 P = fout * (1ull << rdiv) * ms, Q = FREF;
    u64 A = P / Q, rem = P % Q, B = 0, C = 1;
    if (rem) { u64 gg = gcd_u64(rem, Q); bra(rem / gg, Q / gg, DEN_MAX, &B, &C); }
    g_eval++;
    double fb = (double)A + (double)B / (double)C;
    double got = (double)FREF * fb / (double)ms / (double)(1u << rdiv);
    return fabs(got - (double)fout);
}

static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void run(const char *name, int mode)
{
    /* mode: 0=single, 1=full_scan, 2=full_directD */
    const int N = 5000;
    g_inner = g_eval = g_cf = 0;
    srand(424242u);
    double t0 = now_s();
    double acc = 0;
    for (int i = 0; i < N; ++i) {
        double frac = (double)rand() / (double)RAND_MAX;
        u64 fout = (u64)(1000000.0 + frac * (135000000.0 - 1000000.0));
        double e = (mode == 0) ? solve_single(fout) : solve_full(fout, mode == 2);
        acc += (e == INFINITY ? 0 : e);
    }
    double dt = now_s() - t0;
    printf("%-13s  %8.1f us/solve   inner=%8.0f  eval=%6.0f  cf=%7.0f   (mean|err| %.4g Hz)\n",
           name, dt / N * 1e6, (double)g_inner / N, (double)g_eval / N,
           (double)g_cf / N, acc / N);
}

int main(void)
{
    printf("# Si5351 divider-search cost (27 MHz ref, 1-135 MHz, 5000 random freqs)\n");
    printf("# host: native __int128 + hardware divide (NOT the FX3)\n");
    printf("%-13s  %11s   %14s  %12s  %10s\n",
           "strategy", "time", "inner-iters", "evals", "cf-iters");
    run("single",       0);
    run("full_directD", 2);
    run("full_scan",    1);

    printf("\n# FX3 estimate (ARM926EJ-S @200 MHz, no HW divide, software 64/128-bit):\n");
    printf("# Per-op the FX3 is far slower than this host on divide/128-bit-heavy code.\n");
    printf("# The single and full_directD strategies do tens of evals/solve; even at a\n");
    printf("# pessimistic ~1-2 us per exact-eval on the FX3 that is well under 1 ms.\n");
    printf("# full_scan's ~80k inner 128-bit band-checks/solve are the only heavy part;\n");
    printf("# computing D directly removes them. STARTADC is a one-shot config command\n");
    printf("# (current firmware already waits up to 100 ms for PLL lock; EP0 budget ~2 s).\n");
    return 0;
}
