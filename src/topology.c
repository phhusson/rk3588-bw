/* SPDX-License-Identifier: MIT */
#include "topology.h"

#include <string.h>

/* RK3588 TRM v1.0 Part 2, Chapter 1, Tables 1-3 and 1-4.
 * Core checksums are read from the RK3588, not all the same as the example
 * checksum in the TRM. No QoS, scheduler, clock or DDR control is changed.
 */
const struct probe_desc probe_descs[PROBE_COUNT] = {
    {0xfdf50000, 0x194c4006, 0, 0},
    {0xfdf50400, 0xc3f47406, 0, 1},
    {0xfdf50800, 0x520b0406, 1, 0},
    {0xfdf50c00, 0xdca34806, 1, 1},
    {0xfdf51000, 0xbb6f3c06, 2, 0},
    {0xfdf51400, 0xbd05f906, 2, 1},
    {0xfdf51800, 0x86b97e06, 3, 0},
    {0xfdf51c00, 0xadf4e706, 3, 1},
    {0xfe008c00, 0x98cc1906, 4, 0},
    {0xfe009000, 0xb3764606, 4, 1},
    {0xfe009400, 0x53acd106, 4, 2},
    {0xfe009800, 0x9bc40906, 4, 3},
};

#define M(group, name, port, probe, mid) \
    {group, name, (port) * 2 + (probe), mid}

/* Keep each group's entries contiguous. MIDs are the 7-bit packet.User
 * value, NOT RouteId and NOT an IOMMU stream ID. MID 0 (AV1) is valid.
 */
