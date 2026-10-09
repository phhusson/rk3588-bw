#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Opt-in RK3588 integration test. Requires exclusive access to idle probes."""
import csv
import io
import mmap
import os
import signal
import struct
import subprocess
import time

BASES = [0xFDF50000 + i * 0x400 for i in range(8)] + [0xFE008C00 + i * 0x400 for i in range(4)]
COMMON = [8, 12, 0x24] + [0x204 + 0x10 * i + offset for i in range(4) for offset in (0, 4)]
FILTERS = list(range(0x80, 0xB4, 4)) + list(range(0x140, 0x174, 4))


def snapshot():
    result = {}
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    try:
        for i, base in enumerate(BASES):
            page = base & ~(mmap.PAGESIZE - 1)
            with mmap.mmap(fd, mmap.PAGESIZE, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ, offset=page) as mem:
                for offset in COMMON + (FILTERS if i < 8 else []):
                    result[base + offset] = struct.unpack_from("<I", mem, base - page + offset)[0]
    finally:
        os.close(fd)
    return result


def run(*args):
    return subprocess.run(["./rk3588-bw", *args], capture_output=True, text=True, timeout=10)


before = snapshot()
assert all(before[b + 8] == 0 and before[b + 12] == 0 for b in BASES), "Probes already in use"
for args in (("--inspect",), ("--ddr-config",)):
    p = run(*args)
    assert p.returncode == 0, p.stderr
    assert snapshot() == before, "Read-only mode changed registers"

for selection in ("all", "cpu,gpu,npu,vop"):
    p = run("-m", selection, "--csv", "-n", "2", "-d", "200")
    assert p.returncode == 0, p.stderr
    rows = list(csv.DictReader(io.StringIO(p.stdout)))
    assert {r["sample"] for r in rows} == {"1", "2"}
    for row in rows:
        assert float(row["mb_per_s"]) >= 0
        assert 0 < float(row["min_coverage_pct"]) <= float(row["max_coverage_pct"]) <= 100
        if selection != "all":
            assert float(row["min_coverage_pct"]) > 95
    assert snapshot() == before, "Normal exit did not restore configuration"

for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT):
    p = subprocess.Popen(["./rk3588-bw", "--csv"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert "RK3588 NoC" in p.stderr.readline()
        contender = run("-m", "cpu", "-n", "1", "-d", "10")
        assert contender.returncode == 1 and "Cannot lock" in contender.stderr
        p.send_signal(sig)
        out, err = p.communicate(timeout=5)
        assert p.returncode == 128 + sig, err
    finally:
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
            p.communicate(timeout=5)
    assert snapshot() == before, f"Configuration not restored after signal {sig}"

# An undetectable 32-bit wrap must not become a plausible low bandwidth.
p = subprocess.Popen(["./rk3588-bw", "-m", "cpu", "--csv"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    assert "RK3588 NoC" in p.stderr.readline()
    time.sleep(0.03)
    p.send_signal(signal.SIGSTOP)
    time.sleep(0.12)
    p.send_signal(signal.SIGCONT)
    out, err = p.communicate(timeout=5)
    assert p.returncode == 1 and "Sampling stalled" in err, err
    assert len(out.splitlines()) == 1, "Stalled sample emitted as valid data"
finally:
    if p.poll() is None:
        p.send_signal(signal.SIGCONT)
        p.send_signal(signal.SIGTERM)
        p.communicate(timeout=5)
assert snapshot() == before, "Stall recovery did not restore configuration"
print("Live sampling, CSV coverage, read-only modes, locking, signals, stall detection and register restoration passed.")
