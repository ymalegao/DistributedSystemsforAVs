# External dependency pins

`versions.lock` records the upstream revisions used by `setup.sh`. The checked-
in patches capture the current bridge/Veins deltas and reverse-apply cleanly to
the validated vendor trees. Before treating a fresh clone as reproducible,
materialize the pinned revisions and verify the patches there; this requires
network access to the two upstream repositories. The local vendored trees are
left untouched until that verification succeeds.
