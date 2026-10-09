/* SPDX-License-Identifier: MIT */
#include "probe.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum {
    CORE_ID = 0x000, REVISION_ID = 0x004,
    MAIN_CTL = 0x008, CFG_CTL = 0x00c, STAT_PERIOD = 0x024,
    FILTER0 = 0x080, FILTER_STRIDE = 0x060,
    COUNTER_SRC = 0x204, COUNTER_ALARM = 0x208, COUNTER_VAL = 0x20c,
    COUNTER_STRIDE = 0x010,
    STAT_EN = 8, GLOBAL_EN = 1, ACTIVE = 2,
    EVENT_BYTE = 8, EVENT_CHAIN = 16, EVENT_FILTER_BYTE = 20,
};

/* Only these registers are written. StatGo (a dump pulse) and read-only
 * counter values are deliberately excluded. MainCtl.StatEn's rising edge
 * clears the counters. FiltByteAlwaysChainableEn stays clear: counters 0/1
 * count filter 0, and counters 2/3 count filter 2 (NOT filter 1).
 */
static const unsigned saved_offsets[] = {
    MAIN_CTL, CFG_CTL, STAT_PERIOD,
    0x204, 0x208, 0x214, 0x218, 0x224, 0x228, 0x234, 0x238,
    0x080, 0x084, 0x088, 0x08c, 0x090, 0x094, 0x098,
    0x09c, 0x0a0, 0x0a4, 0x0a8, 0x0ac, 0x0b0,
    0x140, 0x144, 0x148, 0x14c, 0x150, 0x154, 0x158,
    0x15c, 0x160, 0x164, 0x168, 0x16c, 0x170,
};
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
_Static_assert(ARRAY_SIZE(saved_offsets) <= SAVED_REGS, "saved register space");

volatile sig_atomic_t interrupted;

uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* /dev/mem O_SYNC gives device mappings on arm64. Volatile alone is not an
 * MMIO barrier: complete accesses before timing, polling, or reconfiguration.
 */
static void io_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static uint32_t read_reg(const struct probe *p, unsigned offset)
{
    uint32_t v = p->regs[offset / 4];
    io_barrier();
    return v;
}

static void write_reg(struct probe *p, unsigned offset, uint32_t value)
{
    io_barrier();
    p->regs[offset / 4] = value;
    io_barrier();
}

static bool applicable(unsigned probe, unsigned offset)
{
    return probe < 8 || offset < FILTER0 || offset >= COUNTER_SRC;
}

int check_platform(void)
{
    char compatible[4096];
    FILE *f = fopen("/sys/firmware/devicetree/base/compatible", "rb");
    if (!f)
        f = fopen("/proc/device-tree/compatible", "rb");
    if (!f) {
        fprintf(stderr, "Cannot read device-tree compatible: %s\n", strerror(errno));
        return -1;
    }
    size_t n = fread(compatible, 1, sizeof(compatible), f);
    fclose(f);
    for (size_t i = 0; i < n;) {
        size_t len = strnlen(compatible + i, n - i);
        if (len == n - i)
            break;
        if (!strcmp(compatible + i, "rockchip,rk3588"))
            return 0;
        i += len + 1;
    }
    fprintf(stderr, "This driver requires a rockchip,rk3588 device tree.\n");
    return -1;
}

