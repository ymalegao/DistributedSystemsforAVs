#!/usr/bin/env bash
# gen_resdb_keys.sh — generate per-replica ED25519 keys, certificates, and
# a server.config for N replicas using the ResDB Bazel toolchain.
#
# Usage:  ./gen_resdb_keys.sh [N]   (default N=4)
# Output: files written to the directory containing this script.
#
# Prerequisites:
#   cd /home/yash/DistributedSystemsforAVs/incubator-resilientdb
#   bazel build //tools:key_generator_tools //tools:certificate_tools
#
# After running this script, set in omnetpp.ini:
#   *.node[*].appl.resdbCryptoDir = "resdb_crypto"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESDB_ROOT="/home/yash/DistributedSystemsforAVs/incubator-resilientdb"
KEYGEN="${RESDB_ROOT}/bazel-bin/tools/key_generator_tools"
CERTGEN="${RESDB_ROOT}/bazel-bin/tools/certificate_tools"
N=${1:-4}
BASE_PORT=18880

echo "=== ResDB key generation: ${N} replicas, base port ${BASE_PORT} ==="

# ── 1. Check tools ─────────────────────────────────────────────────────────────
for bin in "$KEYGEN" "$CERTGEN"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: $bin not found."
        echo "Build it with:"
        echo "  cd $RESDB_ROOT && bazel build //tools:key_generator_tools //tools:certificate_tools"
        exit 1
    fi
done

cd "$SCRIPT_DIR"
mkdir -p logs

# ── 2. Admin keypair (signs every node certificate) ───────────────────────────
if [[ ! -f admin.key.pri ]]; then
    echo "Generating admin keypair..."
    "$KEYGEN" "${SCRIPT_DIR}/admin"
fi

# ── 3. Per-node keypairs + certificates ───────────────────────────────────────
for ((i = 1; i <= N; i++)); do
    port=$(( BASE_PORT + i ))
    echo "  Node $i  127.0.0.1:${port}"

    # Key pair (writes node${i}.key.pri and node${i}.key.pub)
    if [[ ! -f "node${i}.key.pri" ]]; then
        "$KEYGEN" "${SCRIPT_DIR}/node${i}"
    fi

    # Certificate (writes cert_${i}.cert)
    "$CERTGEN" \
        "${SCRIPT_DIR}" \
        "${SCRIPT_DIR}/admin.key.pri" \
        "${SCRIPT_DIR}/admin.key.pub" \
        "${SCRIPT_DIR}/node${i}.key.pub" \
        "${i}" "127.0.0.1" "${port}" "replica"
done

# ── 4. server.config ──────────────────────────────────────────────────────────
cat > server.config <<EOF
{
  region : {
EOF

for ((i = 1; i <= N; i++)); do
    port=$(( BASE_PORT + i ))
    cat >> server.config <<EOF
    replica_info : {
      id:${i},
      ip:"127.0.0.1",
      port: ${port},
    },
EOF
done

cat >> server.config <<EOF
    region_id: 1,
  },
  self_region_id:1,
}
EOF

echo ""
echo "=== Done ==="
echo "Files written to: $SCRIPT_DIR"
echo ""
echo "Replica → NED replicaId mapping (0-based):"
for ((i = 1; i <= N; i++)); do
    echo "  node[$((i-1))].appl.replicaId = $((i-1))  →  node${i}.key.pri / cert_${i}.cert"
done
echo ""
echo "In omnetpp.ini [Config BFTOverV2VWithResilientDB] add:"
echo '  *.node[*].appl.resdbCryptoDir = "resdb_crypto"'
