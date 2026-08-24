#!/usr/bin/env bash
# Materialize pinned dependencies for a clean clone without touching vendored trees.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXTERNAL="${ROOT}/.external"
LOCK="${ROOT}/third_party/versions.lock"
BUILD=1; VERIFY=0
for arg in "$@"; do
  case "$arg" in
    --no-build) BUILD=0;;
    --verify) VERIFY=1; BUILD=0;;
    -h|--help) sed -n '2,6p' "$0"; exit 0;;
    *) echo "unknown option: $arg" >&2; exit 2;;
  esac
done
field() { awk -v n="$1" -v c="$2" '$1 == n {print $c}' "$LOCK"; }
die() { echo "setup: $*" >&2; exit 1; }
materialize() {
  local name="$1" dest="${EXTERNAL}/$1" url kind ref patch
  url="$(field "$name" 2)"; kind="$(field "$name" 3)"; ref="$(field "$name" 4)"; patch="$(field "$name" 5)"
  [[ -f "${ROOT}/third_party/${patch}" ]] || die "missing third_party/${patch}; generate it from the current branch first"
  if [[ ! -d "${dest}/.git" ]]; then
    mkdir -p "$dest"; git -C "$dest" init --quiet; git -C "$dest" remote add origin "$url"
    if [[ "$kind" == tag ]]; then git -C "$dest" fetch --depth 1 origin "refs/tags/${ref}:refs/tags/${ref}"; else git -C "$dest" fetch --depth 1 origin "$ref"; fi
    git -C "$dest" checkout --quiet FETCH_HEAD; git -C "$dest" apply "${ROOT}/third_party/${patch}"
    git -C "$dest" rev-parse HEAD > "${dest}/.upstream-base"
  fi
}
if [[ "$VERIFY" -eq 1 ]]; then
  rc=0
  for name in veins resilientdb; do
    dest="${EXTERNAL}/${name}"; patch="$(field "$name" 5)"
    if [[ -d "${dest}/.git" ]]; then
      if git -C "$dest" apply --reverse --check "${ROOT}/third_party/${patch}"; then
        echo "OK $name (.external)"
      else
        echo "FAIL $name (.external patch drift)" >&2; rc=1
      fi
      continue
    fi

    # During the staged migration the original trees remain the compatibility
    # build inputs. Verify those trees when .external has not been materialized
    # yet; this makes --verify useful offline without claiming a fresh-clone
    # dependency check has passed.
    case "$name" in
      veins) vendor_dir="veins-veins-5.3.1" ;;
      resilientdb) vendor_dir="incubator-resilientdb" ;;
    esac
    if git apply --reverse --check --directory="$vendor_dir" "${ROOT}/third_party/${patch}"; then
      echo "OK $name (vendored compatibility tree; .external not materialized)"
    else
      echo "FAIL $name (no verified .external or compatibility tree)" >&2; rc=1
    fi
  done
  exit "$rc"
fi
mkdir -p "$EXTERNAL"
materialize veins
materialize resilientdb
mkdir -p "${EXTERNAL}/resilientdb/integration"
ln -sfn "${ROOT}/bridge" "${EXTERNAL}/resilientdb/integration/omnet"
if [[ "$BUILD" -eq 1 ]]; then
  "${ROOT}/tools/makeres.sh"
  (cd "${EXTERNAL}/veins" && make -j"${JOBS:-2}" MODE=release)
  (cd "${ROOT}/scenarios/fourway" && make -j"${JOBS:-2}" MODE=release)
fi
echo "setup: dependencies materialized under .external"
