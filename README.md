100% vibe-coded

# rk3588-bw

An MIT-licensed userspace driver and command-line monitor for RK3588 DDR
bandwidth per IP. It programs the documented NoC packet probes through
`/dev/mem` and runs on mainline Linux. No vendor executable, vendor kernel
module, or DDR frequency override is needed.

## Build and run

```sh
make
sudo ./rk3588-bw

# Measure these four IPs simultaneously, with essentially full coverage:
sudo ./rk3588-bw -m cpu,gpu,npu,vop -d 1000 -n 10

# Individual GPU interfaces, in CSV:
sudo ./rk3588-bw -m gpu --per-master --csv

./rk3588-bw --list
sudo ./rk3588-bw --inspect
sudo ./rk3588-bw --ddr-config
```

The only build dependency is a C11 compiler and the system C library.
Cross compilation works with `make CC=aarch64-linux-gnu-gcc`. Optional
installation: `sudo make install PREFIX=/usr/local`.

The monitor requires root (or equivalent `/dev/mem` access), `CONFIG_DEVMEM`,
and permission to map these peripheral register ranges. It checks the device
tree for `rockchip,rk3588` and verifies all selected probe IDs before writing.
The tested kernel has `CONFIG_STRICT_DEVMEM=y` and
`CONFIG_IO_STRICT_DEVMEM` disabled. RK3588S and other SoCs are not currently
accepted; their support has not been validated.

`-d` sets the reporting interval in milliseconds; `-n` limits the number of
reports. Without `-n`, Ctrl-C stops the monitor. Diagnostics go to stderr,
and table/CSV data goes to stdout. To record data:

```sh
sudo ./rk3588-bw -m cpu,gpu,npu,vop --csv -n 60 > bandwidth.csv
```

## What the numbers mean

Each row reports read **plus** write payload bandwidth, in decimal MB/s
(1 MB = 1,000,000 bytes). A group adds the independently measured rates of
its master interfaces. The counters report traffic toward the DDR scheduler;
they do not count CPU cache hits, refresh commands, or DRAM bus overhead.
They are not a DDR utilization percentage. Reads and writes are combined.

`cpu` sums the four port-4 DSU probes, covering the CPU cluster's DDR path.
It is not a measurement per CPU core, process, or thread. `dsu0` through
`dsu3` in `--per-master` output are probe paths, not core numbers.

The other groups include GPU, NPU, VOP display, ISP, VICAP, video encoders
and decoders, RGA, JPEG, USB, storage, DMA, crypto, and the remaining
documented master IDs. `--list` prints every interface, MID, and probe.
There are 33 groups and 73 interfaces. `mmu600` represents the documented
MMU600 interfaces; it does not provide a breakdown by PCIe device. The USB
group covers the four interfaces listed by the TRM, not a claim that every
USB controller has a separate MID. Hardware paths sharing a master ID
cannot be separated by this tool.

Each peripheral probe has four 16-bit counters. Pairing them gives two
32-bit byte counters, hence two independently filtered MIDs at a time.
Selecting `cpu,gpu,npu,vop` fits in one slot. Selecting all groups currently
needs 12 slots on the busiest probe. Less busy probes repeat their own
shorter schedules independently.

`min coverage` is the smallest measured fraction of the report among that
row's interfaces. Each interface's rate is `observed_bytes / active_time`,
then the rates are added. For example, a rate measured over 8% of a report
is an estimate of the full report's average, not a full-interval byte count.
Short bursts may be missed, and differently sampled rows should not be
summed into an exact system total. Select only the IPs of interest to improve
coverage. CSV includes both minimum and maximum coverage.

Sampling windows are at most 20 ms even when the reporting interval is
60 seconds. Per-probe monotonic timestamps account for start/stop skew.
If scheduling delays a window beyond 50 ms, the program discards that
report and exits with an error instead of risking an undetected 32-bit
counter wrap. The guard allows a conservative 64 GB/s per probe. Small
configuration gaps and scheduling overhead make actual reporting intervals
slightly longer than requested; the output includes the measured duration.

## Register handling

The monitor maps only selected NoC probe blocks. It does not change DDR
timings, frequency, clocks, QoS, memory mappings, or the DFI monitor used by
mainline's `rockchip_ddr` perf events. Peripheral probes reside at
`0xfdf50000` through `0xfdf51c00`; DSU probes reside at `0xfe008c00`
through `0xfe009800`, with a stride of `0x400`.

