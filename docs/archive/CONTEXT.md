# Session Handoff — 2026-07-05

## Current Task
R0 late-emergency rollback: quorum/cancel path implemented; current blocker is validating the new manager-side late injection and confirming rollback reaches full M/quorum.

## Key Decisions
- Canonical quorum is `q(N,f)=ceil((N+f+1)/2)`; bridge logs static `N=18,f=5,q=12`.
- Rollback forced-M uses anchored `f_anchored=5`; proposals with `|M|<16` correctly log `[ROLLBACK-UNAVAILABLE]`.
- Witness-triggered cancel works; failure is now membership/recallability, not no-echo.
- Work stream 2 is now implemented as a manager-side R0 supervisor: the route file no longer schedules veh16/veh17; TraCIScenarioManager injects veh16 normal + veh17 ambulance near the junction when batch 0 starts.

## Next Steps
- Build/run R0 from an OMNeT shell and inspect `[R0-SUPERVISOR]`, `[CANCEL-WITNESS]`, `[ROLLBACK-QUORUM]`, and `[ORDER]` logs.
- If SUMO rejects insertion at `r0LateSpawnDepartPos=280m`, tune `*.manager.r0LateSpawnDepartPos` or let the retry loop advance.
- Ledger/hard-timeout portion of Work stream 2 is still not implemented.
w


written by human me:
SUMO/OMNeT implementation notes
The endSimulation race — fix it structurally, not by tuning. SUMO terminates when no active vehicles and no pending departures remain; your late arrivals are exactly the thing that dies in that race. Run SUMO with an explicit --end <large> and make OMNeT++'s sim-time-limit (or your orchestrator observing epoch-e+1 completion) the sole termination authority. Kill the .rou.xml depart-time tuning entirely: since your app already receives the order callback, add a small scenario-supervisor module that, on epoch-e commit, schedules a TraCI vehicle.add for the ambulance at commit + Δ. That makes arrival-after-commit true by construction and turns Δ into a clean experimental axis — the R1 sweep falls out for free.

the tuning in the rou is not working well, I think maybe we can spawn the ambulance and other cars in as soon as one batch is up for now. If we spawn them at the back of intersection, timing seems to be weird and unstable. I rather spawn them and the story right now is that they turned from another street or something and just showed up.
