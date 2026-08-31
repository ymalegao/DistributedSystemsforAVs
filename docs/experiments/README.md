# Experiment documentation

`commands.md` is the runnable command catalog. Each experiment directory under
`experiments/` contains the authoritative manifest and a short scope README;
this directory will gain one paper-facing page per experiment as artifacts are
curated.

Current execution state is tracked in:

- [`../../experiments/STATUS.md`](../../experiments/STATUS.md) — human-readable
  completed/remaining ledger and recommended next order;
- [`../../experiments/registry.json`](../../experiments/registry.json) —
  machine-readable statuses, evidence paths, and next commands.

Small reviewed summaries and aggregate tables belong under each experiment's
`artifacts/curated/`. Raw generated runs live in the owning experiment's
ignored `results/` directory. The legacy `benchmarks/<run-root>` names are
compatibility symlinks, so existing runner, resume, analyzer, and plot commands
continue to resolve the same files without duplicating raw data.
