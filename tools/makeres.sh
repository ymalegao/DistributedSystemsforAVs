#!/usr/bin/env bash
# Rebuild libresdb_omnet_bridge for OMNeT++ / Veins (must use Nix libc++ from opp_env).
#
# Usage (inside opp_env shell or after: source "$OMNETPP_ROOT/setenv"):
#   tools/makeres.sh
#   tools/makeres.sh --force    # delete stale library and rebuild
#
# Cache root defaults to /tmp/bazel, which is the only value this build is
# known to work with. ~/.cache/bazel in particular is polluted on this machine
# and hits toolchain/header errors. Override with BAZEL_OUTPUT_ROOT if you want
# a cache that survives reboots — a cold build is ~8 minutes.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RESDB_ROOT="${RESDB_ROOT:-${REPO_ROOT}/.external/resilientdb}"
BAZEL_OUTPUT_ROOT="${BAZEL_OUTPUT_ROOT:-/tmp/bazel}"
BRIDGE="${RESDB_ROOT}/bazel-bin/integration/omnet/libresdb_omnet_bridge.dylib"
FOURWAY="${REPO_ROOT}/scenarios/fourway/out/clang-release/fourway"
FORCE=0

for arg in "$@"; do
  case "${arg}" in
    --force|-f) FORCE=1 ;;
    -h|--help)
      sed -n '2,8p' "$0"
      exit 0
      ;;
  esac
done

unset SDKROOT CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH SDK_PATH

if [[ "${FORCE}" -eq 1 && -f "${BRIDGE}" ]]; then
  echo "Removing stale bridge: ${BRIDGE}"
  rm -f "${BRIDGE}"
fi

cd "${RESDB_ROOT}"
echo "Building //integration/omnet:resdb_omnet_bridge (output_user_root=${BAZEL_OUTPUT_ROOT})"
# -xc++ makes the Nix clang find libstdc++ (without it: "<cstdint> file not
# found"); -include cstdint is needed by Abseil; --action_env=PATH keeps the
# opp_env toolchain visible to the spawned actions.
bazel --output_user_root="${BAZEL_OUTPUT_ROOT}" build \
  --spawn_strategy=local \
  --cxxopt=-xc++ \
  --host_cxxopt=-xc++ \
  --cxxopt=-Wno-enum-constexpr-conversion \
  --host_cxxopt=-Wno-enum-constexpr-conversion \
  --copt=-Dfdopen=fdopen \
  --host_copt=-Dfdopen=fdopen \
  --cxxopt=-include --cxxopt=cstdint \
  --host_cxxopt=-include --host_cxxopt=cstdint \
  --action_env=PATH="${PATH}" \
  --host_action_env=PATH="${PATH}" \
  //integration/omnet:resdb_omnet_bridge \
  //tools:key_generator_tools \
  //tools:certificate_tools

if [[ "$(uname -s)" == "Darwin" && -f "${BRIDGE}" && -f "${FOURWAY}" ]]; then
  bridge_cxx="$(otool -L "${BRIDGE}" | awk '/libc\+\+/ {print $1; exit}')"
  host_cxx="$(otool -L "${FOURWAY}" | awk '/libc\+\+/ {print $1; exit}')"
  echo "libc++ bridge: ${bridge_cxx}"
  echo "libc++ fourway: ${host_cxx}"
  if [[ -n "${bridge_cxx}" && -n "${host_cxx}" && "${bridge_cxx}" != "${host_cxx}" ]]; then
    echo "ERROR: libc++ mismatch — rebuild did not pick up Nix toolchain." >&2
    echo "Run inside opp_env (or source setenv), then: $0 --force" >&2
    exit 1
  fi
  echo "OK: bridge matches fourway libc++"
fi
