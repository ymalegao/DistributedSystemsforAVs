#!/usr/bin/env bash
PORT=$(cat "/Users/yashmalegaonkar/Documents/v2v/.dual-graph/mcp_port" 2>/dev/null || echo 8080)
OUT=$(curl -sf --max-time 2 "http://127.0.0.1:$PORT/prime" 2>/dev/null || true)
if [[ -n "$OUT" ]]; then
  echo "$OUT"
fi
# Inject CONTEXT.md if it exists (session carry-over, ~200 tokens)
if [[ -f "/Users/yashmalegaonkar/Documents/v2v/CONTEXT.md" ]]; then
  echo ""
  echo "=== CONTEXT.md ==="
  cat "/Users/yashmalegaonkar/Documents/v2v/CONTEXT.md"
  echo "=== end CONTEXT.md ==="
fi
# Inject context store entries (decisions, tasks, next steps) — max 15 lines, 7-day window
STORE="/Users/yashmalegaonkar/Documents/v2v/.dual-graph/context-store.json"
if [[ -f "$STORE" ]] && command -v jq &>/dev/null; then
  CUTOFF=$(date -v-7d +%Y-%m-%d 2>/dev/null || date -d '7 days ago' +%Y-%m-%d 2>/dev/null || echo "2000-01-01")
  ENTRIES=$(jq -r --arg cutoff "$CUTOFF"     '[.[] | select(.date >= $cutoff)] | .[:15] | .[] | "[" + .type + "] " + .content'     "$STORE" 2>/dev/null)
  if [[ -n "$ENTRIES" ]]; then
    echo ""
    echo "=== Stored Context ==="
    echo "$ENTRIES"
    echo "=== end Stored Context ==="
  fi
fi
# Never fail hooks due to stderr/exit behavior.
exit 0
