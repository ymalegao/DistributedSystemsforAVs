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

# ResDB shared library for the Veins OMNeT bridge (Bazel). Use inside opp_env / Nix OMNeT shells.
# Unset SDKROOT and include paths so the Nix clang wrapper is not forced to mix the macOS SDK
# sysroot with Nix libc++ (breaks <atomic>, <string>, etc.). zlib host-tool builds also need the
# fdopen workaround on Darwin; incubator-resilientdb/.bazelrc sets this too — repeated here so a
# copied one-liner stays safe.
buildresdbomnet() {
  (
    local resdb_root="${DISTRIBUTED_AVS_ROOT}/incubator-resilientdb"
    cd "$resdb_root" || exit 1
    unset SDKROOT CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH SDK_PATH
    bazel --output_user_root=/tmp/bazel build \
      --spawn_strategy=local \
      --cxxopt=-xc++ \
      --host_cxxopt=-xc++ \
      --cxxopt=-Wno-enum-constexpr-conversion \
      --host_cxxopt=-Wno-enum-constexpr-conversion \
      --copt=-Dfdopen=fdopen \
      --host_copt=-Dfdopen=fdopen \
      //integration/omnet:resdb_omnet_bridge \
      "$@"
  )
}

# Same environment as buildresdbomnet — e.g. `fourway/resdb_crypto/gen_resdb_keys.sh` prerequisites.
buildresdbtools() {
  (
    local resdb_root="${DISTRIBUTED_AVS_ROOT}/incubator-resilientdb"
    cd "$resdb_root" || exit 1
    unset SDKROOT CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH SDK_PATH
    bazel --output_user_root=/tmp/bazel build \
      --spawn_strategy=local \
      --cxxopt=-xc++ \
      --host_cxxopt=-xc++ \
      --cxxopt=-Wno-enum-constexpr-conversion \
      --host_cxxopt=-Wno-enum-constexpr-conversion \
      --copt=-Dfdopen=fdopen \
      --host_copt=-Dfdopen=fdopen \
      //tools:key_generator_tools //tools:certificate_tools \
      "$@"
  )
}
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

If `buildresdbomnet` / Bazel fails on Boost.MPL `int_float_mixture_enum` / `-Wenum-constexpr-conversion`, your Clang is newer than the pinned Boost expects; `incubator-resilientdb/.bazelrc` already passes `-Wno-enum-constexpr-conversion` for C++ builds.

If plain `bazel build` from `opp_env` fails on macOS with `'limits' file not found` (or similar) while compiling protobuf, run `buildresdbtools` or `buildresdbomnet` from the README (they `unset SDKROOT` / include paths before invoking Bazel). `incubator-resilientdb/.bazelrc` adds macOS-only `--spawn_strategy=local` and `-xc++`, but cannot clear `SDKROOT` in the rc file without conflicting with Bazel's Xcode integration.
