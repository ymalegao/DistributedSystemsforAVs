# Longitudinal distance calibration

This standalone SUMO/TraCI fixture validates the longitudinal physical channel
before any ResDB wire or scheduler changes.

Seven standard 4.5 m vehicles are stopped on one lane. Their bumper clearances
are `0.5, 1, 2, 3, 5, 8 m`, so their front-reference separations are
`5, 5.5, 6.5, 7.5, 9.5, 12.5 m`. The distinction is mandatory: the pairwise
sensor-inversion formula consumes reference-point separation, not empty space
between bumpers.

For each rear vehicle, the attack claims a distance `0.1 m` ahead of the
physically preceding vehicle. Thus `|Delta_long| = separation + 0.1 m`.
Witness samples use `d_obs=d_true+N(0,sigma_long^2)`, centimetre quantization,
and `abs(d_obs-d_claim)<=k*sigma_long`. The exact same residual samples are
re-thresholded at `k={2,2.5,3}`; main experiments use `k=3`.

The fixture reports two intentionally different quantities:

- per-car false-distance acceptance, predicted by the one-dimensional Gaussian
  interval in the PicaBFT specification;
- raw pairwise observation inversion, predicted by
  `Phi(-separation/(sqrt(2)*sigma_long))`.

Neither quantity alone is labeled a committed schedule violation. That is a
later protocol-level metric requiring finalized certificate bytes.

Run inside the v2v environment:

```bash
python3 run_calibration.py
python3 -m unittest test_calibration.py -v
```

The compact pass artifact is `canonical_validation_summary.json`. Bulk results
under `results/` are ignored. This checkpoint does not modify PBFT, Check 10,
`kSafe`, direction eligibility, QUIET handling, or certificate timers.
