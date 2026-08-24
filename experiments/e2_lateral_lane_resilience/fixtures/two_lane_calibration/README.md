# Two-lane four-way sensing calibration (Checkpoint 1)

Parallel fixture: `../bft_intersection_2lane.net.xml` (does **not** replace
`bft_intersection.net.xml`). Production configs are **not** retargeted here.

## Lane mapping (locked)

| SUMO index | Role | Movements |
|---|---|---|
| 0 | T (outer / rightmost) | straight + right |
| 1 | L (inner / leftmost) | left only |

Inbound centerline separation **3.2 m** on every approach. Calibration uses
**North inbound `N2C`** only. No U-turns. TLS forever all-red.

Compatible with existing ResDB approach parsing (`laneId[0]` → N/S/E/W) and
`projectPhysicalLaneIndex` (lateral below 1.6 m → index 0).

**Lane-index audit (why 0=T / 1=L is safe while production stays single-lane):**
- Cardinal mode hardcodes `physicalLaneIndex = 0` and never reads SUMO lane index for
  eligibility; approach comes from `laneId[0]` / `intendedLane`.
- ADJACENT_LATERAL already treats index 0 as the origin-side lane (below the 1.6 m
  boundary) and index 1 as the far lane — same as this fixture.
- Because this PR does not retarget production configs, existing single-lane runs are
  unchanged.

## Gate (Python ↔ C++ parity)

```
accept iff |observedLateral_cm − claimedLateral_cm| ≤ k · σ_lat · 100
```

with centimetre quantization via `llround(m*100)`. See `lateral_gate.py` and
`lateral_gate_ref.cc` (standalone mirror of `ResDBArrivalProtocol` ADJACENT_LATERAL).

## Physics pins (run metadata)

- `--time-to-teleport -1`
- `--collision.check-junctions true`
- `--collision.action none`
- Released vehicles in protocol runs: `setSpeedMode(0)` (unsafe schedules must
  be able to collide). Calibration uses stopped vehicles — speed mode irrelevant.

## Run

```bash
export SUMO_HOME=/path/to/sumo
python3 run_calibration.py          # full 10×10k acceptance
python3 run_calibration.py --quick  # 1×1k smoke
python3 -m unittest test_calibration.py -v
```

`adjacent_lane_calibration/` remains the straight-rig regression/example.
