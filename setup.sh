#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# One-shot setup for a fresh clone.
#
#   ./setup.sh              clone + patch vendors, build everything, make keys
#   ./setup.sh --verify     check the patches still reproduce .external/ exactly
#   ./setup.sh --no-build   materialise .external/ and keys only, skip compiling
#
# Must run inside the OMNeT++ environment, e.g.
#   opp_env shell -w <workspace> omnetpp-6.2.0 --no-build --no-cleanup
#
# Everything is idempotent: re-running skips work that is already done.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTERNAL="${REPO_ROOT}/.external"
THIRD_PARTY="${REPO_ROOT}/third_party"
LOCKFILE="${THIRD_PARTY}/versions.lock"

DO_BUILD=1
DO_VERIFY=0
for arg in "$@"; do
  case "$arg" in
    --no-build) DO_BUILD=0 ;;
    --verify)   DO_VERIFY=1; DO_BUILD=0 ;;
    -h|--help)  sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mwarning: %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

# Read one field out of versions.lock. Fields: url kind ref patch
lock_field() {
  local name="$1" col="$2"
  awk -v n="$name" -v c="$col" '$1==n {print $c}' "$LOCKFILE"
}

# ── 1. Vendored trees ────────────────────────────────────────────────────────
# A shallow fetch of one revision: we only ever need that exact tree, and the
# full ResilientDB history is several hundred MB.
fetch_upstream() {
  local name="$1" dest="$2"
  local url kind ref
  url="$(lock_field "$name" 2)"; kind="$(lock_field "$name" 3)"; ref="$(lock_field "$name" 4)"
  [[ -n "$url" && -n "$ref" ]] || die "no entry for '$name' in $LOCKFILE"

  if [[ -d "$dest/.git" ]]; then
    echo "  $name: already present, leaving alone"
    return 0
  fi
  say "Cloning $name at $ref"
  mkdir -p "$dest"
  git -C "$dest" init --quiet
  git -C "$dest" remote add origin "$url" 2>/dev/null || true
  if [[ "$kind" == "tag" ]]; then
    git -C "$dest" fetch --quiet --depth 1 origin "refs/tags/${ref}:refs/tags/${ref}"
  else
    git -C "$dest" fetch --quiet --depth 1 origin "$ref"
  fi
  git -C "$dest" checkout --quiet FETCH_HEAD

  say "Applying third_party/$(lock_field "$name" 5)"
  git -C "$dest" apply --verbose "${THIRD_PARTY}/$(lock_field "$name" 5)" 2>&1 | sed 's/^/  /'
  # Record the pristine base so regen-patches.sh and --verify can diff against it.
  git -C "$dest" rev-parse HEAD > "$dest/.upstream-base"
}

mkdir -p "$EXTERNAL"
fetch_upstream veins       "${EXTERNAL}/veins"
fetch_upstream resilientdb "${EXTERNAL}/resilientdb"

# The bridge is source in THIS repo. Bazel reaches it as //integration/omnet
# through this symlink, so editing bridge/*.cc rebuilds the real target.
say "Linking bridge/ into the ResilientDB workspace"
mkdir -p "${EXTERNAL}/resilientdb/integration"
ln -sfn "${REPO_ROOT}/bridge" "${EXTERNAL}/resilientdb/integration/omnet"
echo "  .external/resilientdb/integration/omnet -> bridge/"

