# V2V-BFT — Byzantine fault-tolerant intersection management

BFT consensus for autonomous vehicles at an unsignalled intersection. Vehicles
agree on a safe crossing order using PBFT, with an **arrival-certificate** layer
that proves a vehicle really is where it claims to be before its entry can enter
a proposal.

Each vehicle is one OMNeT++ module embedding its own real ResilientDB PBFT
replica, talking to its neighbours over a simulated 802.11p radio, driving a
real SUMO vehicle over TraCI. Nothing about the consensus is mocked.

```
SUMO ──TraCI──> Veins / OMNeT++ ──802.11p──> ResDBIntersectionApp ──C ABI──> ResilientDB PBFT
   mobility        radio + PHY                  the protocol                   consensus
```

## Layout

```
src/v2vbft/           the protocol — this is the interesting part
  app/                  arrival certs, ordering, rollback, transport, TraCI glue
  protocol/             shared types: Primitives, Arrival, Rollback, OrderCandidate
  crypto/               ECDSA P-256 signing, ambulance certificates
  messages/             the OMNeT++ wire message
  sinr/                 per-vehicle channel metrics
bridge/               C ABI bridge into ResilientDB: pre-verification firewall,
                      the intersection executor, deterministic batching table
scenarios/fourway/    omnetpp.ini, SUMO networks and routes, radio config
tools/                run script, key generation, Bazel wrapper, patch regen
plotter/              figure generation (one pipeline, see below)
tests/golden/         structural-invariant harness for safe refactoring
benchmarks/ablations/ the ablation runner
third_party/          pinned upstream refs + our patches to them
docs/                 ARCHITECTURE.md and the CLEAR/WAIT spec
.external/            (gitignored) Veins and ResilientDB, materialised by setup.sh
```

Everything this project defines lives in C++ namespace **`v2vbft`** and NED
package **`org.v2vbft`**. Nothing is added to Veins' namespace or to global
scope, so `libv2vbft.so` can be loaded alongside stock Veins without collisions.
`src/` is the C++ include root (`#include "v2vbft/app/..."`) while `src/v2vbft`
is the NED root — the same split Veins uses between `src` and `src/veins`.

**Veins and ResilientDB are not vendored into this repo.** `third_party/`
carries a pinned upstream revision for each plus a patch, and `setup.sh` clones
and patches them into `.external/`. See
[third_party/README.md](third_party/README.md) for what the patches change and
how to regenerate them after editing.

## Prerequisites

- **OMNeT++ 6.2.0** via `opp_env` — not vendored, and every command below
  assumes you are inside its shell:
  ```bash
  opp_env shell -w <workspace> omnetpp-6.2.0 --no-build --no-cleanup
  ```
- **SUMO** on `PATH`.
- **Bazel** (for the ResilientDB bridge) and **OpenSSL** headers.

## Quick start

```bash
./setup.sh
```

That clones and patches both upstreams, builds the bridge, Veins, `libv2vbft`
and the simulation binary, and generates the replica key directories. Cold, it
takes about ten minutes — nearly all of it Bazel compiling ResilientDB's
dependencies. It is idempotent, so re-running skips whatever is already done.

The four build stages, if you want to drive them individually:

| Stage | Command | Produces |
|---|---|---|
| 1. Bridge | `tools/makeres.sh` | `libresdb_omnet_bridge.so` |
| 2. Veins | `cd .external/veins && ./configure && make -j` | `libveins.so` |
| 3. Protocol | `cd src && make -j` | `libv2vbft.so` |
| 4. Simulation | `cd scenarios/fourway && make -j` | `fourway` |

The binaries must be built locally — a committed one carries an rpath to
whichever machine built it.

## Run

```bash
tools/run-resdb-simulation.sh -u Cmdenv -c FourVehiclesResDB   # headless
tools/run-resdb-simulation.sh -u Qtenv  -c FourVehiclesResDB   # GUI
```

Configs live in `scenarios/fourway/omnetpp.ini`. Useful flags:

| Flag | Effect |
|---|---|
| `--byzleader <id> --leader-byz-type <n>` | make a replica Byzantine; `<n>` is a `ByzantineType` from `src/v2vbft/protocol/Primitives.h` |
| `--no-firewall` | disable the f+1 pre-verification checks |
| `--randomize <N> <F>` | generate a random scenario |
| `--rollback-late-emergency` | late-ambulance rollback scenario |
| `--tolerated-f <n>` | override the tolerated fault count |

Both value-taking flags need their value; passing `--byzleader` bare silently
consumes the next argument.

Full log goes to `/tmp/resdb-simulation.log`, or `$LOG_FILE` if set.

**Replica key directories must match the replica count** — the bridge derives N
from `server.config`, and a directory declaring the wrong N runs with no
consensus while reporting nothing obviously wrong. `setup.sh` generates all
eight (`resdb_crypto` = 4 replicas, `_8`, `_12`, `_16`, `_20`, `_24`, `_rb18` =
18, `_rb_units` = 22). To make one by hand:
`tools/gen_crypto_dir.sh <N> scenarios/fourway/<dir>`.

## Figures

Every figure comes off one pipeline: discover → parse → metrics → figure.

```bash
python3 -m plotter list
python3 -m plotter build ab1_rsu
python3 -m plotter build-all          # -> figures/
```

Log parsing lives in exactly one place, `plotter/io/logparse.py`. If the C++ log
format changes, that is the only file to update. Adding a figure means one
module under `plotter/figures/` and one line in `_registry.py`.

Results come from `benchmarks/ablations/run_ablations.sh`; both results and
figures are gitignored.

## Refactoring safely

Consensus timings are **not** reproducible: the bridge runs PBFT on worker
threads polled on a fixed tick, so the same binary can report a commit at 32.6s
on one run and 33.1s on the next. Comparing timings produces constant false
alarms.

`tests/golden/` therefore compares *structure* — did every replica commit,
schedule size, quorum, stop-sign fallbacks, whether rollback fired — over
several repetitions, because commit success is genuinely probabilistic near the
quorum edge.

```bash
python3 tests/golden/run_golden.py capture     # on known-good code
# ... refactor ...
python3 tests/golden/run_golden.py check       # non-zero exit if anything moved
python3 tests/golden/run_golden.py capture --from-logs   # rebuild without re-running
```

The 18-vehicle rollback cells take ~7 minutes per run, so a full matrix is
roughly 55 minutes. Never run a simulation concurrently with the harness unless
`LOG_FILE` is set per run.

## Troubleshooting

**`OMNETPP_ROOT is not set`** — you are outside the `opp_env` shell.

**`libveins.so` / `libv2vbft.so`: cannot open shared object file** — rebuild
stages 2–4 locally.

**Exit 139 / 133 on 18-vehicle runs** — a known TraCI crash when a node queries
a departed vehicle after sim overrun. It fires *after* the commit, so metrics
are unaffected; the golden harness judges the invariants, not the exit code.

**Bazel fails on Boost `int_float_mixture_enum`, or `<cstdint> file not
found`** — your Clang is newer than the pinned Boost expects. `tools/makeres.sh`
already passes the necessary suppressions and `-xc++`; use it rather than
calling `bazel build` directly.

**Patch fails to apply during `setup.sh`** — the pinned upstream revision moved
or your `.external/` is dirty. Delete the offending tree under `.external/` and
re-run.

