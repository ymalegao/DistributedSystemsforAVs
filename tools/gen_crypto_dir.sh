#!/usr/bin/env bash
# Generate a ResilientDB replica key/certificate directory.
set -euo pipefail
N="${1:?need replica count}"
OUT="${2:?need output directory}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESDB_ROOT="${RESDB_ROOT:-${ROOT}/.external/resilientdb}"
KEYGEN="${RESDB_ROOT}/bazel-bin/tools/key_generator_tools"
CERTGEN="${RESDB_ROOT}/bazel-bin/tools/certificate_tools"
BASE_PORT=18880
[[ "$N" =~ ^[0-9]+$ && "$N" -gt 0 ]] || { echo "invalid replica count" >&2; exit 2; }
for bin in "$KEYGEN" "$CERTGEN"; do [[ -x "$bin" ]] || { echo "missing $bin (build ResilientDB first)" >&2; exit 1; }; done
mkdir -p "$OUT/logs"
OUT="$(cd "$OUT" && pwd)"
[[ -f "$OUT/admin.key.pri" ]] || "$KEYGEN" "$OUT/admin"
for ((i=1; i<=N; i++)); do
  port=$((BASE_PORT+i)); [[ -f "$OUT/node${i}.key.pri" ]] || "$KEYGEN" "$OUT/node${i}"
  "$CERTGEN" "$OUT" "$OUT/admin.key.pri" "$OUT/admin.key.pub" "$OUT/node${i}.key.pub" "$i" 127.0.0.1 "$port" replica
done
{
  echo '{'; echo '  region : {'
  for ((i=1; i<=N; i++)); do echo "    replica_info : { id:${i}, ip:\"127.0.0.1\", port: $((BASE_PORT+i)), },"; done
  echo '    region_id: 1,'; echo '  },'; echo '  self_region_id:1,'; echo '}'
} > "$OUT/server.config"
echo "GEN_DONE N=${N} dir=${OUT} replica_info=$(grep -c replica_info "$OUT/server.config")"
