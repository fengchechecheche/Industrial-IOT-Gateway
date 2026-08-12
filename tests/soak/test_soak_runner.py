from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.soak.evidence import RotatingTextWriter  # noqa: E402
from tools.soak.process import ManagedProcess  # noqa: E402


class SoakRunnerUnitTest(unittest.TestCase):
    def test_managed_process_is_stopped_within_budget(self) -> None:
        process = ManagedProcess(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        returncode = process.stop(0.2)
        self.assertIsNotNone(returncode)
        self.assertIsNotNone(process.poll())

    def test_rotating_writer_never_overwrites_previous_segments(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            writer = RotatingTextWriter(directory, "events", "jsonl", 12)
            writer.write("1234567890\n")
            writer.write("abcdefghij\n")
            writer.close()
            paths = sorted(directory.glob("events_*.jsonl"))
            self.assertEqual(len(paths), 2)
            self.assertEqual(paths[0].read_text(encoding="utf-8"), "1234567890\n")
            self.assertEqual(paths[1].read_text(encoding="utf-8"), "abcdefghij\n")


if __name__ == "__main__":
    unittest.main()
