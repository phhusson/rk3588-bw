/* SPDX-License-Identifier: MIT */
#include "topology.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void check_schedule(const bool selected[MAX_MASTERS])
{
    struct schedule s;
    make_schedule(&s, selected);
    for (unsigned p = 0; p < PROBE_COUNT; ++p) {
        unsigned seen[MAX_MASTERS] = {0};
        unsigned slots = (s.count[p] + 1) / 2;
        assert(s.count[p] <= MAX_PER_PROBE);
        assert(s.slots >= slots);
        for (unsigned slot = 0; slot < slots; ++slot) {
            for (unsigned pair = 0; pair < 2; ++pair) {
                int idx = scheduled_master(&s, p, slot, pair);
                if (idx < 0)
                    continue;
                assert((size_t)idx < master_count);
                assert(masters[idx].probe == p);
                assert(selected[idx]);
                ++seen[idx];
                /* Scheduling repeats without changing the counter mapping. */
                assert(scheduled_master(&s, p, slot + slots, pair) == idx);
            }
        }
        if (!slots)
            assert(scheduled_master(&s, p, 42, 0) == -1);
        for (size_t i = 0; i < master_count; ++i)
            assert(seen[i] == (unsigned)(selected[i] && masters[i].probe == p));
    }
}

int main(void)
{
    bool selected[MAX_MASTERS] = {false};
    assert(master_count <= MAX_MASTERS);
    check_schedule(selected);
    bool found_av1 = false;
    for (size_t i = 0; i < master_count; ++i) {
        assert(masters[i].probe < PROBE_COUNT);
        assert(masters[i].mid >= -1 && masters[i].mid <= 0x7f);
        if (!strcmp(masters[i].group, "av1")) {
            assert(masters[i].mid == 0 && masters[i].probe == 4);
            found_av1 = true;
        }
        for (size_t j = i + 1; j < master_count; ++j) {
            assert(strcmp(masters[i].name, masters[j].name));
            assert(masters[i].probe != masters[j].probe || masters[i].mid != masters[j].mid);
        }
        selected[i] = true;
        /* Includes every odd/even list length, empty probes, and full lists. */
        check_schedule(selected);
    }
    assert(found_av1);
    for (size_t i = 0; i < master_count; ++i)
        selected[i] = !strcmp(masters[i].group, "cpu") || !strcmp(masters[i].group, "gpu") ||
                      !strcmp(masters[i].group, "npu") || !strcmp(masters[i].group, "vop");
    struct schedule s;
    make_schedule(&s, selected);
    assert(s.slots == 1); /* These four complete IPs can run simultaneously. */
    check_schedule(selected);

    /* Scaling must use enabled time per interface, not the full report, nor
     * a sum of times across parallel interfaces. Use counts beyond 32 bits.
     */
    struct measurement a = {UINT64_C(8000000000), UINT64_C(1000000000)};
    struct measurement b = {UINT64_C(1000000000), UINT64_C(250000000)};
    assert(fabs(bandwidth_mbps(&a) - 8000.0) < 1e-9);
    assert(fabs(bandwidth_mbps(&b) - 4000.0) < 1e-9);
    assert(fabs(bandwidth_mbps(&a) + bandwidth_mbps(&b) - 12000) < 1e-9);
    assert(fabs(coverage_percent(&b, UINT64_C(1000000000)) - 25) < 1e-9);
    struct measurement zero = {0};
    assert(bandwidth_mbps(&zero) == 0 && coverage_percent(&zero, 0) == 0);
    puts("Topology, scheduling, MID zero, coverage and bandwidth arithmetic passed.");
    return 0;
}
