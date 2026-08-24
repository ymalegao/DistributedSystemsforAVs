#!/usr/bin/env bash
# Wipe the ResDB Bazel output root (clean --expunge).
#
# Usage:
#   ./scripts/clearcache.sh
#   clearcache                 # if aliased via ~/.bash_profile
#
# Override location with BAZEL_OUTPUT_ROOT (must match makeres).
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RESDB_ROOT="${RESDB_ROOT:-${REPO_ROOT}/incubator-resilientdb}"
BAZEL_OUTPUT_ROOT="${BAZEL_OUTPUT_ROOT:-${HOME}/.cache/bazel-resdb}"

cd "${RESDB_ROOT}"
echo "Expunging Bazel cache at output_user_root=${BAZEL_OUTPUT_ROOT}"
bazel --output_user_root="${BAZEL_OUTPUT_ROOT}" clean --expunge
echo "Done. Next makeres will be a full cold build."
