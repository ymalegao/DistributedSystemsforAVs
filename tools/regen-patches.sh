#!/usr/bin/env bash
# Rewrite third_party/*.patch from the current state of .external/.
#
# Run this after editing anything inside .external/veins or
# .external/resilientdb, then commit the regenerated patch.
#
#   tools/regen-patches.sh              both trees
#   tools/regen-patches.sh veins        just one
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXTERNAL="${REPO_ROOT}/.external"
THIRD_PARTY="${REPO_ROOT}/third_party"
LOCKFILE="${THIRD_PARTY}/versions.lock"

lock_field() { awk -v n="$1" -v c="$2" '$1==n {print $c}' "$LOCKFILE"; }
die() { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

# Files that are build output or otherwise never part of a patch.
# Bare directory names matter as much as the globs: `diff -rq` reports a
# whole new directory as one "Only in X: name" line, so `out/*` alone never
# matches it and the entry reaches diff as a directory.
is_excluded() {
  case "$1" in
    .git|.git/*|.upstream-base)                       return 0 ;;
    out|out/*|bazel-*|__pycache__|*/__pycache__|*/__pycache__/*) return 0 ;;
    *_m.cc|*_m.h|*.so|*.o|*.d)                        return 0 ;;
    src/Makefile)                                     return 0 ;;  # ./configure generates it
    bin/veins_run|bin/veins_inet_run)                 return 0 ;;  # ./configure generates these too
    ecosystem|ecosystem/*)                            return 0 ;;  # extras the bridge never builds
    integration|integration/*)                        return 0 ;;  # this repo's bridge/, symlinked in
    *) return 1 ;;
  esac
}

regen() {
  local name="$1"
  local dest="${EXTERNAL}/${name}" patch="${THIRD_PARTY}/$(lock_field "$name" 5)"
  [[ -d "$dest" ]] || die "$dest missing — run ./setup.sh first"

  local base; base="$(lock_field "$name" 4)"
  local scratch; scratch="$(mktemp -d)"
  trap 'rm -rf "$scratch"' RETURN

  echo "==> $name: re-fetching pristine $base"
  git -C "$scratch" init --quiet
  git -C "$scratch" remote add origin "$(lock_field "$name" 2)"
  if [[ "$(lock_field "$name" 3)" == "tag" ]]; then
    git -C "$scratch" fetch --quiet --depth 1 origin "refs/tags/${base}:refs/tags/${base}"
  else
    git -C "$scratch" fetch --quiet --depth 1 origin "$base"
  fi
  git -C "$scratch" checkout --quiet FETCH_HEAD

  echo "==> $name: diffing"
  : > "$patch"
  local count=0
  while IFS= read -r rel; do
    is_excluded "$rel" && continue
    # A patch describes files. Directories reach us from "Only in X: <dir>"
    # lines and would make diff error out on a directory operand.
    [[ -d "$dest/$rel" ]] && continue
    [[ -f "$dest/$rel" ]] || continue
    local a="$scratch/$rel"
    [[ -f "$a" ]] || a=/dev/null
    diff -u --label "a/$rel" --label "b/$rel" "$a" "$dest/$rel" >> "$patch" || true
    count=$((count + 1))
  done < <(
    diff -rq "$scratch" "$dest" -x .git 2>/dev/null \
      | sed -E "s|^Files $scratch/(.*) and .* differ$|\1|; s|^Only in $dest/?(.*): (.*)$|\1/\2|; t; d" \
      | sed 's|^/||' \
      | sort
  )
  echo "    $patch: $count files, $(grep -cE '^[+-][^+-]' "$patch" || true) changed lines"
}

# Must build an array: "${@:-veins resilientdb}" expands the default to a single
# word, so the loop would run once with name="veins resilientdb".
names=("$@")
[[ ${#names[@]} -eq 0 ]] && names=(veins resilientdb)

for name in "${names[@]}"; do
  regen "$name"
done
