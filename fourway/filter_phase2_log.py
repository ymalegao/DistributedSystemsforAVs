#!/usr/bin/env python3
"""Keep analyzer-consumed records while discarding verbose protocol tracing.

The filter derives bracketed record names from analyze_log.py itself so adding
a new analyzer regex automatically keeps its corresponding log record. It is
used only by the large Phase 2 pilot preset; validation presets retain full
logs.
"""

from pathlib import Path
import re
import sys


ANALYZER = Path(__file__).with_name("analyze_log.py")
TAG_PATTERN = re.compile(r"\\\[([A-Za-z][A-Za-z0-9_-]+)")
LINE_TAG_PATTERN = re.compile(r"\[([A-Za-z][A-Za-z0-9_-]+)(?:\s|\])")
ALWAYS_KEEP = (
    "Teleporting vehicle",
    "Simulation stopped",
    "Error:",
    "<!>",
    "[ANN-ORIGIN-INVALID]",
    "[CERT-INVALID]",
    "verify FAIL",
    "[METROLOGY-END]",
    "[SCHEDULER-BATCH-ADMIT]",
    "[SCHEDULER-MODE]",
    "[DIRECTION-ABLATION-CONFIG]",
    "[ATTACK-PROPOSER]",
    "[BYZANTINE]",
    "[OMNET-PREVERIFY]",
    "[ACTIVE-VIEW]",
    "[VC-DEBUG]",
    "[PROPOSER-CERT-STATE]",
    "[CERT-RELAY-WITHHELD]",
    "[CERT-GOSSIP-WITHHELD]",
    "[CERT-RELAY-MODE]",
    "[DISCOVERY-COMPLETE]",
    "[CERT-RELAY]",
    "[EXECUTOR]",
    "[CONSENSUS_ATTACK_OUTCOME]",
)


def analyzer_tags() -> set[str]:
    return set(TAG_PATTERN.findall(ANALYZER.read_text()))


def main() -> int:
    tags = analyzer_tags()
    for line in sys.stdin:
        match = LINE_TAG_PATTERN.search(line)
        if (match and match.group(1) in tags) or any(token in line for token in ALWAYS_KEEP):
            sys.stdout.write(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
