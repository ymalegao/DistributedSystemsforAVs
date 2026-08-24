#!/usr/bin/env python3
"""
pbft_matrix.py — structured PBFT-round analysis for resdb-simulation.log.

Built because ResDB's worker threads write to stdout without a shared lock,
so concurrent log lines interleave/concatenate (e.g. "self=4" + "self=5" from
two threads racing becomes "self=45" in the file). Ad-hoc grep/awk over that
silently produces wrong numbers. Every parser here is anchored to the FULL
line (^...$) and *drops* anything that doesn't match cleanly rather than
guessing — corrupted-line counts are reported so you know how much was
discarded.

Subcommands:
  matrix --phase PREPARE|COMMIT --seq N
      For each local replica (self=), the distinct senders whose vote it
      actually received for that phase+seq. Answers "did delivery fail
      randomly (different replicas missing different senders) or
      systematically (everyone missing the same senders)?"

  retry-arm --phase PREPARE|COMMIT --seq N
      Which replicas ever armed (attempted) a retry for that phase+seq —
      i.e. which replicas ever tried to send at all. If this set is much
      smaller than the full replica set, the replicas NOT listed never
      transitioned into that phase locally — a state-machine/quorum-gate
      issue, not a network delivery issue.

  quorum --seq N
      All [PBFT-QUORUM] lines for a seq, deduped, one per (self, count).

  forced-view [--seq N]
      All view-install lines (CANCEL-FORCED-VIEW, ROLLBACK-FORCED-VIEW,
      EPOCH-VIEW, EPOCH-VIEW-REJECT) — confirms whether/which forced view
      (f_override, N, quorum) was actually installed for a request, since
      the *proposer's own* [CANCEL-QUORUM]/[ROLLBACK-FORCED-VIEW] log is an
      application-side computation, not proof the bridge installed it.

  corruption --pattern REGEX
      Reports how many lines matching a loose version of REGEX fail to
      match the strict ^...$ anchored version — i.e. how much of that
      message class is unreadable due to interleaving, in this log.

Usage:
  python3 scripts/pbft_matrix.py /tmp/resdb-simulation.log matrix --phase COMMIT --seq 2
  python3 scripts/pbft_matrix.py /tmp/resdb-simulation.log retry-arm --phase COMMIT --seq 2
  python3 scripts/pbft_matrix.py /tmp/resdb-simulation.log forced-view --seq 2
"""
import argparse
import re
import sys
from collections import defaultdict

PHASE_LINE = {
    "PREPARE": re.compile(r"^\[PBFT-PREPARE\] self=(\d+) seq=(\d+) hash=(\S+) sender=(\d+)$"),
    "COMMIT": re.compile(r"^\[PBFT-COMMIT\] self=(\d+) seq=(\d+) hash=(\S+) sender=(\d+)$"),
}
RETRY_ARM_RE = re.compile(
    r"^\[PBFT-RETRY-ARM\] r(\d+) phase=TYPE_(PREPARE|COMMIT) view=(\d+) seq=(\d+) "
    r"interval=([\d.]+) max=(\d+) t=([\d.]+)$"
)
RETRY_FIRE_RE = re.compile(
    r"^\[PBFT-RETRY\] r(\d+) source=(\S+) phase=TYPE_(PREPARE|COMMIT) view=(\d+) seq=(\d+) attempt=(\d+)$"
)
QUORUM_RE = re.compile(
    r"^\[PBFT-QUORUM\] self=(\d+) seq=(\d+) hash=(\S+) source=(\S+) quorum=(\d+) N=(\d+) epoch=(\d+)$"
)
FORCED_VIEW_PATTERNS = [
    re.compile(r"^\[CANCEL-FORCED-VIEW\].*$"),
    re.compile(r"^\[ROLLBACK-FORCED-VIEW\].*$"),
    re.compile(r"^\[EPOCH-VIEW\].*$"),
    re.compile(r"^\[EPOCH-VIEW-REJECT\].*$"),
    re.compile(r"^\[CANCEL-QUORUM\].*$"),
]


