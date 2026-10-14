// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for firmware/main/aoip/fpp.h (no IDF needed):
//   gcc -Wall -Wextra tools/fpp_select_test.c -o /tmp/fpp_select_test && /tmp/fpp_select_test
#include <stdio.h>

#include "../firmware/main/aoip/fpp.h"

static int failures;

static void check(unsigned min, unsigned max, unsigned mtu, unsigned prof, unsigned want)
{
    unsigned got = aoip_select_fpp(min, max, mtu, prof);
    if (got != want) {
        printf("FAIL min=%u max=%u mtu=%u prof=%u: got %u want %u\n", min, max, mtu, prof, got, want);
        failures++;
    }
}

int main(void)
{
    check(2, 32, 46, 32, 32);   // normal: transmitter max inside all caps
    check(2, 32, 46, 8, 8);     // latency profile caps (profile 4)
    check(2, 64, 46, 64, 46);   // MTU caps (profile cap above it)
    check(16, 32, 46, 8, 16);   // min above profile cap: degraded, still unfragmented
    check(4, 4, 46, 4, 4);      // exact fit at profile 2
    check(2, 0, 46, 32, 2);     // degenerate max with a real min: request the min
    check(0, 0, 46, 32, 0);     // degenerate advertisement: caller maps 0 -> 1
    // NOTE: min > mtu (refuse-the-flow) is decided by the caller in
    // subscriber.c, not by aoip_select_fpp (which requires min <= mtu).
    if (failures == 0) printf("fpp_select_test: all cases pass\n");
    return failures != 0;
}