# ── --verify: do the patches still describe .external/ exactly? ──────────────
if [[ "$DO_VERIFY" -eq 1 ]]; then
  say "Verifying patches still describe .external/ exactly"
  rc=0
  for name in veins resilientdb; do
    dest="${EXTERNAL}/${name}"
    patch="${THIRD_PARTY}/$(lock_field "$name" 5)"
    [[ -d "$dest/.git" ]] || { warn "$name not materialised; run ./setup.sh first"; rc=1; continue; }

    # Two independent checks, because each catches a different kind of drift.
    #
    # 1. The patch must reverse-apply cleanly. That proves every hunk in the
    #    patch is present in the tree exactly as recorded.
    if ! git -C "$dest" apply --reverse --check "$patch" 2>/dev/null; then
      echo "  $name: FAIL — the tree no longer matches $(basename "$patch")"
      echo "    someone edited a patched file; run tools/regen-patches.sh"
      rc=1
      continue
    fi

    # 2. The set of files git reports as changed must equal the set of files the
    #    patch touches. Check 1 alone would pass happily if a NEW file were
    #    edited that the patch knows nothing about.
    changed="$(git -C "$dest" status --porcelain --untracked-files=no \
                 | awk '{print $NF}' | grep -v '^\.upstream-base$' | sort)"
    in_patch="$(grep '^--- a/' "$patch" | sed 's|^--- a/||' | sort -u)"
    extra="$(comm -23 <(echo "$changed") <(echo "$in_patch"))"
    if [[ -n "$extra" ]]; then
      echo "  $name: FAIL — edits to files the patch does not cover:"
      echo "$extra" | sed 's/^/      /'
      echo "    run tools/regen-patches.sh to fold them in"
      rc=1
    else
      echo "  $name: OK — $(echo "$in_patch" | wc -l) patched files, no drift"
    fi
  done
  exit "$rc"
fi

# ── 2. Build ─────────────────────────────────────────────────────────────────
if [[ "$DO_BUILD" -eq 1 ]]; then
  [[ -n "${OMNETPP_ROOT:-}" ]] || die "OMNETPP_ROOT is not set — run inside the opp_env shell (see README)."

  say "Building the ResilientDB bridge (Bazel, ~8 min cold)"
  "${REPO_ROOT}/tools/makeres.sh"

  say "Building Veins"
  ( cd "${EXTERNAL}/veins" && [[ -f src/Makefile ]] || ./configure )
  ( cd "${EXTERNAL}/veins" && make -j"$(nproc)" MODE=release )

  say "Building libv2vbft (the protocol)"
  ( cd "${REPO_ROOT}/src" && make -j"$(nproc)" MODE=release )
fi

# ── 3. Replica keys ──────────────────────────────────────────────────────────
# Each scenario reads its replica count from server.config, so the N here is
# load-bearing: a directory that declares the wrong N runs with no consensus
# and reports nothing obviously wrong. Names must match resdbCryptoDir in
# scenarios/fourway/omnetpp.ini.
say "Generating replica key directories"
SCENARIO="${REPO_ROOT}/scenarios/fourway"
declare -A CRYPTO_DIRS=(
  [resdb_crypto]=4          [resdb_crypto_8]=8
  [resdb_crypto_12]=12      [resdb_crypto_16]=16
  [resdb_crypto_20]=20      [resdb_crypto_24]=24
  [resdb_crypto_rb18]=18    [resdb_crypto_rb_units]=22
)
KEYGEN="${EXTERNAL}/resilientdb/bazel-bin/tools/key_generator_tools"
if [[ ! -x "$KEYGEN" ]]; then
  warn "Bazel key tools not built; skipping key generation."
  warn "Run ./setup.sh without --no-build, then: tools/gen_crypto_dir.sh <N> <dir>"
else
  for dir in "${!CRYPTO_DIRS[@]}"; do
    n="${CRYPTO_DIRS[$dir]}"
    have=$(grep -c 'replica_info' "${SCENARIO}/${dir}/server.config" 2>/dev/null || echo 0)
    if [[ "$have" -eq "$n" ]]; then
      echo "  $dir: already declares $n replicas"
    else
      echo "  $dir: generating $n replicas"
      "${REPO_ROOT}/tools/gen_crypto_dir.sh" "$n" "${SCENARIO}/${dir}" | sed 's/^/    /'
    fi
  done
fi

# ── 4. Simulation binary ─────────────────────────────────────────────────────
if [[ "$DO_BUILD" -eq 1 ]]; then
  say "Building the simulation binary"
  ( cd "$SCENARIO" && make -j"$(nproc)" MODE=release )
fi

say "Done"
cat <<'MSG'
Run a simulation with:

  tools/run-resdb-simulation.sh -u Cmdenv -c FourVehiclesResDB   # headless
  tools/run-resdb-simulation.sh -u Qtenv  -c FourVehiclesResDB   # GUI

Configs live in scenarios/fourway/omnetpp.ini.
MSG
