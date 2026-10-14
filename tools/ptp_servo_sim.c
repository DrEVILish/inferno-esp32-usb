// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host simulation of the PTPv1 follower servo (firmware/main/clock/ptp_servo.c).
//
//   cc -O2 -I firmware/main -o ptp_servo_sim tools/ptp_servo_sim.c
//      firmware/main/clock/ptp_servo.c -lm   (one line)
//   ./ptp_servo_sim [scenario]
//
// Scenarios: 0 clean, 1 queueing jitter + periodic +15 µs Sync delays,
// 2 persistent -36 µs path-delay shift at t=60 s, 3 all of the above with a
// +80 ppm crystal. Prints lock time, steps, and the true offset statistics
// after lock. Exits non-zero if lock takes > 25 s or the locked |offset|
// 99th percentile exceeds 2 µs (5 µs in scenario 2/3 after the shift).
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock/ptp_servo.h"

// Local clock model: L(T) = l0 + ∫ (1 + (drift + corr) * 1e-9) dT, integrated
// in 1 ms steps of master time T.
typedef struct {
    double l;           // local time at the current master time (ns)
    double drift_ppb;   // crystal error (positive = fast)
    double corr_ppb;    // applied correction
} lclock_t;

static double rnd(void) { return (double)rand() / RAND_MAX; }
static double expo(double mean) { return -mean * log(1.0 - rnd() * 0.999999); }

int main(int argc, char **argv)
{
    int scen = argc > 1 ? atoi(argv[1]) : 1;
    srand(1234 + scen);

    lclock_t c = { .l = 5.0e9, .drift_ppb = (scen == 3) ? 80000.0 : 41000.0, .corr_ppb = 0 };
    servo_t s;
    servo_init(&s);

    const double base_delay = 50000.0;     // one-way path, ns
    const double ts_q = 40.0;              // timestamp quantum, ns
    double T = 1.0e9 * 1000;               // master time, ns
    const double T_end = T + 300e9;
    const double T_start = T;
    double next_sync = T + 0.1e9, next_dreq = 1e30;
    double lock_at = -1;
    int prev_steps = 0;
    double sum2 = 0, maxabs = 0;
    int nstat = 0;
    static double hist[400000];
    int nh = 0;
    double shift_at = (scen >= 2) ? T_start + 60e9 : 1e30;

    for (; T < T_end; T += 1e6) {
        // advance the local clock by 1 ms of master time
        c.l += 1e6 * (1.0 + (c.drift_ppb + c.corr_ppb) * 1e-9);
        double rel = (T - T_start) * 1e-9;
        double d_ms = base_delay, d_sm = base_delay;
        if (T >= shift_at) {
            d_ms -= 36000.0;                    // leader-to-us faster under load
        }
        if (scen >= 1) {
            d_ms += expo(150.0);
            d_sm += expo(150.0);
            double ph = fmod(rel, 89.0);
            if (ph > 40.0 && ph < 46.0) {
                d_ms += 9000.0 + rnd() * 10000.0;   // Syncs queued behind a burst
            }
        }
        if (T >= next_sync) {
            double t1 = T;
            // local time at arrival = L(T + d_ms) ≈ l + d_ms*(rate)
            double t2 = c.l + d_ms;
            t2 = floor(t2 / ts_q) * ts_q;
            servo_act_t a;
            servo_sync(&s, (int64_t)t1, (int64_t)t2, &a);
            if (a.step_ns) c.l += (double)a.step_ns;
            if (a.set_freq) c.corr_ppb = a.freq_ppb;
            next_sync += 250e6;
            if (s.stage == SERVO_TRACK && next_dreq > 1e29) next_dreq = T + 50e6;
        }
        if (T >= next_dreq) {
            double t3 = floor(c.l / ts_q) * ts_q;
            double t4 = T + d_sm;
            servo_delay(&s, (int64_t)t3, (int64_t)t4);
            next_dreq += servo_want_fast_delay(&s) ? 250e6 : (750e6 + rnd() * 500e6);
        }
        double true_off = c.l - T;
        if (s.locked && lock_at < 0) {
            lock_at = rel;
            prev_steps = (int)s.st.steps;
        }
        if (lock_at >= 0 && rel > lock_at + 5.0 && ((int64_t)(T / 1e6) % 250) == 0) {
            double a = fabs(true_off);
            // Scenario 2/3: PTPv1 cannot see path asymmetry; after a -36 µs
            // leader-to-us change the true offset settles at +18 µs (the
            // servo zeroes ms - (ms + sm)/2). Measure deviation from that,
            // skipping the 30 s it takes to be accepted and slewed.
            if (T > shift_at) {
                if (T < shift_at + 30e9) continue;
                a = fabs(true_off - 18000.0);
            }
            sum2 += a * a;
            if (a > maxabs) maxabs = a;
            if (nh < 400000) hist[nh++] = a;
            nstat++;
        }
    }
    // percentile
    for (int i = 1; i < nh; i++) {
        double v = hist[i];
        int j = i - 1;
        while (j >= 0 && hist[j] > v) { hist[j + 1] = hist[j]; j--; }
        hist[j + 1] = v;
    }
    double p99 = nh ? hist[(int)(nh * 0.99)] : 0;
    printf("scenario %d: lock at %.1f s, steps total %" PRIu32 " (after lock %d), lock losses %" PRIu32
           ", outliers %" PRIu32 ", phase shifts %" PRIu32 "\n",
           scen, lock_at, s.st.steps, (int)s.st.steps - prev_steps, s.st.lock_losses,
           s.st.outliers, s.st.phase_shifts);
    printf("  after lock: rms %.0f ns, p99 %.0f ns, max %.0f ns; freq %.1f ppb (crystal %.0f), delay %" PRId64 " ns\n",
           nstat ? sqrt(sum2 / nstat) : 0, p99, maxabs, s.freq_ppb, c.drift_ppb, s.delay_ns);
    int fail = lock_at < 0 || lock_at > 25.0 || p99 > ((scen >= 2) ? 5000.0 : 2000.0);
    printf("  %s\n", fail ? "FAIL" : "ok");
    return fail;
}
