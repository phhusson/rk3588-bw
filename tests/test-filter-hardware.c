/* SPDX-License-Identifier: MIT */
/* Opt-in live check: both independent filter/counter pairs observe the same
 * display stream. This catches a wrong filter-to-counter mapping and lost
 * upper 16 bits. Needs an active display for a nonzero test result.
 */
#include "probe.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static void on_signal(int sig)
{
    interrupted = sig;
}

int main(void)
{
    struct sigaction sa = {.sa_handler = on_signal};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    size_t vop = 0;
    while (vop < master_count && strcmp(masters[vop].name, "vop_m0"))
        ++vop;
    if (vop == master_count)
        return 1;
    struct schedule s = {.slots = 1};
    unsigned p = masters[vop].probe;
    s.count[p] = 2;
    s.master[p][0] = s.master[p][1] = vop;
    struct monitor m;
    if (monitor_open(&m, &s, false))
        return 1;
    int result = monitor_claim(&m);
    struct measurement values[MAX_MASTERS] = {{0}};
    if (!result)
        result = monitor_slice(&m, &s, 0, UINT64_C(20000000), values);
    if (!result) {
        volatile uint32_t *r = m.probe[p].regs;
        uint32_t a = (r[0x20c / 4] & 0xffff) | ((r[0x21c / 4] & 0xffff) << 16);
        uint32_t b = (r[0x22c / 4] & 0xffff) | ((r[0x23c / 4] & 0xffff) << 16);
        printf("Filter 0: %" PRIu32 " bytes; filter 2: %" PRIu32 " bytes\n", a, b);
        if (a != b) {
            fprintf(stderr, "Independent filters disagree.\n");
            result = -1;
        } else if (a <= 65535) {
            puts("SKIP: insufficient display traffic to verify upper counter halves.");
        } else {
            puts("Independent MID filters and chained 32-bit counters passed.");
        }
    }
    if (monitor_close(&m))
        result = -1;
    return result ? 1 : 0;
}