def read_lines(path):
    with open(path, "r", errors="replace") as f:
        return f.readlines()


def strip_prefix(line):
    # Log lines are frequently prefixed "12345:" when piped through grep -n
    # upstream, but raw log lines have no such prefix. Strip both forms.
    line = line.rstrip("\n")
    m = re.match(r"^\d+:(.*)$", line)
    return m.group(1) if m else line


# Structural anchoring (^...$) catches interleaved lines that concatenate two
# whole log statements, but NOT the narrower case where two threads' writes
# race inside the same numeric field (e.g. real "self=4" and a concurrent
# "5" land as "self=45") — the line still parses cleanly, just with a wrong
# value. server.config never exceeds ~18 replicas, so anything outside this
# range in a replica/self/sender/N field is almost certainly that kind of
# corruption, not a real value. Used to drop implausible rows rather than
# report them as fact.
MAX_PLAUSIBLE_REPLICA_ID = 20


def plausible(n):
    return 0 <= n <= MAX_PLAUSIBLE_REPLICA_ID


def cmd_matrix(args, lines):
    pat = PHASE_LINE[args.phase]
    matrix = defaultdict(set)
    matched = 0
    dropped = 0
    for raw in lines:
        line = strip_prefix(raw)
        m = pat.match(line)
        if not m:
            continue
        self_id, seq, _hash, sender = m.groups()
        self_id, seq, sender = int(self_id), int(seq), int(sender)
        if seq != args.seq:
            continue
        if not (plausible(self_id) and plausible(sender)):
            dropped += 1
            continue
        matched += 1
        matrix[self_id].add(sender)
    if dropped:
        print(f"  (dropped {dropped} structurally-valid but implausible-value lines — "
              f"self/sender outside 0-{MAX_PLAUSIBLE_REPLICA_ID})")

    if not matrix:
        print(f"No clean {args.phase} seq={args.seq} lines found.")
        return

    print(f"=== {args.phase} delivery matrix, seq={args.seq} ({matched} clean lines) ===")
    all_selves = sorted(matrix.keys())
    for self_id in all_selves:
        senders = sorted(matrix[self_id])
        print(f"  self={self_id:>3}  n={len(senders):>2}  senders={senders}")

    # Flag systematic vs random: intersection vs union of sender sets.
    all_sender_sets = list(matrix.values())
    common = set.intersection(*all_sender_sets) if all_sender_sets else set()
    union = set.union(*all_sender_sets) if all_sender_sets else set()
    print()
    print(f"  senders common to EVERY replica's view: {sorted(common)}")
    print(f"  senders seen by AT LEAST ONE replica:   {sorted(union)}")
    if common == union and len(all_sender_sets) > 1:
        print("  -> identical sender set everywhere: SYSTEMATIC (not random per-link loss)")
    elif len(union) > len(common):
        print("  -> sender sets differ across replicas: looks like per-link/random loss")


def cmd_retry_arm(args, lines):
    armed = defaultdict(set)  # phase -> {replica ids that ever armed}
    matched = 0
    dropped = 0
    for raw in lines:
        line = strip_prefix(raw)
        m = RETRY_ARM_RE.match(line)
        if not m:
            continue
        rid, phase, _view, seq, _interval, _max, _t = m.groups()
        rid, seq = int(rid), int(seq)
        if seq != args.seq:
            continue
        if not plausible(rid):
            dropped += 1
            continue
        matched += 1
        armed[phase].add(rid)
    if dropped:
        print(f"  (dropped {dropped} implausible-value ARM lines)")

    phase = args.phase
    ids = sorted(armed.get(phase, set()))
    print(f"=== retry-arm for phase={phase} seq={args.seq} ({matched} clean ARM lines total) ===")
    print(f"  replicas that EVER armed a retry (i.e. attempted to send): {ids}")
    print(f"  count: {len(ids)}")


