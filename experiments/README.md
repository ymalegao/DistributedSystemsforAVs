# PicaBFT experiments

This tree owns experiment definitions, fixtures, status, and curated results.
It deliberately separates source-controlled experiment evidence from local raw
simulation output.

- [`STATUS.md`](STATUS.md) is the human-readable run ledger and next-work list.
- [`registry.json`](registry.json) is the machine-readable source of truth.
- Each experiment's `manifest.json` defines its axes, commands, and component
  status.
- `artifacts/curated/` contains small, reviewed summaries and aggregate tables
  selected for version control.
- Raw per-run logs, channel traces, keys, `.vec/.sca`, and analyzer dumps live
  in each experiment's ignored `results/` directory. Compatibility symlinks
  under `benchmarks/` preserve existing runner, resume, analyzer, and plotting
  paths. [`result_roots.json`](result_roots.json) is the ownership map; run
  `python3 tools/migrate_experiment_results.py` to audit it or add `--apply` to
  perform an idempotent migration.

Status vocabulary:

| Status | Meaning |
|---|---|
| `FULL_PASS` | Planned statistical matrix completed and passed. |
| `SMOKE_PASS` | Wiring/validation smoke passed; statistical matrix remains. |
| `PARTIAL` | Some rows are complete, but at least one required row is missing. |
| `HARNESS_READY` | Runnable harness exists but the required run is not complete. |
| `PLANNED` | Design is recorded; implementation is not complete. |
| `DEFERRED` | Explicitly outside the current submission scope. |

Do not mark an experiment `FULL_PASS` from the existence of a directory. The
registry must name a summary artifact whose `passed` field is true and record
the planned/completed run counts when the harness provides them.

Validate the ledger and curated evidence with:

```bash
python3 experiments/validate_registry.py
```
