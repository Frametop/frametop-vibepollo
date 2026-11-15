#!/usr/bin/env python3
"""vibepollo-host-input-default: host input is turned on once, and the owner's choice stands."""

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[4] / "packaging/linux/vibepollo-host-input-default"

FAKE_SYSTEMCTL = """#!/bin/sh
echo "$*" >>"$FAKE_DIR/calls"
case "$1" in
  is-enabled) cat "$FAKE_DIR/state" 2>/dev/null; exit 1 ;;
  enable) exit "$(cat "$FAKE_DIR/enable_rc" 2>/dev/null || echo 0)" ;;
esac
exit 0
"""


class HostInputDefault(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.dir = Path(self.temporary.name)
        self.marker = self.dir / "marker"
        self.systemd = self.dir / "run-systemd"
        self.systemd.mkdir()
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        systemctl = bin_dir / "systemctl"
        systemctl.write_text(FAKE_SYSTEMCTL)
        systemctl.chmod(stat.S_IRWXU)
        # Only the two machine paths change; the logic under test is the shipped script's.
        script = SOURCE.read_text()
        for old, new in (("marker=/var/lib/vibepollo-host-input-default", f"marker={self.marker}"),
                         ("[ -d /run/systemd/system ]", f"[ -d {self.systemd} ]")):
            self.assertEqual(script.count(old), 1, old)
            script = script.replace(old, new)
        self.script = self.dir / "vibepollo-host-input-default"
        self.script.write_text(script)
        self.script.chmod(stat.S_IRWXU)
        self.env = dict(os.environ, PATH=f"{bin_dir}:/usr/bin:/bin", FAKE_DIR=str(self.dir))

    def tearDown(self):
        self.temporary.cleanup()

    def run_script(self):
        result = subprocess.run([str(self.script)], env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def calls(self):
        path = self.dir / "calls"
        return path.read_text().splitlines() if path.exists() else []

    def test_first_run_turns_it_on_and_records_it(self):
        (self.dir / "state").write_text("disabled\n")
        self.run_script()
        self.assertIn("enable --now vibepollo-host-input.socket", self.calls())
        self.assertTrue(self.marker.exists())

    def test_owner_choice_stands_after_the_first_run(self):
        self.marker.touch()
        (self.dir / "state").write_text("disabled\n")
        self.run_script()
        self.assertEqual(self.calls(), [])

    def test_a_masked_socket_is_left_masked(self):
        (self.dir / "state").write_text("masked\n")
        self.run_script()
        self.assertFalse(any(call.startswith("enable") for call in self.calls()))
        self.assertTrue(self.marker.exists())

    def test_a_failed_enable_is_retried_next_time(self):
        (self.dir / "state").write_text("disabled\n")
        (self.dir / "enable_rc").write_text("1")
        result = self.run_script()
        self.assertIn("could not turn on Frametop host input", result.stderr)
        self.assertFalse(self.marker.exists())

    def test_without_systemd_nothing_happens(self):
        self.systemd.rmdir()
        self.run_script()
        self.assertEqual(self.calls(), [])
        self.assertFalse(self.marker.exists())


if __name__ == "__main__":
    unittest.main()
