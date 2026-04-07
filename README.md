# DistributedSystemsforAVs

This repo contains the active OMNeT++/Veins + JNI + BFT-SMaRt intersection simulation.

The current runnable pieces are:
- `bftsmart`
- `veins-veins-5.3.1`
- `fourway`

OMNeT++ is an external prerequisite and is not vendored in this repo.

## What A Teammate Needs


Prerequisites:
- OMNeT++ 6.2.0 installed separately
- Java 11
- a shell where OMNeT++ is available, or `OMNETPP_ROOT` exported manually

Optional environment variables:
- `OMNETPP_ROOT`
- `JAVA_HOME`
- `BFTSMART_ROOT`
- `VEINS_ROOT`
- `FOURWAY_ROOT`
- `DISTRIBUTED_AVS_ROOT`

Only `OMNETPP_ROOT` is truly external. The other repo paths default automatically to this checkout.

## First-Time Setup

```bash
git clone <repo-url> DistributedSystemsforAVs
cd DistributedSystemsforAVs
```

Source the OMNeT environment, or export `OMNETPP_ROOT` yourself:

```bash
source "$OMNETPP_ROOT/setenv"
```

If OMNeT++ is already sourced, the launcher will infer `OMNETPP_ROOT` automatically.

## Build

Build Java first:

```bash
cd /path/to/DistributedSystemsforAVs/bftsmart/library
./gradlew installDist
```

Then build Veins:

```bash
cd /path/to/DistributedSystemsforAVs/veins-veins-5.3.1
make
```

Then build `fourway` if needed:

```bash
cd /path/to/DistributedSystemsforAVs/fourway
make
```

## Run

From the repo:

```bash
cd /path/to/DistributedSystemsforAVs/bftsmart
./run-omnet-simulation.sh -u Cmdenv -c TwelveVehiclesAmbulanceBFT
```

GUI mode:

```bash
./run-omnet-simulation.sh -c TwelveVehiclesAmbulanceBFT
```

Notes:
- the script defaults the simulation directory to `fourway`
- the script auto-discovers repo-local `bftsmart`, `veins-veins-5.3.1`, and `fourway`
- the script dynamically discovers Nix runtime library directories from the local OMNeT install when needed

## Optional Shell Helpers

If you want the same convenience helpers used locally, add something like this to your shell after OMNeT is set up:

```bash
export DISTRIBUTED_AVS_ROOT="${DISTRIBUTED_AVS_ROOT:-$HOME/DistributedSystemsforAVs}"

makeveins() { ( cd "$DISTRIBUTED_AVS_ROOT/veins-veins-5.3.1" && make "$@"; ); }
makebft()   { ( cd "$DISTRIBUTED_AVS_ROOT/bftsmart/library" && ./gradlew installDist "$@"; ); }
runomnet()  { ( cd "$DISTRIBUTED_AVS_ROOT/bftsmart" && ./run-omnet-simulation.sh "$@"; ); }
runomnetnogui() { ( cd "$DISTRIBUTED_AVS_ROOT/bftsmart" && ./run-omnet-simulation.sh -u Cmdenv "$@"; ); }
```

## Troubleshooting

If the embedded JVM fails during startup:
- run `./gradlew installDist` in `bftsmart/library`
- make sure `JAVA_HOME` points to a Java 11 install if your default JDK is different

If the launcher says it cannot find OMNeT++:
- source your OMNeT shell with `source "$OMNETPP_ROOT/setenv"`
- or export `OMNETPP_ROOT` before running

If your OMNeT install was built under Nix:
- no extra manual path edits should be needed
- `run-omnet-simulation.sh` derives the loader paths from your local `liboppsim` dependencies
