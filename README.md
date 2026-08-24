# DistributedSystemsforAVs / PicaBFT

This repo contains the active OMNeT++/Veins + ResilientDB intersection simulation.
The old Java/JNI/BFT-SMaRt implementation has been removed from the runnable code path.

The current runnable pieces are organized by responsibility:

- `veins-veins-5.3.1`
- `incubator-resilientdb`
- `fourway`
- `src/`, `bridge/`, and `scenarios/fourway/` (the structure-only migration
  is in progress; compatibility paths remain available)
- `tools/experiment_orchestrator.py` (also available as
  `./experiment_orchestrator.py`)
- `experiments/` for experiment-owned manifests and fixtures
- `docs/` for architecture, protocol specification, and command catalogs

OMNeT++ is an external prerequisite and is not vendored in this repo.

## Prerequisites

- OMNeT++ 6.2.0 installed separately, typically at `/home/yash/omnetpp/omnetpp-6.2.0`
- A shell where OMNeT++ is available, or `OMNETPP_ROOT` exported manually
- Bazel/toolchain support for building `incubator-resilientdb`

Optional environment variables:

- `OMNETPP_ROOT`
- `VEINS_ROOT`
- `FOURWAY_ROOT`
- `RESDB_ROOT`
- `DISTRIBUTED_AVS_ROOT`

## Environment

From the OMNeT++ installation:

```bash
cd /home/yash/omnetpp/omnetpp-6.2.0
source setenv
opp_env shell
source ~/.bashrc
```

Then return to the repo:

```bash
cd /home/yash/DistributedSystemsforAVs
```

## Build

Build the ResDB OMNeT bridge and helper tools:

```bash
makeres
```

Build Veins:

```bash
makeveins
```

Equivalent direct commands are:

```bash
./scripts/makeres.sh
( cd veins-veins-5.3.1 && make )
```

## Run

Use the ResDB simulation runner directly:

```bash
fourway/run-resdb-simulation.sh -u Cmdenv -c FourVehiclesResDB
```

GUI mode:

```bash
fourway/run-resdb-simulation.sh -u Qtenv -c FourVehiclesResDB
```

Common configs include:

- `FourVehiclesResDB`
- `EightVehiclesResDB`
- `TwelveVehiclesResDB`
- `SixteenVehiclesResDB`
- `EighteenVehiclesResDB`
- `TwentyVehiclesResDB`

The compatibility wrappers `fourway/run.sh`, `fourway/run_cmdenv.sh`, and
`fourway/run_correct.sh` now delegate to `fourway/run-resdb-simulation.sh`.

For batch experiments, use the orchestrator:

```bash
python tools/experiment_orchestrator.py
```

Pass the same arguments you normally use for your experiment suite. The root
`experiment_orchestrator.py` wrapper is retained so older command logs remain
reproducible.

## Repository layout

`src/` and `bridge/` are the intended homes for protocol and ResilientDB
integration code. `scenarios/fourway/` owns SUMO/OMNeT inputs. Each directory
under `experiments/` owns one experiment's parameters, fixtures, validation,
and analysis; generated `results/` and `figures/` are ignored. Shared parsing
and plotting code belongs in `plotter/`, while build and launch helpers belong
in `tools/`. The vendored dependency trees remain locally available during
this staged migration and will be materialized under `.external/` by
`setup.sh` in the dependency-extraction checkpoint.

## Notes

- All active consensus traffic uses the C++ ResDB bridge and Veins 802.11p radio path.
- Historical Java migration notes remain in archived documentation, but they are not part of the build.
- Route and result files with `bft_` in their names are retained as experiment artifacts or SUMO scenario names.

## Troubleshooting

If the launcher cannot find OMNeT++:

- source the OMNeT shell with `source "$OMNETPP_ROOT/setenv"`
- or export `OMNETPP_ROOT` before running

If `makeres` / Bazel fails on Boost.MPL `int_float_mixture_enum` /
`-Wenum-constexpr-conversion`, your Clang is newer than the pinned Boost expects;
`incubator-resilientdb/.bazelrc` already passes the required suppression for C++ builds.

If plain `bazel build` from `opp_env` fails on macOS with `'limits' file not found`
or similar while compiling protobuf, use `scripts/makeres.sh`; it clears SDK/include
environment variables before invoking Bazel.
