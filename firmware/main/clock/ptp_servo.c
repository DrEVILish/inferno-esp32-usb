// SPDX-License-Identifier: GPL-3.0-or-later
//
// PTPv1 follower servo. See ptp_servo.h for the interface and
// docs/11-net-clock.md for the design.
#include "clock/ptp_servo.h"

#include <string.h>

static inline int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

static double clampd(double v, double lim)
{
    return v > lim ? lim : (v < -lim ? -lim : v);
}

// In-place selection (Hoare quickselect) of the k-th smallest element.
static double select_d(double *a, int n, int k)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        double pivot = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) {
                double t = a[i]; a[i] = a[j]; a[j] = t;
                i++; j--;
            }
        }
        if (k <= j) {
            hi = j;
        } else if (k >= i) {
            lo = i;
        } else {
            break;
        }
    }
    return a[k];
}

// k-th smallest of v[0..n) (v is not modified).
static int64_t kth_i64(const int64_t *v, int n, int k)
{
    double tmp[SERVO_SHIFT_N > SERVO_DELAY_N ? SERVO_SHIFT_N : SERVO_DELAY_N];
    for (int i = 0; i < n; i++) {
        tmp[i] = (double)v[i];
    }
    return (int64_t)select_d(tmp, n, k);
}

static int64_t median_i64(const int64_t *v, int n)
{
    return kth_i64(v, n, n / 2);
}

// Theil-Sen slope of (fx, fy): median of the pairwise slopes of points at
// least 1 s apart. Robust against a few queueing spikes, unlike least squares.
static double theil_sen(const servo_t *s)
{
    static double slopes[SERVO_FREQ_MAX_N * (SERVO_FREQ_MAX_N - 1) / 2];
    int m = 0;
    for (int i = 0; i < s->fn; i++) {
        for (int j = i + 1; j < s->fn; j++) {
            int64_t dx = s->fx[j] - s->fx[i];
            if (dx >= 1000000000LL) {
                slopes[m++] = (double)(s->fy[j] - s->fy[i]) / (double)dx;
            }
        }
    }
    if (m == 0) {
        return 0.0;
    }
    return select_d(slopes, m, m / 2);
}

static void set_freq(servo_t *s, double ppb, servo_act_t *act)
{
    s->freq_ppb = clampd(ppb, SERVO_MAX_PPB);
    act->set_freq = true;
    act->freq_ppb = s->freq_ppb;
}

// Step by -off. The proportional term is dropped: the applied frequency falls
// back to the integral (the rate estimate) so a saturated P term cannot carry
// over into the new phase.
static void do_step(servo_t *s, int64_t off, servo_act_t *act)
{
    act->step_ns = -off;
    s->st.steps++;
    set_freq(s, s->integ_ppb, act);
    s->last_ms_valid = false;
    s->prev_t2_valid = false;
    s->skip = 1;
    s->lock_cnt = 0;
    s->unlock_cnt = 0;
    s->outl_n = 0;
    s->off_ref = 0;
    s->offset_ns = 0;
}

static void pi_update(servo_t *s, int64_t off, double dt, double kp, double ki, servo_act_t *act)
{
    s->integ_ppb = clampd(s->integ_ppb - ki * (double)off * dt, SERVO_MAX_PPB);
    set_freq(s, s->integ_ppb - kp * (double)off, act);
}

static void unlock(servo_t *s)
{
    if (s->locked) {
        s->st.lock_losses++;
    }
    s->locked = false;
    s->lock_cnt = 0;
    s->unlock_cnt = 0;
    s->outl_n = 0;
}

void servo_init(servo_t *s)
{
    memset(s, 0, sizeof *s);
    s->stage = SERVO_IDLE;
}

void servo_restart(servo_t *s)
{
    unlock(s);
    s->stage = SERVO_IDLE;
    s->fn = 0;
    s->dn = 0;
    s->didx = 0;
    s->delay_ns = 0;
    s->delay_valid = false;
    s->delay_snapped = false;
    s->last_ms_valid = false;
    s->prev_t2_valid = false;
    s->skip = 0;
    s->off_ref = 0;
    s->offset_ns = 0;
}

bool servo_want_fast_delay(const servo_t *s)
{
    return s->stage == SERVO_TRACK && s->dn < SERVO_DELAY_N;
}

static void track_locked(servo_t *s, int64_t off, double dt, servo_act_t *act)
{
    int64_t dev = off - s->off_ref;
    if (iabs64(dev) > SERVO_OUTLIER_NS) {
        // Queueing spike or a real phase shift. Hold the rate (no update)
        // until it is clear which.
        s->st.outliers++;
        s->last_ms_outlier = true;
        s->outl[s->outl_n % SERVO_SHIFT_N] = off;
        s->outl_n++;
        if (s->outl_n >= SERVO_SHIFT_N) {
            int64_t lo = s->outl[0], hi = s->outl[0];
            for (int i = 1; i < SERVO_SHIFT_N; i++) {
                if (s->outl[i] < lo) lo = s->outl[i];
                if (s->outl[i] > hi) hi = s->outl[i];
            }
            if (hi - lo <= SERVO_SHIFT_AGREE_NS) {
                // They agree: the path really changed. Accept the new phase;
                // the PI slews it out (a step only if it is large).
                int64_t med = median_i64(s->outl, SERVO_SHIFT_N);
                s->st.phase_shifts++;
                s->outl_n = 0;
                if (iabs64(med) > SERVO_STEP_NS) {
                    unlock(s);
                    do_step(s, med, act);
                } else {
                    s->off_ref = med;
                    s->offset_ns = med;
                }
                return;
            }
            if (s->outl_n >= SERVO_HOLD_MAX) {
                // Scattered for too long: start over from the raw offsets.
                // Reset the outlier window too: the median below must not
                // mix samples from before the re-anchor (the other unlock
                // path at do_step resets it).
                unlock(s);
                s->outl_n = 0;
                s->off_ref = off;
            }
        }
        return;
    }
    s->outl_n = 0;
    pi_update(s, off, dt, SERVO_KP_LOCK, SERVO_KI_LOCK, act);
    s->off_ref = off;
    s->offset_ns = off;
    if (iabs64(off) > SERVO_LOCK_OUT_NS) {
        if (++s->unlock_cnt >= SERVO_UNLOCK_N) {
            unlock(s);
        }
    } else {
        s->unlock_cnt = 0;
    }
}

