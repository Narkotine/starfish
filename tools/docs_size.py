#!/usr/bin/env python3
"""Keep the docs a session reads at startup short enough to be read whole.

STATE.md and TODO.md are read at the beginning of every session. Past a few
tens of kilobytes, an agent stops reading them and greps the first screens
instead -- the protocol keeps saying "lire STATE.md" while nobody does. So each
file that every session opens has a cap, its detail lives in a file named by the
cap's message, and the cap is *checked* (here, in CI) rather than trusted.

Raise a cap only with a reason; moving the detail is almost always the fix.
"""

from __future__ import annotations

import sys
from pathlib import Path

CAPS = [
    ("STATE.md", 6_000, "read at every session; the detail belongs in docs/state/"),
    ("TODO.md", 50_000, "only open tasks; finished ones go to docs/todo-archive.md"),
    ("README.md", 45_000, "the map of the docs, not a second STATE.md"),
]


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    failures = []
    for name, cap, why in CAPS:
        path = root / name
        if not path.is_file():
            failures.append(f"MISSING  {name}")
            continue
        size = path.stat().st_size
        if size > cap:
            failures.append(f"TOO BIG  {name:<10} {size:7d} > {cap}  ({why})")
        else:
            print(f"ok       {name:<10} {size:7d} <= {cap}")

    for line in failures:
        print(line)
    if failures:
        print("\nMove the detail (see the reason above each cap); do not raise the cap.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
