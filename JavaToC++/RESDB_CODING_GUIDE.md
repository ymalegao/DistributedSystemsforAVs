# ResDB Veins layer — coding guide

Keep **`ResDBIntersectionApp.cc`** focused on **module lifecycle, message dispatch, and consensus triggers**. When adding behavior, **prefer new translation units** or **`ResDBTraCI.cc`** over growing the main app file without bound.

## Reference implementation

Use **[`V2VProxyModule.h`](../veins-veins-5.3.1/src/veins/modules/bftsmart/V2VProxyModule.h)** and **[`V2VProxyModule.cc`](../veins-veins-5.3.1/src/veins/modules/bftsmart/V2VProxyModule.cc)** as the **organizational model**:

- One application module class inheriting `DemoBaseApplLayer`.
- **Clear sections** in the `.cc` file (handlers, timers, cert protocol, JNI — in ResDB: radio, cert, propose, TraCI callbacks).
- **Heavy helpers** live **beside** the module in dedicated sources when they are reusable or bulky (e.g. TraCI helpers split out — same idea as keeping crypto in `CryptoAuth`).

ResDB does **not** use JNI, but the **split between “orchestration in the module” vs “helpers elsewhere”** still applies.

## Rules of thumb

1. **`ResDBIntersectionApp.{cc,h}`** — `initialize`, `handleSelfMsg`, `onWSM`, phase transitions, **`proposeAll`**, cert assembly hooks, and thin wrappers that call into helpers.
2. **`ResDBTraCI.cc` / `ResDBTraCI.h`** — distance-to-intersection, stop/resume, lane discovery, **`vehicleHasClearedIntersectionTraCI`**, and similar **SUMO/TraCI-only** logic.
3. **New domain slices** (e.g. a dedicated arrival-protocol helper namespace, or metrics logging) — **new `.cc/.h` pair** under `resDB/` rather than hundreds of lines inline in the app.
4. **Do not** paste large standalone functions into the bottom of `ResDBIntersectionApp.cc` “for convenience” — if it isn’t a module override or a one-off five-liner, **extract it**.
5. **Match naming and structure** of existing Veins code in this tree (logging prefixes, `sendBFTMessage` patterns, NED parameters for tunables).

## Docs tie-in

- **[`5stepplan.md`](5stepplan.md)** — implementation status, gaps, build/run, **handoff** notes.
- **[`RESDBARCH.md`](RESDBARCH.md)** — architecture (legacy Java vs ResDB, bridge PreVerify, phases).