int inspect_ddr_config(FILE *out)
{
    if (check_platform())
        return -1;
    int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/mem");
        return -1;
    }
    const uint64_t address = UINT64_C(0xfd59e010);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        close(fd);
        return -1;
    }
    uint64_t aligned = address & ~((uint64_t)page_size - 1);
    void *map = mmap(NULL, (size_t)page_size, PROT_READ, MAP_SHARED, fd, (off_t)aligned);
    close(fd);
    if (map == MAP_FAILED) {
        perror("mmap CENTER_GRF");
        return -1;
    }
    struct probe p = {.regs = (volatile uint32_t *)((char *)map + (address - aligned))};
    uint32_t con4 = read_reg(&p, 0);
    munmap(map, (size_t)page_size);
    unsigned mode = (con4 >> 4) & 0x1f;
    fprintf(out, "CENTER_GRF_SOC_CON4 @ 0x%08" PRIx64 " = 0x%08x\n"
            "Channel address-map field [8:4] = 0x%02x\n", address, con4, mode);
    if (mode == 0)
        fprintf(out, "Four-channel interleaved address layout (TRM Part 2, section 1.3.5).\n");
    else if ((mode >= 1 && mode <= 8) || (mode >= 0x10 && mode <= 0x18))
        fprintf(out, "Non-interleaved channel address layout (TRM Part 2, Table 1-5).\n");
    else
        fprintf(out, "Unrecognized channel address-map encoding.\n");
    fprintf(out, "This field describes the channel address layout. DRAM bank/rank hashing\n"
            "and the interleave granule are not decoded: their controls are in\n"
            "DSU_SGRF/BUS_SGRF (TRM Part 2, section 1.5.6). No registers changed.\n");
    return 0;
}

int monitor_open(struct monitor *m, const struct schedule *s, bool read_only)
{
    memset(m, 0, sizeof(*m));
    m->mem_fd = m->lock_fd = -1;
    if (check_platform())
        return -1;
    if (!read_only) {
        m->lock_fd = open("/run/lock/rk3588-bw.lock",
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (m->lock_fd < 0 || flock(m->lock_fd, LOCK_EX | LOCK_NB)) {
            fprintf(stderr, "Cannot lock /run/lock/rk3588-bw.lock: %s\n",
                    strerror(errno));
            goto fail;
        }
    }
    m->mem_fd = open("/dev/mem", (read_only ? O_RDONLY : O_RDWR) | O_SYNC | O_CLOEXEC);
    if (m->mem_fd < 0) {
        fprintf(stderr, "Cannot open /dev/mem: %s (run as root; CONFIG_DEVMEM required)\n",
                strerror(errno));
        goto fail;
    }
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        fprintf(stderr, "Cannot determine page size.\n");
        goto fail;
    }
    for (unsigned i = 0; i < PROBE_COUNT; ++i) {
        if (s && !s->count[i])
            continue;
        struct probe *p = &m->probe[i];
        uint64_t base = probe_descs[i].address;
        uint64_t aligned = base & ~((uint64_t)page_size - 1);
        p->map_size = (size_t)page_size;
        p->mapping = mmap(NULL, p->map_size,
                          PROT_READ | (read_only ? 0 : PROT_WRITE),
                          MAP_SHARED, m->mem_fd, (off_t)aligned);
        if (p->mapping == MAP_FAILED) {
            p->mapping = NULL;
            fprintf(stderr, "Cannot map probe 0x%08" PRIx64 ": %s\n", base, strerror(errno));
            goto fail;
        }
        p->regs = (volatile uint32_t *)((char *)p->mapping + (base - aligned));
        uint32_t id = read_reg(p, CORE_ID), rev = read_reg(p, REVISION_ID);
        if (id != probe_descs[i].core_id || rev != 0x3c9d4e00) {
            fprintf(stderr, "Unexpected probe at 0x%08" PRIx64
                    ": ID=0x%08x revision=0x%08x; no registers changed.\n", base, id, rev);
            goto fail;
        }
    }
    return 0;
fail:
    monitor_close(m);
    return -1;
}

static int stop_probe(struct probe *p)
{
    write_reg(p, MAIN_CTL, 0);
    write_reg(p, CFG_CTL, 0);
    uint64_t deadline = monotonic_ns() + UINT64_C(10000000);
    while (read_reg(p, CFG_CTL) & ACTIVE) {
        if (monotonic_ns() >= deadline) {
            fprintf(stderr, "Probe did not become idle; refusing to reconfigure it.\n");
            return -1;
        }
    }
    return 0;
}

