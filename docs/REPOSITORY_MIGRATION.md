# Repository cleanup status

The cleanup is staged on `chore/repo-structure` and keeps protocol namespaces
unchanged. The structure-only checkpoint is complete:

- architecture/spec/command docs are under `docs/`;
- the dispatcher and launch/build helpers have canonical `tools/` paths with
  compatibility wrappers;
- bridge and Veins application snapshots are under `bridge/` and
  `src/v2vbft/app/` and were byte-compared with the validated source;
- scenario inputs and experiment fixtures have experiment-owned manifests;
- current-branch vendor patches and dependency pins are recorded under
  `third_party/`;
- generated keys, overrides, results, and binaries are ignored while existing
  local files are left untouched.

The dependency-extraction checkpoint is intentionally not marked complete until
the pinned upstream trees can be fetched and `tools/setup.sh --verify` passes
against a fresh materialization. This machine currently has no network access;
the existing vendored trees remain the compatibility build inputs meanwhile.
No protocol behavior or namespace was changed by this cleanup.
