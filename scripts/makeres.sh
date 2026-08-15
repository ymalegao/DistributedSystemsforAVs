#!/usr/bin/env bash
# Rebuild libresdb_omnet_bridge for OMNeT++ / Veins (must use Nix libc++ from opp_env).
#
# Usage (inside opp_env shell or after: source "$OMNETPP_ROOT/setenv"):
#   ./scripts/makeres.sh
#   ./scripts/makeres.sh --force    # delete stale dylib and rebuild
#
# Cache lives under ~/.cache/bazel-resdb (not /tmp) so reboots keep the
# action cache. Wipe with: ./scripts/clearcache.sh  (or: clearcache)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RESDB_ROOT="${RESDB_ROOT:-${REPO_ROOT}/incubator-resilientdb}"
BAZEL_OUTPUT_ROOT="${BAZEL_OUTPUT_ROOT:-${HOME}/.cache/bazel-resdb}"
BRIDGE="${RESDB_ROOT}/bazel-bin/integration/omnet/libresdb_omnet_bridge.dylib"
FOURWAY="${REPO_ROOT}/fourway/out/clang-release/fourway"
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
bazel --output_user_root="${BAZEL_OUTPUT_ROOT}" build \
  --spawn_strategy=local \
  --cxxopt=-xc++ \
  --host_cxxopt=-xc++ \
  --cxxopt=-Wno-enum-constexpr-conversion \
  --host_cxxopt=-Wno-enum-constexpr-conversion \
  --copt=-Dfdopen=fdopen \
  --host_copt=-Dfdopen=fdopen \
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
