#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Hardware-independent CLI checks. No successful monitor invocation here."""
import subprocess
import unittest


class CLI(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run(["./rk3588-bw", *args], capture_output=True, text=True, timeout=5)

    def test_help(self):
        p = self.run_cli("--help")
        self.assertEqual(p.returncode, 0)
        self.assertIn("--masters", p.stdout)

    def test_list_contains_cpu_and_mid_zero(self):
        p = self.run_cli("--list")
        self.assertEqual(p.returncode, 0)
        self.assertIn("dsu3", p.stdout)
        self.assertRegex(p.stdout, r"av1\s+av1\s+0x00\s+p2\.0")

    def test_invalid_arguments_before_hardware_access(self):
        cases = [
            ["--masters", "imaginary"], ["--masters", ""],
            ["--interval", "nan"], ["--interval", "0"], ["--interval", "9"],
            ["--interval", "1.5"], ["--interval", "60001"],
            ["--count", "-1"], ["--count", "2extra"],
            ["--count", "9999999999999999999999999"],
            ["--inspect", "--reset"], ["--reset", "--masters", "cpu"],
            ["--ddr-config", "--inspect"], ["--ddr-config", "--csv"],
            ["--list", "--csv"], ["extra-argument"], ["--bogus"],
        ]
        for args in cases:
            with self.subTest(args=args):
                p = self.run_cli(*args)
                self.assertEqual(p.returncode, 2, p.stderr)
                self.assertNotIn("/dev/mem", p.stderr)


if __name__ == "__main__":
    unittest.main()