const struct master_desc masters[] = {
    {"cpu", "dsu0", 8, -1},
    {"cpu", "dsu1", 9, -1},
    {"cpu", "dsu2", 10, -1},
    {"cpu", "dsu3", 11, -1},
    M("gpu", "gpu_m0", 2, 0, 0x15),
    M("gpu", "gpu_m1", 3, 0, 0x16),
    M("gpu", "gpu_m2", 0, 0, 0x17),
    M("gpu", "gpu_m3", 1, 0, 0x18),
    M("npu", "npu0_rd", 2, 0, 0x2d),
    M("npu", "npu0_wr", 1, 1, 0x2c),
    M("npu", "npu1", 3, 0, 0x2e),
    M("npu", "npu2", 0, 0, 0x2f),
    M("vop", "vop_m0", 0, 1, 0x46),
    M("vop", "vop_m1", 1, 1, 0x47),
    M("isp", "isp0_rd", 3, 0, 0x1d),
    M("isp", "isp0_wr", 2, 1, 0x1d),
    M("isp", "isp1_rd", 2, 0, 0x1e),
    M("isp", "isp1_wr", 3, 1, 0x1e),
    M("vicap", "vicap_m0", 1, 1, 0x44),
    M("vicap", "vicap_m1", 0, 0, 0x45),
    M("rkvdec", "rkvdec0", 2, 0, 0x34),
    M("rkvdec", "rkvdec1", 3, 0, 0x35),
    M("rkvenc", "rkvenc0_m0_rd", 1, 0, 0x36),
    M("rkvenc", "rkvenc0_m1_rd", 0, 0, 0x37),
    M("rkvenc", "rkvenc0_m2_wr", 0, 0, 0x38),
    M("rkvenc", "rkvenc1_m0_rd", 0, 0, 0x39),
    M("rkvenc", "rkvenc1_m1_rd", 1, 0, 0x3a),
    M("rkvenc", "rkvenc1_m2_wr", 1, 0, 0x3b),
    M("rga", "rga2_rd", 3, 0, 0x30),
    M("rga", "rga2_wr", 3, 0, 0x31),
    M("rga", "rga3_0", 2, 0, 0x32),
    M("rga", "rga3_1", 1, 0, 0x33),
    M("fisheye", "fisheye0", 1, 0, 0x0f),
    M("fisheye", "fisheye1", 0, 0, 0x10),
    M("av1", "av1", 2, 0, 0x00),
    M("vdpu", "vdpu", 2, 0, 0x43),
    M("iep", "iep", 3, 0, 0x1c),
    M("jpeg_dec", "jpeg_dec", 3, 0, 0x1f),
    M("jpeg_enc", "jpeg_enc0", 2, 0, 0x20),
    M("jpeg_enc", "jpeg_enc1", 2, 0, 0x21),
    M("jpeg_enc", "jpeg_enc2", 3, 0, 0x22),
    M("jpeg_enc", "jpeg_enc3", 3, 0, 0x23),
    M("hdcp", "hdcp0", 2, 0, 0x19),
    M("hdcp", "hdcp1", 2, 0, 0x1a),
    M("hdmirx", "hdmirx", 2, 0, 0x1b),
    M("usb", "usb3_0", 2, 0, 0x40),
    M("usb", "usb3_1", 2, 0, 0x41),
    M("usb", "usb2host_0", 2, 0, 0x3e),
    M("usb", "usb2host_1", 2, 0, 0x3f),
    M("emmc", "emmc", 3, 0, 0x0e),
    M("fspi", "fspi", 3, 0, 0x11),
    M("sdio", "sdio", 3, 0, 0x3c),
    M("sdmmc", "sdmmc", 2, 0, 0x3d),
    M("dmac", "dmac0", 1, 0, 0x08),
    M("dmac", "dmac1", 1, 0, 0x09),
    M("dmac", "dmac2", 1, 0, 0x0a),
    M("decom", "decom", 1, 0, 0x05),
    M("crypto", "crypto_s", 2, 0, 0x02),
    M("crypto", "crypto_ns", 2, 0, 0x01),
    M("gic", "gic600_m0", 3, 0, 0x12),
    M("gic", "gic600_m1", 2, 0, 0x13),
    M("gic", "gic600m", 1, 0, 0x14),
    M("mmu600", "mmu600pcie_tcu", 3, 0, 0x29),
    M("mmu600", "mmu600php_tbu", 3, 0, 0x2a),
    M("mmu600", "mmu600php_tcu", 3, 0, 0x2b),
    M("mcu", "mcu_npu", 0, 0, 0x27),
    M("mcu", "mcu_ddr", 0, 0, 0x26),
    M("mcu", "mcu_pmu", 2, 0, 0x28),
    M("dma2ddr", "dma2ddr", 0, 0, 0x07),
    M("vad", "vad", 2, 0, 0x42),
    M("dcf", "dcf", 2, 0, 0x03),
    M("dsu_mp", "dsu_mp", 3, 0, 0x0d),
    M("debug", "debug", 3, 0, 0x04),
};
#undef M

const size_t master_count = sizeof(masters) / sizeof(masters[0]);
_Static_assert(sizeof(masters) / sizeof(masters[0]) <= MAX_MASTERS,
               "increase MAX_MASTERS");

void make_schedule(struct schedule *s, const bool selected[MAX_MASTERS])
{
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; i < master_count; ++i) {
        unsigned p = masters[i].probe;
        if (selected[i])
            s->master[p][s->count[p]++] = i;
    }
    for (unsigned p = 0; p < PROBE_COUNT; ++p) {
        unsigned slots = (s->count[p] + 1) / COUNTER_PAIRS;
        if (slots > s->slots)
            s->slots = slots;
    }
}

int scheduled_master(const struct schedule *s, unsigned p, unsigned slot,
                     unsigned pair)
{
    unsigned slots = (s->count[p] + 1) / COUNTER_PAIRS;
    if (!slots)
        return -1;
    unsigned n = (slot % slots) * COUNTER_PAIRS + pair;
    return n < s->count[p] ? (int)s->master[p][n] : -1;
}

double bandwidth_mbps(const struct measurement *m)
{
    return m->active_ns ? (double)m->bytes * 1000.0 / (double)m->active_ns : 0;
}

double coverage_percent(const struct measurement *m, uint64_t wall_ns)
{
    return wall_ns ? (double)m->active_ns * 100.0 / (double)wall_ns : 0;
}
