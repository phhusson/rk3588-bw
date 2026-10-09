/* SPDX-License-Identifier: MIT */
#include "probe.h"
#include "topology.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static void on_signal(int signum)
{
    interrupted = signum;
}

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: rk3588-bw [options]\n"
        "Monitor RK3588 DDR-bound NoC payload bytes per IP (read + write).\n"
        "\n"
        "  -m, --masters LIST    Comma-separated IP groups; default: all\n"
        "  -d, --interval MS     Reporting interval, 10..60000 ms; default: 1000\n"
        "  -n, --count N         Reports to produce; 0 means forever (default)\n"
        "  -p, --per-master      Show individual master interfaces\n"
        "      --csv             Emit CSV, with explicit sampling coverage\n"
        "  -l, --list            List supported IPs, MIDs and probes; no hardware access\n"
        "      --inspect         Read probe identification and control registers only\n"
        "      --ddr-config      Read DDR channel interleave/address-map setting\n"
        "      --reset           Stop and clear all probes after an abandoned run\n"
        "  -h, --help            Show this help\n"
        "\n"
        "Examples:\n"
        "  sudo ./rk3588-bw\n"
        "  sudo ./rk3588-bw -m cpu,gpu,npu,vop -d 1000 -n 10\n"
        "  sudo ./rk3588-bw -m gpu --per-master --csv\n"
        "\n"
        "MB/s = 1,000,000 bytes/s. CPU is the sum of the four DSU probes.\n"
        "Selections with more than two MIDs per probe are time multiplexed.\n"
        "Coverage is the least-observed interface's fraction of the report.\n"
        "Do not run another NoC probe tool at the same time.\n");
}

static int parse_number(const char *s, unsigned min, unsigned max, unsigned *out)
{
    char *end;
    errno = 0;
    if (!s[0] || s[0] < '0' || s[0] > '9')
        return -1;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n < min || n > max)
        return -1;
    *out = (unsigned)n;
    return 0;
}

static int select_groups(const char *arg, bool selected[MAX_MASTERS])
{
    char *list = strdup(arg), *save = NULL;
    if (!list)
        return -1;
    bool any = false;
    for (char *name = strtok_r(list, ", \t", &save); name;
         name = strtok_r(NULL, ", \t", &save)) {
        bool found = false;
        for (size_t i = 0; i < master_count; ++i) {
            if (!strcasecmp(name, masters[i].group) || !strcasecmp(name, "all")) {
                selected[i] = true;
                found = any = true;
            }
        }
        if (!found) {
            fprintf(stderr, "Unknown IP group '%s'; use --list.\n", name);
            free(list);
            return -1;
        }
    }
    free(list);
    if (!any)
        fprintf(stderr, "--masters requires at least one IP group.\n");
    return any ? 0 : -1;
}

static void list_masters(void)
{
    printf("%-12s %-19s %-5s %s\n", "IP", "interface", "MID", "probe");
    for (size_t i = 0; i < master_count; ++i) {
        const struct master_desc *m = &masters[i];
        const struct probe_desc *p = &probe_descs[m->probe];
        printf("%-12s %-19s ", m->group, m->name);
        if (m->mid < 0)
            printf("all   ");
        else
            printf("0x%02x  ", (unsigned)m->mid);
        printf("p%u.%u @ 0x%08" PRIx64 "\n", p->port, p->number, p->address);
    }
}

static void print_report(unsigned sample, uint64_t wall_ns,
                         const struct measurement values[MAX_MASTERS],
                         const bool selected[MAX_MASTERS], bool per_master, bool csv)
{
    if (!csv)
        printf("\nSample %u (%.3f s)     read + write, decimal MB/s\n"
               "%-20s %14s %14s\n", sample, (double)wall_ns / 1e9,
               per_master ? "Master" : "IP", "MB/s", "min coverage");
    for (size_t i = 0; i < master_count;) {
        size_t end = i + 1;
        if (!per_master)
            while (end < master_count && !strcmp(masters[i].group, masters[end].group))
                ++end;
        double mbps = 0, min_cov = 100, max_cov = 0;
        bool any = false;
        for (size_t j = i; j < end; ++j) {
            if (!selected[j])
                continue;
            any = true;
            double cov = coverage_percent(&values[j], wall_ns);
            mbps += bandwidth_mbps(&values[j]);
            if (cov < min_cov)
                min_cov = cov;
            if (cov > max_cov)
                max_cov = cov;
        }
        const char *name = per_master ? masters[i].name : masters[i].group;
        if (any) {
            if (csv)
                printf("%u,%.6f,%s,%.6f,%.3f,%.3f\n", sample, (double)wall_ns / 1e9,
                       name, mbps, min_cov, max_cov);
            else
                printf("%-20s %14.2f %13.1f%%\n", name, mbps, min_cov);
        }
        i = end;
    }
}