MID filtering uses `Filters_UserBase/UserMask`, not `RouteId`. Filter 0
feeds counters 0/1, and filter 2 feeds counters 2/3, with
`FiltByteAlwaysChainableEn=0`. All opcodes, statuses, lengths and addresses
are allowed. Event `0x14` counts filtered payload bytes; event `0x08`
counts unfiltered DSU bytes; event `0x10` chains the upper half. The rising
edge of `MainCtl.StatEn` resets counters. The driver disables the probe
and waits for `CfgCtl.Active` to clear before changing filters.

The tool checks for occupied probes and locks `/run/lock/rk3588-bw.lock`
against other copies of itself. Do not run a vendor probe tool or another
NoC probe driver concurrently: those cannot participate in this lock.
Original configuration registers are restored on normal exit, errors,
SIGINT, SIGTERM, SIGHUP, and SIGQUIT. Historical counter values cannot be
restored. SIGKILL or a process crash can leave probes enabled. After
stopping all other probe users, recover with:

```sh
sudo ./rk3588-bw --reset
```

This explicitly clears the configuration of all 12 probes. It is not
performed automatically when a probe is busy.

## DDR interleaving

`--ddr-config` reads `CENTER_GRF_SOC_CON4` at `0xfd59e010` without writing
anything. On the development board, its value is `0x0000da0f`, with bits
`[8:4] = 0`. TRM Part 2, section 1.3.5 associates that setting with the
four-channel interleaved address layout.

This is a channel address-layout indication, not a complete decode of DRAM
bank or rank interleaving. The public TRM is inconsistent here: Part 1
marks the low bits reserved, while Part 2 describes the channel-map field.
Part 2, section 1.5.6 places the actual granule and bank/rank hash controls
in `DSU_SGRF` and `BUS_SGRF`. Their encoding is not provided in those public
register descriptions, so this tool does not access or guess those settings.

## Validation

```sh
make check                 # topology, scheduling, scaling and CLI; no MMIO
sudo make check-hardware   # opt-in live RK3588 tests, requires idle probes
```

The hardware tests check sampling and CSV coverage, read-only modes,
exclusive locking, configuration restoration after normal exits and signals,
and rejection of delayed samples. A separate live check routes the same
VOP stream through both filter/counter pairs and verifies matching 32-bit
counts. This last check needs active display scanout for a nonzero result.

Tested on an RK3588 NVR demo board running mainline Linux
`7.3.0-rc3-g4982d3552a3b-dirty`, on 2026-10-08:

| Workload | Observed result |
| --- | --- |
| Display scanout | About 498 MB/s attributed to VOP |
| Repeated CPU writes to a 128 MiB buffer | About 18 GB/s attributed to CPU; GPU and NPU remained zero |
| Vulkan fill/copy on Mali-G610, two 128 MiB buffers | About 25.5 GB/s attributed to GPU, matching the workload's byte rate |
| Vulkan compute reads on Mali-G610, one 512 MiB buffer | About 22 GB/s read-only; the probe measured about 22.1 GB/s read+write, including the kernel's checksum stores |
| Mainline DFI perf counters during CPU load | Collected concurrently without probe interference |

Other IP mappings follow the TRM; they have not all been exercised with
active workloads. Neither the original vendor executable nor its binary
contents are required or distributed.

## Hardware references

- [Rockchip RK3588 TRM v1.0 Part 2](https://dl.khadas.com/products/edge2/datasheet/rockchip-rk3588-trm-v1.0-part2.pdf):
  Chapter 1, Tables 1-3/1-4 for MIDs and probe addresses; sections 1.4.7/1.4.8
  for registers; section 1.5 for measurement procedures; sections 1.3.5/1.5.6
  for channel addressing and interleaving.
- [Rockchip RK3588 TRM v1.0 Part 1](https://dl.khadas.com/products/edge2/datasheet/rockchip-rk3588-trm-v1.0-part1.pdf):
  memory map and section 6.10 for `CENTER_GRF`.
- The local vendor kernel's `drivers/devfreq/event/rockchip-nocp.c` confirms
  the byte/chain counter model. Mainline's `rockchip-dfi.c` provides the
  independent whole-DDR perf interface used for cross-checking.

All driver source in this repository is under the MIT license in `LICENSE`.
Reference PDFs and vendor binaries are not part of the distribution.
