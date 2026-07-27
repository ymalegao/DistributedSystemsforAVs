#!/usr/bin/env bash
# gen_crypto_dir.sh — generate a resdb crypto dir (keys + certs + server.config)
# for N replicas. Corrected paths for this machine.
#   ./gen_crypto_dir.sh <N> <output_dir>
set -euo pipefail
N="${1:?need N}"
OUT="${2:?need output dir}"
FOURWAY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESDB_ROOT="$(cd "$FOURWAY/.." && pwd)/incubator-resilientdb"
KEYGEN="${RESDB_ROOT}/bazel-bin/tools/key_generator_tools"
CERTGEN="${RESDB_ROOT}/bazel-bin/tools/certificate_tools"
BASE_PORT=18880

for bin in "$KEYGEN" "$CERTGEN"; do
  [[ -x "$bin" ]] || { echo "ERROR: $bin missing"; exit 1; }
done

mkdir -p "$OUT/logs"
cd "$OUT"
OUT_ABS="$(pwd)"

[[ -f admin.key.pri ]] || "$KEYGEN" "${OUT_ABS}/admin"

for ((i = 1; i <= N; i++)); do
  port=$(( BASE_PORT + i ))
  [[ -f "node${i}.key.pri" ]] || "$KEYGEN" "${OUT_ABS}/node${i}"
  "$CERTGEN" "${OUT_ABS}" "${OUT_ABS}/admin.key.pri" "${OUT_ABS}/admin.key.pub" \
    "${OUT_ABS}/node${i}.key.pub" "${i}" "127.0.0.1" "${port}" "replica"
done

{
  echo "{"
  echo "  region : {"
  for ((i = 1; i <= N; i++)); do
    port=$(( BASE_PORT + i ))
    echo "    replica_info : {"
    echo "      id:${i},"
    echo "      ip:\"127.0.0.1\","
    echo "      port: ${port},"
    echo "    },"
  done
  echo "    region_id: 1,"
  echo "  },"
  echo "  self_region_id:1,"
  echo "}"
} > server.config

echo "GEN_DONE N=${N} dir=${OUT_ABS} replica_info=$(grep -c replica_info server.config) keys=$(ls node*.key.pri | wc -l)"
