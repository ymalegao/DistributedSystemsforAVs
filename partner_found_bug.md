# PBFT vs RAFT V2V Intersection Benchmark — Comparison Issues

## Context for the LLM Reading This

Two independent V2V intersection-coordination systems are being compared:

| Property | System A — **RAFT** | System B — **PBFT** (your project) |
|---|---|---|
| Algorithm | RAFT (crash fault tolerant) | PBFT via BFT-SMaRt (Byzantine fault tolerant) |
| Transport | 802.11p WAVE, pure C++ in OMNeT++ | 802.11p WAVE in OMNeT++ + Java BFT-SMaRt via JNI |
| Simulator | OMNeT++ 5.6.2 + Veins + SUMO | OMNeT++ + Veins 5.3.1 + SUMO |
| Language | C++ only | C++ (Veins) + Java (BFT-SMaRt) bridged via JNI |

**Your project** is the PBFT one. All issues and fixes described below are in your codebase:
`DistributedSystemsforAVs/`

The goal is an apple-to-apple comparison — same radio model, same timing methodology, same
measurement units. Currently that comparison is invalid for the reasons listed below.

**Before making any change, read the relevant file yourself to verify the issue exists exactly as described. Code may have changed. Always confirm before editing.**

---

## Issue 1 — PRIMARY: Consensus Latency is Measured with Java Wall Clock, Not Simulation Time

### What the issue is

OMNeT++ runs a **discrete-event simulation**. Time inside the simulator is `simTime()` — it is
not real clock time. One second of simulated time might complete in 10ms or 10 minutes of real
time depending on how fast the machine is.

BFT-SMaRt is a Java library that runs in a **real OS process** on the JVM. It uses
`System.currentTimeMillis()` — this is the machine's real wall clock.

These two clocks are completely independent. Measuring consensus latency with
`System.currentTimeMillis()` in Java means you are measuring **how fast the Java process ran on
this particular machine**, not how long consensus took in the simulated V2V network. If the host
machine is under load, latency goes up. If Java has a warm JIT, it goes down. None of this
reflects the simulated radio environment.

RAFT measures everything with `simTime()` in C++, so its numbers are simulation-time seconds —
reproducible, machine-independent, and correctly reflecting the 802.11p radio delays.

### What it is causing

The primary consensus latency graph (`propose_all_consensus_latency_s`) shows Java CPU
speed, not PBFT protocol speed over V2V. The numbers are incomparable to RAFT's latency numbers
which are in simulation seconds.

### The full chain — trace this yourself before changing anything

**Step 1 — clock starts in Java:**
File: `bftsmart/library/src/main/java/bftsmart/demo/intersection/IntersectionServer.java`
Search for: `consensusStartWall = System.currentTimeMillis()`
This is where the wall clock starts when a PROPOSE_ALL round begins.

**Step 2 — clock stops in Java:**
Same file. Search for: `long deliveredWall = System.currentTimeMillis()`
The difference `(deliveredWall - consensusStartWall) / 1000.0` is the wall-clock seconds.

**Step 3 — sent to C++ via JNI:**
Same file. Search for: `notifyProposeAllConsensusMetric`
This calls a native method that passes the wall-clock seconds across to C++.

**Step 4 — C++ logs it:**
File: `veins-veins-5.3.1/src/veins/modules/bftsmart/V2VProxyModule.cc`
Search for: `ProposeAll_Consensus_Wall`
This is the log line that `analyze_log.py` parses.

**Step 5 — Python parses it:**
File: `fourway/analyze_log.py`
Search for: `RE_BFTCONS_PROPOSE_ALL`
This regex extracts the value and stores it in `propose_all_wall_raw`.

**Step 6 — written to JSON:**
Same file, line where `"propose_all_consensus_latency_s"` is assigned.
Value is `propose_all_lat` which came from `propose_all_wall_raw` — the wall clock.

**Step 7 — plotted:**
Whatever script reads `propose_all_consensus_latency_s` from the JSON to draw the graph.
This is the field that should be replaced.

### The good news — a correct sim-time measurement already exists

In `V2VProxyModule.cc`, search for `proposeAllSubmitTime` and `orderConsensusEndTime`.
These are both `simtime_t` variables. Their difference is computed as `durProposeAllSim` and
logged as `PROPOSE_ALL_BFT(sim)=`.

`analyze_log.py` already parses this line and writes it to every JSON as:
```json
"propose_all_consensus_latency_sim_s": <value>
```

This field already exists in your JSON output. It is just never read by the plot script.

### What needs to be changed

**In `fourway/analyze_log.py` and any benchmark plotting script:**

Find every place that reads `"propose_all_consensus_latency_s"` and replace with
`"propose_all_consensus_latency_sim_s"`.

Check these files:
- `fourway/analyze_log.py` — lines near 569 and 647

For each file: read it, search for `propose_all_consensus_latency_s`, and replace with
`propose_all_consensus_latency_sim_s`. Do not blindly search-replace — some occurrences may be
in comments or string labels for graph axes; those do not need to change (or can say "sim").

### How to verify the fix is correct

After the change, regenerate JSON from a log file and confirm:
1. `propose_all_consensus_latency_s` is still written (for backwards compat — leave it)
2. `propose_all_consensus_latency_sim_s` is now what gets plotted
3. The sim-time values should be consistent with V2V message round-trip time (~2–20ms per hop
   for 802.11p at short range), scaled by the number of PBFT message rounds. If you see values
   of 100ms–500ms in simulation time that is plausible. If you see values of 1–30 seconds that
   likely includes stop-sign or queuing wait time, not just consensus.

---

## Issue 2 — Self-Delivery Bypasses the 802.11p Radio Stack

### What the issue is

