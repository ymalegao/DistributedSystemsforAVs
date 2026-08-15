# Archived documents

Historical notes kept for provenance. **They describe earlier states of the
project and are not maintained** — where one contradicts the code, the code
wins. Do not follow build or run instructions from anything in here; see the
top-level `README.md`.

| Document | What it was |
|---|---|
| `PROJECT_HANDOFF.md`, `JAVA_SIDE_HANDOFF.md`, `HANDOFF_rollback_congestion.md` | session handoff notes |
| `LC_INVESTIGATION.md`, `LC_PROTOCOL_RESEARCH_SUMMARY.md` | leader-change investigation and its literature survey |
| `report.md` | a long-form results write-up |
| `effective_timer_audit_16veh.md` | timer audit for the 16-vehicle scenario |
| `newprotocol_plan.txt` | an early protocol plan, since superseded |
| `partner_found_bug.md`, `partner_questions_raft_timing.md`, `partner.ini` | notes and config from the partner comparison |
| `project_migration_resdb.md` | the BFT-SMaRt → ResilientDB migration record |
| `JavaToC++/` | design notes from the Java → C++ port |
| `AGENTS.md`, `CODEX.md`, `CONTEXT.md` | agent/tooling instructions for a setup no longer in use |
| `bft_v2v_multiconfig.png`, `Figure_1.png` | figures from earlier runs |

Known-stale specifics worth flagging:

- `AGENTS.md` describes a dual-graph MCP tooling setup that has been removed.
- Several documents reference `experiment_orchestrator.py` and
  `fourway/analyze_log.py`, both deleted; figures are now produced by
  `plotter/`.
- `JavaToC++/5stepplan.md` describes `messages/intersection.proto`. Both copies
  of that file were deleted — they were never compiled, and the real wire format
  is the C structs in `integration/omnet/resdb_omnet_bridge.h`.
