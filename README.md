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

## Getting started

Installing the toolchain, building the four stages, running a simulation, and
checking the result are all in **[INSTALLATION.md](INSTALLATION.md)**.

```bash
tools/check-prereqs.sh    # is this machine ready?
./setup.sh                # clone, patch, build everything, generate keys
```

## Figures

Every figure comes off one pipeline: discover → parse → metrics → figure.

```bash
python3 -m plotter list
python3 -m plotter build ab1_rsu_1lane
python3 -m plotter build-all          # -> figures/
```

The compact paper figure layout and metric definitions are in
[docs/paper-figures.md](docs/paper-figures.md). Ablation 2 is retired from both
the runner and plotter; priority is included in the baseline figures.

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

## Contributions and contact

ymalegao@ucsc.edu · mthiruma@ucsc.edu

