# Third-party components

This project is licensed **GPL-3.0-or-later** (see [LICENSE](LICENSE)). It is
built on two upstream projects, neither of which is vendored into this
repository — `setup.sh` fetches both at the revisions pinned in
[`third_party/versions.lock`](third_party/versions.lock).

## Veins

- **Upstream:** <https://github.com/sommer/veins>, tag `veins-5.3.1`
- **Copyright:** Christoph Sommer and the Veins contributors
- **License:** GPL-2.0-or-later
- **Used for:** IEEE 802.11p PHY/MAC, the OMNeT++ node model, and SUMO
  integration over TraCI.
- **Modified:** yes — see `third_party/veins.patch` (10 files). The changes add
  hooks to `TraCIScenarioManager` and an `IIntersectionApp` interface.

The protocol under `src/v2vbft/` subclasses Veins' `DemoBaseApplLayer` and
includes Veins headers, so it is a derivative work of Veins.

## ResilientDB (Apache incubating)

- **Upstream:** <https://github.com/apache/incubator-resilientdb>, commit
  `1e0896b24e6a1a804abdb66374d455c9c7e56181`
- **Copyright:** The Apache Software Foundation
- **License:** Apache-2.0
- **Used for:** the PBFT consensus engine each vehicle embeds.
- **Modified:** yes — see `third_party/resilientdb.patch` (44 files), adapting
  PBFT to a simulated, socketless, single-process deployment.

`bridge/` in this repository is original work that links against ResilientDB;
it is not derived from it beyond using its public interfaces.

## Why GPL-3.0-or-later

Apache-2.0 and GPL-2.0-only are incompatible: Apache-2.0's patent-termination
clause is an additional restriction GPL-2.0 does not permit. Apache-2.0 **is**
compatible with GPL-3.0, which accommodates that clause. Veins is licensed
GPL-2.0-**or-later**, so the combined work may be distributed under GPL-3.0.
That makes GPL-3.0-or-later the only coherent licence for a work that both
derives from Veins and links ResilientDB.

> This is the maintainers' reading, not legal advice. If you plan to
> redistribute or build a product on this, get your own counsel.

## Reference papers

`papers/` holds PDFs of prior work the design draws on. They are third-party
publications under their publishers' terms, not covered by this repository's
licence, and are kept here for the authors' reference. If this repository is
published, check each publisher's redistribution terms — several of these are
almost certainly not redistributable, and replacing the directory with a list of
titles and DOIs would avoid the question entirely.
