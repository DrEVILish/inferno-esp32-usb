// SPDX-License-Identifier: GPL-3.0-or-later
//
// PTPv1 follower servo: pure arithmetic, no hardware and no RTOS, so the same
// file builds into the firmware and into the host simulation
// (tools/ptp_servo_sim.c).
//
// Inputs are timestamp pairs, all in ns:
//   servo_sync(t1, t2)   t1 = leader's precise origin of a Sync, t2 = our HW
//                        RX timestamp of that Sync
//   servo_delay(t3, t4)  t3 = our HW TX timestamp of a Delay_Req, t4 = the
//                        leader's receipt time from the Delay_Resp
// Outputs are actions the caller applies to the clock immediately: a step
// (add step_ns to the local clock) and/or an absolute frequency correction.
//
// Owner: clock/ (net+clock agent).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- tuning (see docs/11-net-clock.md for the reasoning) -------------------
#define SERVO_STEP_NS           100000      // |offset| above this: step the clock
#define SERVO_SNAP_NS           20000       // one phase snap when the path delay first becomes known
#define SERVO_MAX_PPB           300000.0    // frequency correction limit (±300 ppm)
#define SERVO_FREQ_MIN_N        6           // samples for the initial frequency estimate...
#define SERVO_FREQ_SPAN_NS      3000000000LL // ...spanning at least 3 s
#define SERVO_FREQ_MAX_N        32
#define SERVO_KP_ACQ            0.5         // 1/s   (acquisition: τ ≈ 2 s)
#define SERVO_KI_ACQ            0.0625      // 1/s²  (= Kp²/4, critically damped)
#define SERVO_KP_LOCK           0.15        // 1/s   (locked: τ ≈ 7 s)
#define SERVO_KI_LOCK           0.005625    // 1/s²  (= Kp²/4)
#define SERVO_LOCK_IN_NS        3000        // lock: this many consecutive samples...
#define SERVO_LOCK_N            8           // ...within ±3 µs (path delay known)
#define SERVO_LOCK_OUT_NS       10000       // unlock: this many consecutive...
#define SERVO_UNLOCK_N          8           // ...accepted samples beyond ±10 µs
#define SERVO_OUTLIER_NS        5000        // locked: farther than this from the last accepted offset
#define SERVO_SHIFT_N           40          // consecutive outliers that agree => real phase shift
#define SERVO_SHIFT_AGREE_NS    8000        // ...when their spread is within this
#define SERVO_HOLD_MAX          80          // consecutive outliers before giving up the lock
#define SERVO_DELAY_N           16          // path delay: low percentile of the last N samples
#define SERVO_DELAY_MIN_N       3           // path delay valid from this many samples
#define SERVO_DELAY_MAX_NS      2000000     // sanity bound on one path delay sample
#define SERVO_DELAY_MIN_NS      (-20000)    // (asymmetric links can read slightly negative)

typedef enum {
    SERVO_IDLE = 0,     // no sample yet (or restarted): next sample steps
    SERVO_FREQ,         // estimating the frequency error after the first step
    SERVO_TRACK,        // PI control on the offset
} servo_stage_t;

typedef struct {
    int64_t step_ns;    // != 0: add to the local clock now (then bump the epoch)
    bool    set_freq;
    double  freq_ppb;   // absolute correction (positive = speed the counter up)
} servo_act_t;

typedef struct {
    uint32_t samples, steps, lock_losses, outliers, phase_shifts;
    uint32_t delay_samples, delay_rejected;
} servo_stats_t;

typedef struct {
    servo_stage_t stage;
    bool     freq_known;            // survives restarts (holdover)
    double   freq_ppb;              // currently applied correction
    double   integ_ppb;             // PI integral

    // frequency estimate
    int      fn;
    int64_t  fx[SERVO_FREQ_MAX_N];  // t2 relative to the first point
    int64_t  fy[SERVO_FREQ_MAX_N];  // t2 - t1
    int64_t  f_t0;

    // path delay
    int64_t  dring[SERVO_DELAY_N];
    int      dn, didx;
    int64_t  delay_ns;
    bool     delay_valid, delay_snapped;
    int64_t  last_ms;               // t2 - t1 of the latest usable Sync
    bool     last_ms_valid;
    bool     last_ms_outlier;       // latest Sync was held out as an outlier

    // tracking
    int64_t  prev_t2;
    bool     prev_t2_valid;
    int      skip;
    bool     locked;
    int      lock_cnt, unlock_cnt;
    int64_t  off_ref;               // last accepted offset
    int      outl_n;
    int64_t  outl[SERVO_SHIFT_N];   // ring of recent outlier offsets
    int64_t  offset_ns;             // last accepted offset (reported)

    servo_stats_t st;
} servo_t;

void servo_init(servo_t *s);

// Forget leader-specific state (path delay, lock, phase) but keep the
// frequency estimate. Used on leader change / leader loss.
void servo_restart(servo_t *s);

void servo_sync(servo_t *s, int64_t t1, int64_t t2, servo_act_t *act);
void servo_delay(servo_t *s, int64_t t3, int64_t t4);

// True while Delay_Req should go out fast (path delay not yet settled).
bool servo_want_fast_delay(const servo_t *s);
