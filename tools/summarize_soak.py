#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib

from soak.summary import summarize


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize Industrial-IOT-Gateway soak evidence")
    parser.add_argument("evidence_directory", type=pathlib.Path)
    arguments = parser.parse_args()
    summary = summarize(arguments.evidence_directory)
    print(f"SOAK_STATUS={summary['status']}")
    print(f"LONG_SOAK_PASS={str(summary['long_soak_pass']).lower()}")
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