static void setup_filter(struct probe *p, unsigned filter)
{
    unsigned b = FILTER0 + FILTER_STRIDE * filter;
    for (unsigned o = 0; o <= 0x30; o += 4)
        write_reg(p, b + o, 0);
    write_reg(p, b + 0x10, 0x3f); /* full address window */
    write_reg(p, b + 0x1c, 0x0f); /* all opcodes */
    write_reg(p, b + 0x20, 0x03); /* requests and responses */
    write_reg(p, b + 0x24, 0x0f); /* all packet lengths */
    write_reg(p, b + 0x30, 0x7f); /* exact 7-bit MID match */
}

int monitor_claim(struct monitor *m)
{
    /* Check EVERY mapped probe before writing ANY of them. A running vendor
     * tool does not honor our lock; the hardware busy check also catches it.
     */
    for (unsigned i = 0; i < PROBE_COUNT; ++i) {
        struct probe *p = &m->probe[i];
        if (p->regs && (read_reg(p, MAIN_CTL) || (read_reg(p, CFG_CTL) & 3))) {
            fprintf(stderr, "Probe at 0x%08" PRIx64 " is in use. Stop the other monitor.\n"
                    "If a previous process was killed, --reset clears abandoned probes.\n",
                    probe_descs[i].address);
            return -1;
        }
    }
    for (unsigned i = 0; i < PROBE_COUNT; ++i) {
        struct probe *p = &m->probe[i];
        if (!p->regs)
            continue;
        for (size_t j = 0; j < ARRAY_SIZE(saved_offsets); ++j)
            if (applicable(i, saved_offsets[j]))
                p->saved[j] = read_reg(p, saved_offsets[j]);
        p->owned = true;
        if (stop_probe(p))
            return -1;
        write_reg(p, STAT_PERIOD, 0);
        for (unsigned c = 0; c < 4; ++c) {
            write_reg(p, COUNTER_SRC + c * COUNTER_STRIDE, 0);
            write_reg(p, COUNTER_ALARM + c * COUNTER_STRIDE, 0);
        }
        if (i < 8) {
            setup_filter(p, 0);
            setup_filter(p, 2);
        }
    }
    return 0;
}

int monitor_slice(struct monitor *m, const struct schedule *s, unsigned slot,
                  uint64_t duration_ns, struct measurement samples[MAX_MASTERS])
{
    for (unsigned i = 0; i < PROBE_COUNT; ++i) {
        struct probe *p = &m->probe[i];
        if (!p->owned)
            continue;
        for (unsigned pair = 0; pair < COUNTER_PAIRS; ++pair) {
            int idx = scheduled_master(s, i, slot, pair);
            p->master[pair] = idx;
            unsigned counter = pair * 2;
            uint32_t event = 0;
            if (idx >= 0) {
                int mid = masters[idx].mid;
                event = mid < 0 ? EVENT_BYTE : EVENT_FILTER_BYTE;
                if (mid >= 0)
                    write_reg(p, FILTER0 + counter * FILTER_STRIDE + 0x2c, (uint32_t)mid);
            }
            write_reg(p, COUNTER_SRC + counter * COUNTER_STRIDE, event);
            write_reg(p, COUNTER_SRC + (counter + 1) * COUNTER_STRIDE,
                      idx < 0 ? 0 : EVENT_CHAIN);
        }
    }
    for (unsigned i = 0; i < PROBE_COUNT; ++i) {
        struct probe *p = &m->probe[i];
        if (!p->owned)
            continue;
        write_reg(p, MAIN_CTL, STAT_EN); /* reset counters while globally disabled */
        uint64_t before = monotonic_ns();
        write_reg(p, CFG_CTL, GLOBAL_EN);
        p->start_ns = before + (monotonic_ns() - before) / 2;
    }
    uint64_t deadline_ns = monotonic_ns() + duration_ns;
    struct timespec deadline = {
        .tv_sec = (time_t)(deadline_ns / UINT64_C(1000000000)),
        .tv_nsec = (long)(deadline_ns % UINT64_C(1000000000)),
    };
    int error = 0;
    while (!interrupted) {
        error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        if (error != EINTR)
            break;
    }
    int result = error && error != EINTR ? -1 : 0;
    for (unsigned i = 0; i < PROBE_COUNT; ++i) {
        struct probe *p = &m->probe[i];
        if (!p->owned)
            continue;
        uint64_t before = monotonic_ns();
        write_reg(p, MAIN_CTL, 0); /* freeze before reading the 16-bit halves */
        uint64_t elapsed = before + (monotonic_ns() - before) / 2 - p->start_ns;
        if (stop_probe(p))
            result = -1;
        /* A pair is only 32 bits. At a conservative 64 GB/s per probe,
         * 50 ms is < 2^32 bytes. Never silently accept a scheduler stall
         * that could have caused an unobservable counter wrap.
         */
        if (elapsed > UINT64_C(50000000)) {
            fprintf(stderr, "Sampling stalled for %.1f ms; discarding this report to avoid counter wrap.\n",
                    (double)elapsed / 1e6);
            result = -1;
        }
        for (unsigned pair = 0; pair < COUNTER_PAIRS; ++pair) {
            int idx = p->master[pair];
            if (idx < 0)
                continue;
            unsigned reg = COUNTER_VAL + pair * 2 * COUNTER_STRIDE;
            uint32_t low = read_reg(p, reg) & 0xffff;
            uint32_t high = read_reg(p, reg + COUNTER_STRIDE) & 0xffff;
            samples[idx].bytes += low | (high << 16);
            samples[idx].active_ns += elapsed;
        }
    }
    if (result)
        return result;
    return interrupted ? 1 : 0;
}

