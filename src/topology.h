/* SPDX-License-Identifier: MIT */
#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROBE_COUNT 12
#define MAX_MASTERS 80
#define MAX_PER_PROBE 32
#define COUNTER_PAIRS 2

struct probe_desc {
    uint64_t address;
    uint32_t core_id;
    unsigned port, number;
};

struct master_desc {
    const char *group;
    const char *name;
    unsigned probe;
    int mid;                   /* -1: unfiltered CPU/DSU probe */
};

struct schedule {
    size_t master[PROBE_COUNT][MAX_PER_PROBE];
    unsigned count[PROBE_COUNT];
    unsigned slots;            /* maximum slots needed by any one probe */
};

struct measurement {
    uint64_t bytes;
    uint64_t active_ns;
};

extern const struct probe_desc probe_descs[PROBE_COUNT];
extern const struct master_desc masters[];
extern const size_t master_count;

void make_schedule(struct schedule *s, const bool selected[MAX_MASTERS]);
/* -1 for an unused counter pair. Shorter lists repeat independently. */
int scheduled_master(const struct schedule *s, unsigned probe, unsigned slot,
                     unsigned pair);
double bandwidth_mbps(const struct measurement *m);
double coverage_percent(const struct measurement *m, uint64_t wall_ns);

#endif
