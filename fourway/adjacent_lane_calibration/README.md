# Adjacent-lane continuous perception calibration fixture
#
# Purpose
# -------
# Calibrate the lane-normal lateral gate (q0/q1 and the ROC in δ) on two
# genuinely adjacent parallel lanes separated by 3.2 m. This fixture is NOT a
# cardinal N/E intersection experiment and must not be used as one.
#
# Geometry (canonical metres)
# ---------------------------
#   lane A center: y = 0.0
#   boundary:      y = 1.6
#   lane B center: y = 3.2
#   unit normal n from A toward B: (0, 1)
#
# Target travels on lane A. Claims are
#   u_claim = quantize_1cm(project_normal(p_true) + δ)
# Witness observations are
#   u_obs   = quantize_1cm(project_normal(p_true) + N(0, σ²))
# Acceptance (re-thresholded from the same samples):
#   accept(k) iff |u_obs - u_claim| <= k σ,  k ∈ {2, 2.5, 3}
# Longitudinal motion along the lane is intentionally absent from this residual.
#
# Analytic reference vs implemented gate
# --------------------------------------
# - The one-dimensional Gaussian interval at nominal kσ is the ideal prediction.
# - The published gate ROC uses centimetre-quantized scalar projections (`pooled`).
# - Quantizing both scalar endpoints can shift |u_obs-u_claim| by at most 1 cm.
# - Validation compares quantized empirical rates against the Gaussian band at
#     thresholds kσ ± 1 cm, using Bonferroni-adjusted Wilson intervals over
#     the predeclared 36-cell family (12 δ × 3 k). Ordinary Wilson 95% intervals
#     are retained for plotting only.
#
# Scope boundary
# --------------
# In scope: SUMO two-lane edge, TraCI world XY / lane tracing, geometry, RNG,
# 1 cm quantization, offline k-rethresholding, empirical vs 1D Gaussian analytic,
# and calibration of the scalar rule used by the dedicated ResDB fixture.
# Out of scope: PBFT mechanics, Check 10 mechanics, kSafe changes, cardinal N/E
# mapping, and N=16 collision metrology.
#
# Protocol wiring
# ---------------
# The dedicated `SixteenVehiclesAdjacentLaneResDB` configuration uses this same
# scalar rule. ResDBPerception derives the lane-normal origin and direction from
# the TraCI lane-0/lane-1 centerlines after SUMO-to-OMNeT coordinate conversion,
# rather than relying on the canonical y values written above. The announcement
# carries one fixed signed centimetre claim plus its derived physical-lane index;
# each witness caches one noisy scalar observation and verdict. Cardinal N/S/E/W
# configurations continue to use the categorical approach channel.
#
# Requirements
# ------------
#   SUMO_HOME pointing at a SUMO install (bin/sumo + tools/)
#   Python 3 with numpy and scipy
#
# Quick start
# -----------
#   export SUMO_HOME=/path/to/sumo
#   export PATH="$SUMO_HOME/bin:$PATH"
#   python3 run_calibration.py
#   python3 analyze_calibration.py
#   python3 -m unittest test_calibration.py -v
#
# Full ROC is 12 δ × 100_000 observations (10 seeds × 10_000). Bulk `.npy`
# traces land in `results/` (gitignored). The checked-in
# `canonical_validation_summary.json` is the compact pass artifact.
#
# Checkpoint pass criteria
# ------------------------
# - TraCI keeps the target on lane A (W2E_0)
# - Centerlines are 3.2 m apart; normal/boundary stable
# - 1 cm quantization deterministic; δ=1.6 → AMBIGUOUS
# - Quantized gate: Bonferroni Wilson interval overlaps Gaussian band at kσ ± 1 cm
#   for all 36 cells (α_cell = 0.05/36)
# - Multiple nontrivial false-lane (δ>1.6) ROC points at σ=0.5
# - Identical seeds → identical samples