int main(int argc, char **argv)
{
    unsigned interval_ms = 1000, count = 0;
    bool selected[MAX_MASTERS] = {false};
    bool masters_given = false, per_master = false, csv = false;
    bool list = false, inspect = false, reset = false, ddr_config = false;
    enum { OPT_CSV = 256, OPT_INSPECT, OPT_RESET, OPT_DDR_CONFIG };
    const struct option opts[] = {
        {"masters", required_argument, NULL, 'm'},
        {"interval", required_argument, NULL, 'd'},
        {"count", required_argument, NULL, 'n'},
        {"per-master", no_argument, NULL, 'p'},
        {"csv", no_argument, NULL, OPT_CSV},
        {"list", no_argument, NULL, 'l'},
        {"inspect", no_argument, NULL, OPT_INSPECT},
        {"ddr-config", no_argument, NULL, OPT_DDR_CONFIG},
        {"reset", no_argument, NULL, OPT_RESET},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "m:d:n:plh", opts, NULL)) != -1) {
        switch (c) {
        case 'm':
            masters_given = true;
            if (select_groups(optarg, selected))
                return 2;
            break;
        case 'd':
            if (parse_number(optarg, 10, 60000, &interval_ms)) {
                fprintf(stderr, "--interval must be an integer from 10 to 60000 ms.\n");
                return 2;
            }
            break;
        case 'n':
            if (parse_number(optarg, 0, 1000000000, &count)) {
                fprintf(stderr, "--count must be an integer from 0 to 1000000000.\n");
                return 2;
            }
            break;
        case 'p': per_master = true; break;
        case 'l': list = true; break;
        case OPT_CSV: csv = true; break;
        case OPT_INSPECT: inspect = true; break;
        case OPT_DDR_CONFIG: ddr_config = true; break;
        case OPT_RESET: reset = true; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind != argc || (unsigned)list + (unsigned)inspect + (unsigned)reset +
                         (unsigned)ddr_config > 1) {
        usage(stderr);
        return 2;
    }
    if ((inspect || reset || list || ddr_config) && (masters_given || csv || per_master)) {
        fprintf(stderr, "Informational/reset modes cannot be combined with output/selection options.\n");
        return 2;
    }
    if (list) {
        list_masters();
        return 0;
    }
    if (ddr_config)
        return inspect_ddr_config(stdout) ? 1 : 0;
    if (!masters_given)
        for (size_t i = 0; i < master_count; ++i)
            selected[i] = true;
    struct schedule schedule;
    make_schedule(&schedule, selected);
    if (!inspect && !reset && interval_ms < schedule.slots) {
        fprintf(stderr, "This selection needs at least %u ms per report.\n", schedule.slots);
        return 2;
    }

    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        if (sigaction(signals[i], &action, NULL)) {
            perror("sigaction");
            return 1;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct monitor monitor;
    if (monitor_open(&monitor, inspect || reset ? NULL : &schedule, inspect))
        return 1;
    if (inspect || reset) {
        int result = 0;
        if (reset) {
            result = monitor_reset(&monitor);
            if (!result)
                puts("All 12 RK3588 NoC probes stopped and cleared.");
        } else {
            monitor_inspect(&monitor, stdout);
        }
        if (monitor_close(&monitor))
            result = -1;
        return result ? 1 : 0;
    }
    if (monitor_claim(&monitor)) {
        monitor_close(&monitor);
        return 1;
    }

    /* Complete enough scheduling rounds to visit every selected MID, with
     * <=20 ms between counter resets even for minute-long output intervals.
     */
    unsigned rounds = (interval_ms + schedule.slots * 20 - 1) / (schedule.slots * 20);
    unsigned slices = rounds * schedule.slots;
    uint64_t slice_ns = (uint64_t)interval_ms * UINT64_C(1000000) / slices;
    fprintf(stderr, "RK3588 NoC: %u multiplex slot%s, %.3f ms slices. "
            "CPU = aggregate DSU DDR traffic.\n",
            schedule.slots, schedule.slots == 1 ? "" : "s", (double)slice_ns / 1e6);
    if (csv)
        puts("sample,elapsed_s,ip,mb_per_s,min_coverage_pct,max_coverage_pct");
    int result = 0;
    for (unsigned sample = 1; !interrupted && (!count || sample <= count); ++sample) {
        struct measurement values[MAX_MASTERS] = {{0}};
        uint64_t start = monotonic_ns();
        for (unsigned slot = 0; slot < slices && !interrupted; ++slot) {
            int ret = monitor_slice(&monitor, &schedule, slot, slice_ns, values);
            if (ret) {
                result = ret < 0 ? 1 : 0;
                break;
            }
        }
        if (interrupted || result)
            break;
        print_report(sample, monotonic_ns() - start, values, selected, per_master, csv);
        if (ferror(stdout)) {
            perror("stdout");
            result = 1;
            break;
        }
    }
    if (monitor_close(&monitor))
        result = 1;
    return interrupted ? 128 + interrupted : result;
}
