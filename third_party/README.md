# Vendored dependencies

This project needs two upstream projects that it patches:

| | Upstream | Pinned at | License |
|---|---|---|---|
| **Veins** | [sommer/veins](https://github.com/sommer/veins) | tag `veins-5.3.1` | GPL-2.0-or-later |
| **ResilientDB** | [apache/incubator-resilientdb](https://github.com/apache/incubator-resilientdb) | commit `1e0896b2` | Apache-2.0 |

Neither tree is committed here. `setup.sh` clones both at the pinned revisions
into `.external/` (gitignored) and applies the patches below.

## What the patches actually change

**`veins.patch`** — 10 files, ~674 lines. Almost all of it is
`TraCIScenarioManager.cc` (+539), which gains the hooks the intersection
protocol needs from the mobility layer: departure recording, crash freezing,
and lane-level queries. It also adds `IIntersectionApp.h`, a 2-method interface
that lets `TraCIScenarioManager` call into the application **without naming a
concrete app type** — that is what allows this repo's protocol to live outside
the Veins tree at all.

It does *not* touch `configure`, `Makefile` or `src/makefrag`: `libveins`
builds exactly as upstream builds it.

**`resilientdb.patch`** — 44 files, ~1,512 changed lines. Mostly the PBFT core
under `platform/consensus/ordering/pbft/`, adapted for a simulated,
socketless, single-process deployment: simulated time (`sim_time_provider`),
forced view changes over proposal-defined membership (`omnet_forced_view.h`),
and pre-verification hooks. The rest is Bazel build fixes for a modern Clang.

The OMNeT bridge itself is **not** in this patch — it is first-class source in
this repo under [`bridge/`](../bridge), which `setup.sh` symlinks to
`.external/resilientdb/integration/omnet` so Bazel builds it as
`//integration/omnet:resdb_omnet_bridge`.

## Regenerating

After editing anything inside `.external/`, run:

```bash
tools/regen-patches.sh
```

It re-clones each upstream at its pinned revision into a scratch directory,
diffs your working tree against it, and rewrites the patch files. Commit the
result. Running `setup.sh --verify` afterwards checks that the patches
reproduce your tree byte-for-byte.