In BFT-SMaRt consensus, every replica sends messages to every other replica — and also to
**itself** (this is required by the PBFT algorithm). When a replica sends to itself, the message
should logically pass through the network too, but the current implementation short-circuits it.

**File:** `bftsmart/library/src/main/java/bftsmart/communication/V2V/V2VServersCommunicationLayer.java`

Look at the `send()` method. When `target == me`, it calls `deliverToBFTSmart(sm)` directly —
this is a plain Java method call. The message never enters OMNeT++, never touches the 802.11p
MAC layer, never experiences propagation delay or channel contention.

**RAFT**, by contrast, puts every single message — including its own — through
`sendDelayedDown()`, which injects it into the full Veins 802.11p radio stack with a small
random backoff (`uniform(0.001, 0.005)` seconds).

### What it is causing

For every consensus round, PBFT gets some messages delivered instantly (self-messages) while
RAFT pays the full radio delay for equivalent messages. This makes PBFT appear faster in
simulation even if the protocol is theoretically slower.

Additionally, PBFT has more message rounds than RAFT (PBFT requires 3 phases: PROPOSE, WRITE,
ACCEPT; RAFT uses 2 phases: AppendEntries, response). So the radio bypass saves PBFT
proportionally more time.

### What needs to be checked

In `V2VServersCommunicationLayer.java`, find the `send()` method and look for:
```java
if (target == me) {
    deliverToBFTSmart(sm);
```

Confirm this is a direct Java call, not routed through JNI → C++ → OMNeT++ → radio.

Also check `ReliableV2VMessaging.java` and any other sender classes for the same pattern.

### What needs to be changed

For a fair comparison, self-messages should also be routed through the JNI bridge to OMNeT++
so they experience the same radio model. However this is architecturally complex.

If you want to fix it properly: route the `target == me` case through the same JNI path as
remote targets, then let OMNeT++ echo it back. This requires the C++ side to route messages
back to Java when the source and destination replica IDs match. (LETS DO THIS I THINK for the message it would just be (send from java->jni->c++->jni->java), make it pass through and process in c++ so everything is processed in c++) 

### How to verify

After any fix, add logging: when a self-message is sent and when it is delivered. Confirm
the delivery timestamp is later than the send timestamp by at least the propagation delay
(~microseconds for co-located nodes, ~milliseconds for opposite-side vehicles).

---

## Issue 3 — realtimescheduler-scaling Decouples Java Timers from Simulation Time

### What the issue was when this document was made: 

In `fourway/omnetpp.ini`, line 3:
```ini
scheduler-class = "omnetpp::cRealTimeScheduler"
realtimescheduler-scaling = 0.1
```

now its 
```ini
scheduler-class = "omnetpp::cRealTimeScheduler"
realtimescheduler-scaling = 1
```

But this creates a problem for measurement: `realtimescheduler` does NOT make Java's
`System.currentTimeMillis()` reflect simulation time. Java still measures real wall-clock
milliseconds. If Java takes 120ms to complete a BFT round and OMNeT++ has scaled that to
1200ms of simulation time, the Java timer records 0.12 seconds but the simulator clock shows
1.2 seconds. They are 10× apart.

### What it is causing

The `realtimescheduler` was meant to prevent message ordering issues, not to make Java timers
accurate. Using wall clock time while also having realtimescheduler creates a measurement that
is neither the true wall clock nor the true simulation time — it's a ratio that varies
depending on how busy the host machine is.

### What needs to be checked

Verify whether Java's timing measurements in `IntersectionServer.java` are used anywhere
in a way that assumes they reflect simulation time. Specifically: is there any place where
a Java-measured duration is fed back into OMNeT++ as a delay or schedule time? If so, that
is a direct coupling bug.

Check `V2VProxyModule.cc` for any code that reads a Java-provided duration and uses it in
`scheduleAt(simTime() + javaDuration, ...)`.

### What needs to be changed

Issue 1's fix (using `propose_all_consensus_latency_sim_s`) eliminates the direct measurement
problem. The realtimescheduler itself can stay — it serves a valid architectural purpose
(preventing Java starvation). Just do not use any Java wall-clock duration as a simulation
metric.

---


---

## Summary of Changes — Ordered by Priority

| Priority | File | Change |
|---|---|---|
| 1 — MUST fix | `fourway/analyze_log.py` | Replace `propose_all_consensus_latency_s` → `propose_all_consensus_latency_sim_s` in plot logic |
| 2 — MUST disclose or fix | `V2VServersCommunicationLayer.java` `send()` | Self-delivery bypasses radio — document or route through JNI |
| 3 — Verify | `fourway/omnetpp.ini` `[Config BFT4Replicas]` | Confirm this config is not used in benchmarks; if used, set `useServiceChannel = false` |

---

## What the LLM Should Do — Step by Step

1. **Read `fourway/analyze_log.py` in full.** Locate every occurrence of
   `propose_all_consensus_latency_s` that appears in a value-assignment or data-read context
   (not in a key-name string that writes both fields). Change those reads to use the `_sim_s`
   variant. Do not change the lines that *write* both fields to JSON — keep both fields in the
   output for backwards compatibility.


3. **Read `V2VServersCommunicationLayer.java` `send()` method.** Confirm the self-delivery
   shortcut. Add a code comment documenting that self-messages bypass the radio stack, so
   this is a known asymmetry versus RAFT. Only attempt the architectural fix if you fully
   understand the JNI round-trip mechanism in this codebase.

5. **After all changes:** I will run the configurations and then run the `analyze_log.py` script on an existing log file to regenerate
   JSON, then run the plot script to regenerate graphs. Verify the consensus latency values
   are now in the range of 5ms–500ms (simulation seconds for 802.11p V2V). Values in the
   range of 1–30 seconds strongly suggest wall clock contamination is still present.