def cmd_quorum(args, lines):
    seen = set()
    matched = 0
    dropped = 0
    for raw in lines:
        line = strip_prefix(raw)
        m = QUORUM_RE.match(line)
        if not m:
            continue
        self_id, seq, _hash, source, quorum, n, epoch = m.groups()
        self_id, seq, quorum, n = int(self_id), int(seq), int(quorum), int(n)
        if seq != args.seq:
            continue
        if not (plausible(self_id) and plausible(quorum) and plausible(n)):
            dropped += 1
            continue
        matched += 1
        seen.add((self_id, source, quorum, n, epoch))
    if dropped:
        print(f"  (dropped {dropped} implausible-value QUORUM lines)")

    print(f"=== PBFT-QUORUM reached, seq={args.seq} ({matched} clean lines) ===")
    for self_id, source, quorum, n, epoch in sorted(seen):
        print(f"  self={self_id:>3}  source={source:<8} quorum={quorum} N={n} epoch={epoch}")
    if not seen:
        print("  (quorum was never locally reached by anyone for this seq)")


def cmd_forced_view(args, lines):
    print(f"=== forced-view / quorum-install lines{' (seq filter not applied — greps whole line)' if args.seq is None else ''} ===")
    for i, raw in enumerate(lines, 1):
        line = strip_prefix(raw)
        for pat in FORCED_VIEW_PATTERNS:
            if pat.match(line):
                if args.seq is not None and f"seq={args.seq}" not in line and f"cancelled_epoch={args.seq}" not in line:
                    continue
                print(f"  {i}: {line}")
                break


def cmd_corruption(args, lines):
    loose = re.compile(args.pattern)
    strict_candidates = {
        "PBFT-COMMIT": PHASE_LINE["COMMIT"],
        "PBFT-PREPARE": PHASE_LINE["PREPARE"],
    }
    loose_count = 0
    strict_count = 0
    strict_re = None
    for name, pat in strict_candidates.items():
        if name in args.pattern:
            strict_re = pat
            break
    for raw in lines:
        line = strip_prefix(raw)
        if loose.search(line):
            loose_count += 1
            if strict_re and strict_re.match(line):
                strict_count += 1
    print(f"=== corruption check for /{args.pattern}/ ===")
    print(f"  lines loosely matching:  {loose_count}")
    if strict_re:
        print(f"  lines cleanly parseable: {strict_count}")
        if loose_count:
            pct = 100.0 * (loose_count - strict_count) / loose_count
            print(f"  corrupted/unparseable:   {loose_count - strict_count} ({pct:.1f}%)")
    else:
        print("  (no strict parser registered for this pattern — add one to compare)")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("log", help="path to resdb-simulation.log")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("matrix", help="per-replica delivery matrix for a phase+seq")
    sp.add_argument("--phase", choices=["PREPARE", "COMMIT"], required=True)
    sp.add_argument("--seq", type=int, required=True)
    sp.set_defaults(func=cmd_matrix)

    sp = sub.add_parser("retry-arm", help="which replicas ever attempted to send for a phase+seq")
    sp.add_argument("--phase", choices=["PREPARE", "COMMIT"], required=True)
    sp.add_argument("--seq", type=int, required=True)
    sp.set_defaults(func=cmd_retry_arm)

    sp = sub.add_parser("quorum", help="who locally reached quorum for a seq")
    sp.add_argument("--seq", type=int, required=True)
    sp.set_defaults(func=cmd_quorum)

    sp = sub.add_parser("forced-view", help="view-install / forced-view lines")
    sp.add_argument("--seq", type=int, default=None)
    sp.set_defaults(func=cmd_forced_view)

    sp = sub.add_parser("corruption", help="how much of a message class is interleaving-corrupted")
    sp.add_argument("--pattern", required=True, help="substring/regex to loosely match, e.g. PBFT-COMMIT")
    sp.set_defaults(func=cmd_corruption)

    args = p.parse_args()
    lines = read_lines(args.log)
    args.func(args, lines)


if __name__ == "__main__":
    main()