void monitor_inspect(const struct monitor *m, FILE *out)
{
    fprintf(out, "probe   physical    core_id   revision   main_ctl   cfg_ctl\n");
    for (unsigned i = 0; i < PROBE_COUNT; ++i) {
        const struct probe *p = &m->probe[i];
        if (p->regs)
            fprintf(out, "p%u.%u    %08" PRIx64 "   %08x  %08x   %08x   %08x\n",
                    probe_descs[i].port, probe_descs[i].number, probe_descs[i].address,
                    read_reg(p, CORE_ID), read_reg(p, REVISION_ID),
                    read_reg(p, MAIN_CTL), read_reg(p, CFG_CTL));
    }
}

int monitor_reset(struct monitor *m)
{
    for (unsigned i = 0; i < PROBE_COUNT; ++i) {
        struct probe *p = &m->probe[i];
        if (!p->regs)
            continue;
        if (stop_probe(p))
            return -1;
        for (size_t j = 0; j < ARRAY_SIZE(saved_offsets); ++j)
            if (applicable(i, saved_offsets[j]))
                write_reg(p, saved_offsets[j], 0);
    }
    return 0;
}

int monitor_close(struct monitor *m)
{
    int result = 0;
    for (unsigned i = 0; i < PROBE_COUNT; ++i) {
        struct probe *p = &m->probe[i];
        if (p->owned) {
            if (stop_probe(p)) {
                result = -1;
            } else {
                for (size_t j = 2; j < ARRAY_SIZE(saved_offsets); ++j)
                    if (applicable(i, saved_offsets[j]))
                        write_reg(p, saved_offsets[j], p->saved[j]);
                write_reg(p, MAIN_CTL, p->saved[0]);
                write_reg(p, CFG_CTL, p->saved[1] & GLOBAL_EN);
            }
        }
        if (p->mapping)
            munmap(p->mapping, p->map_size);
        memset(p, 0, sizeof(*p));
    }
    if (m->mem_fd >= 0)
        close(m->mem_fd);
    if (m->lock_fd >= 0)
        close(m->lock_fd); /* don't unlink: another process may hold this inode */
    m->mem_fd = m->lock_fd = -1;
    return result;
}