static void track_unlocked(servo_t *s, int64_t off, double dt, servo_act_t *act)
{
    if (iabs64(off) > SERVO_STEP_NS) {
        do_step(s, off, act);
        return;
    }
    if (s->delay_valid && !s->delay_snapped) {
        // First offset that includes the real path delay: remove the phase
        // error the frequency estimate left in one go.
        s->delay_snapped = true;
        if (iabs64(off) > SERVO_SNAP_NS) {
            do_step(s, off, act);
            return;
        }
    }
    pi_update(s, off, dt, SERVO_KP_ACQ, SERVO_KI_ACQ, act);
    s->off_ref = off;
    s->offset_ns = off;
    if (s->delay_valid && iabs64(off) < SERVO_LOCK_IN_NS) {
        if (++s->lock_cnt >= SERVO_LOCK_N) {
            s->locked = true;
            s->unlock_cnt = 0;
            s->outl_n = 0;
        }
    } else {
        s->lock_cnt = 0;
    }
}

void servo_sync(servo_t *s, int64_t t1, int64_t t2, servo_act_t *act)
{
    act->step_ns = 0;
    act->set_freq = false;
    act->freq_ppb = s->freq_ppb;
    s->st.samples++;

    if (s->skip > 0) {
        s->skip--;
        return;
    }
    int64_t ms = t2 - t1;
    int64_t delay = s->delay_valid ? s->delay_ns : 0;
    int64_t off = ms - delay;

    switch (s->stage) {
    case SERVO_IDLE:
        // Time is unrelated before the first sample: always step. After a
        // restart with a known rate, step only if the phase is far off.
        if (!s->freq_known || iabs64(off) > SERVO_STEP_NS) {
            do_step(s, off, act);
        }
        s->fn = 0;
        s->stage = s->freq_known ? SERVO_TRACK : SERVO_FREQ;
        return;

    case SERVO_FREQ:
        if (s->fn == 0) {
            s->f_t0 = t2;
        }
        s->fx[s->fn] = t2 - s->f_t0;
        s->fy[s->fn] = ms;
        s->fn++;
        if ((s->fn >= SERVO_FREQ_MIN_N && s->fx[s->fn - 1] >= SERVO_FREQ_SPAN_NS) ||
                s->fn >= SERVO_FREQ_MAX_N) {
            // ms grows by slope ns per local ns: the local clock is fast by
            // slope × 1e9 ppb, so slow it down by that much.
            double drift_ppb = theil_sen(s) * 1e9;
            s->integ_ppb = clampd(s->freq_ppb - drift_ppb, SERVO_MAX_PPB);
            set_freq(s, s->integ_ppb, act);
            s->freq_known = true;
            s->stage = SERVO_TRACK;
            s->prev_t2_valid = false;
            s->skip = 1;        // the next sample straddles the rate change
            s->fn = 0;
        }
        return;

    case SERVO_TRACK:
    default:
        break;
    }

    s->last_ms = ms;
    s->last_ms_valid = true;
    s->last_ms_outlier = false;
    double dt = 0.25;
    if (s->prev_t2_valid) {
        dt = (double)(t2 - s->prev_t2) * 1e-9;
        if (dt < 0.03) dt = 0.03;
        if (dt > 2.0) dt = 2.0;
    }
    s->prev_t2 = t2;
    s->prev_t2_valid = true;

    if (s->locked) {
        track_locked(s, off, dt, act);
    } else {
        track_unlocked(s, off, dt, act);
    }
}

void servo_delay(servo_t *s, int64_t t3, int64_t t4)
{
    // Only once the rate is corrected (earlier samples are biased by the
    // frequency error over the Sync-to-Delay_Req interval) and against a
    // Sync measured in the current phase that was not itself an outlier.
    if (s->stage != SERVO_TRACK || !s->last_ms_valid || s->last_ms_outlier) {
        s->st.delay_rejected++;
        return;
    }
    int64_t sm = t4 - t3;
    int64_t d = (s->last_ms + sm) / 2;
    if (d < SERVO_DELAY_MIN_NS || d > SERVO_DELAY_MAX_NS) {
        s->st.delay_rejected++;
        return;
    }
    s->st.delay_samples++;
    s->dring[s->didx] = d;
    s->didx = (s->didx + 1) % SERVO_DELAY_N;
    if (s->dn < SERVO_DELAY_N) {
        s->dn++;
    }
    if (s->dn >= SERVO_DELAY_MIN_N) {
        // Queueing only ever adds delay, so take a low percentile (the 25th)
        // rather than the median: up to 3/4 of the window can be inflated.
        s->delay_ns = kth_i64(s->dring, s->dn, s->dn / 4);
        s->delay_valid = true;
    }
}
