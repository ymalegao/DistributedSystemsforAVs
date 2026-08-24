#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for name in veins resilientdb; do
  tree="${ROOT}/.external/${name}"; [[ -d "${tree}/.git" ]] || { echo "missing ${tree}" >&2; exit 1; }
  base="$(cat "${tree}/.upstream-base" 2>/dev/null || true)"; [[ -n "$base" ]] || { echo "${name}: missing .upstream-base" >&2; exit 1; }
  git -C "$tree" diff --binary "$base" -- . > "${ROOT}/third_party/${name}.patch"
  echo "wrote third_party/${name}.patch"
done
