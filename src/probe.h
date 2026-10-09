/* SPDX-License-Identifier: MIT */
#ifndef PROBE_H
#define PROBE_H

#include "topology.h"

#include <signal.h>
#include <stdio.h>

#define SAVED_REGS 40

struct probe {
    void *mapping;
    size_t map_size;
    volatile uint32_t *regs;
    uint32_t saved[SAVED_REGS];
    bool owned;
    uint64_t start_ns;
    int master[COUNTER_PAIRS];
};

struct monitor {
    struct probe probe[PROBE_COUNT];
    int mem_fd;
    int lock_fd;
};

extern volatile sig_atomic_t interrupted;
uint64_t monotonic_ns(void);
int check_platform(void);
int inspect_ddr_config(FILE *out);
int monitor_open(struct monitor *m, const struct schedule *s, bool read_only);
int monitor_claim(struct monitor *m);
int monitor_slice(struct monitor *m, const struct schedule *s, unsigned slot,
                  uint64_t duration_ns, struct measurement samples[MAX_MASTERS]);
void monitor_inspect(const struct monitor *m, FILE *out);
int monitor_reset(struct monitor *m);
int monitor_close(struct monitor *m);

#endif
