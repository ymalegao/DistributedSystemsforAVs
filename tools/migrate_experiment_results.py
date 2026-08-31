#!/usr/bin/env python3
"""Move raw experiment trees under experiments/ while preserving old paths."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
MAP_PATH = REPO_ROOT / "experiments" / "result_roots.json"


def load_roots() -> tuple[Path, dict[str, str]]:
    payload = json.loads(MAP_PATH.read_text())
    compatibility_root = REPO_ROOT / payload["compatibility_root"]
    return compatibility_root, payload["roots"]


def migrate(*, apply: bool) -> int:
    compatibility_root, roots = load_roots()
    failures: list[str] = []
    moved = linked = already = 0

    for name, owner in sorted(roots.items()):
        source = compatibility_root / name
        destination = REPO_ROOT / "experiments" / owner / "results" / name
        relative_target = Path(os.path.relpath(destination, source.parent))

        if source.is_symlink():
            if source.readlink() != relative_target or not destination.is_dir():
                failures.append(f"invalid compatibility link: {source}")
            else:
                already += 1
            continue

        if not source.exists():
            if destination.is_dir():
                if apply:
                    source.symlink_to(relative_target, target_is_directory=True)
                linked += 1
            continue

        if not source.is_dir():
            failures.append(f"source is not a directory: {source}")
            continue
        if destination.exists():
            failures.append(f"destination already exists: {destination}")
            continue

        print(f"{source.relative_to(REPO_ROOT)} -> {destination.relative_to(REPO_ROOT)}")
        if apply:
            destination.parent.mkdir(parents=True, exist_ok=True)
            source.rename(destination)
            source.symlink_to(relative_target, target_is_directory=True)
        moved += 1

    if failures:
        for failure in failures:
            print(f"ERROR: {failure}")
        return 1

    mode = "applied" if apply else "dry-run"
    print(f"Migration {mode}: moved={moved} linked={linked} already={already}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--apply",
        action="store_true",
        help="perform atomic moves and create compatibility symlinks",
    )
    args = parser.parse_args()
    return migrate(apply=args.apply)


if __name__ == "__main__":
    raise SystemExit(main())
