# Effective Timer Audit (16-vehicle scenarios)

This sheet lists effective timer values after INI inheritance/overrides for:
- `Config SixteenVehiclesBFTOverV2V`
- `Config SixteenVehiclesAmbulanceBFT` (same timers as above)

INI inheritance chain: `[General]` → `BFTOverV2V` (extends `WithChannelSwitching`) → `SixteenVehiclesBFTOverV2V` → (for Ambulance) `SixteenVehiclesAmbulanceBFT`

Units are milliseconds unless noted.
**Source column**: `INI` = explicitly set in `fourway/omnetpp.ini`; `NED` = NED parameter default (not overridden for 16-veh configs).

## INI / NED effective timers (post-override)

| Parameter | Effective Value | Source | What it controls |
|---|---:|:---:|---|
| `sim-time-limit` | `50,000,000 ms` (`50000s`) | INI `[General]` | Max simulation time before stop |
| `*.manager.updateInterval` | `100 ms` (`0.1s`) | INI `[General]` | SUMO <-> OMNeT sync period |
| `*.rsu[*].appl.beaconInterval` | `1,000 ms` (`1s`) | INI `[General]` | RSU beacon transmission period |
| `*.node[*].appl.beaconInterval` | `1,000 ms` (`1s`) | INI `[General]` | Vehicle beacon period (if beaconing enabled) |
| `*.node[*].appl.triggerJoinTime` | `1,000 ms` (`1s`) | NED | Base time offset for JOIN trigger logic |
| `*.node[*].appl.certCollectionTimeoutSec` | `2,000 ms` (`2.0s`) | NED | Leader wait window for `ARRIVAL_CERT` collection |
| `*.node[*].appl.consensusTimeoutSec` | `120,000 ms` (`120s`) | INI `SixteenVehiclesBFTOverV2V` (overrides NED `40s`) | Fallback timeout if consensus quorum not reached |
| `*.node[*].appl.stopSignTimeoutSec` | `25,000 ms` (`25s`) | INI `SixteenVehiclesBFTOverV2V` (overrides `BFTOverV2V` `10s`) | Per-car safety fallback to release to SUMO |
| `*.node[*].appl.orderDelayGap` | `0 ms` (`0s`) | NED (matches default; see note ①) | Extra delay before ORDER submit (leader diagnostics) |
| `*.node[*].appl.viewJitterMin` | `1 ms` (`0.001s`) | NED | Minimum random jitter for view/echo sends |
| `*.node[*].appl.viewJitterMax` | `5 ms` (`0.005s`) | NED | Maximum random jitter for view/echo sends |
| `*.node[*].appl.viewAgreementSlotSec` | `25 ms` (`0.025s`) | NED | Slot spacing used for agreement/echo staggering |
| `*.node[*].appl.ackJitterMin` | `1 ms` (`0.001s`) | NED | Minimum ACK jitter |
| `*.node[*].appl.ackJitterMax` | `10 ms` (`0.010s`) | NED | Maximum ACK jitter |
| `*.node[*].appl.witnessSlotSec` | `2 ms` (`0.002s`) | NED | Slot spacing for witness-response bursts |
| `*.node[*].appl.broadcastSlotSec` | `5 ms` (`0.005s`) | NED | Slot spacing for generic broadcast message types |
| `*.node[*].appl.arrivalSlotSec` | `25 ms` (`0.025s`) | NED | Slot spacing for arrival announcements |
| `*.node[*].appl.broadcastJitterMin` | `0.1 ms` (`0.0001s`) | NED | Minimum broadcast jitter |
| `*.node[*].appl.broadcastJitterMax` | `1 ms` (`0.001s`) | NED | Maximum broadcast jitter |
| `*.node[*].appl.orderCollectMinSec` | `4,000 ms` (`4.0s`) | NED | ORDER collection minimum window |
| `*.node[*].appl.orderCollectPerReplicaSec` | `80 ms` (`0.08s`) | NED | ORDER collection additive window per replica |
| `*.node[*].appl.orderCollectMaxSec` | `5,000 ms` (`5s`) | NED | ORDER collection hard max window |

## Constructor/runtime protocol timers (C++ — fixed unless code changes)

| Parameter | Value | What it controls |
|---|---:|---|
| `retxCheckTimer` initial schedule | `20 ms` (`0.02s`) | Starts JNI retransmission polling loop |
| `retxCheckTimer` recurrence | `2–6 ms` (`0.001 + uniform(0.001..0.005)s`) | Ongoing retransmission check cadence |
| `checkJavaReadyTimer` initial schedule | `500 ms` (`0.5s`) | First readiness probe for Java replica |
| `checkJavaReadyTimer` retry cadence | `100 ms` (`0.1s`) | Probe interval while Java is not ready |
| `processQueueTimer` initial schedule | `100 ms` (`0.1s`) | Starts Java→OMNeT outgoing queue drain |
| `processQueueTimer` recurrence | `50 ms` (`0.05s`) | Queue drain heartbeat |
| `checkPositionTimer` initial schedule | `500 ms` (`0.5s`) | Starts intersection proximity/phase checks |
| `checkPositionTimer` recurrence | `50 ms` (`0.05s`) | Position/clearance control loop cadence |
| `CLEARANCE_TIMEOUT` | `60,000 ms` (`60s`) | Max wait for expected batch departures before force-advance |

## Java BFT-SMaRt layer timer constants (RequestsTimer.java — fixed unless code changes)

These are `static final` constants compiled into the JAR. Override at JVM launch via `-D<property>=<N>`.

| Constant | Default Value | JVM property override | What it controls |
|---|---:|:---|---|
| `JITTER_WALL_MS` | `0 ms` | `-Dbftsmart.lc_jitter_wall_ms` | Wall-clock jitter added before STOP/LC messages enter JNI queue (zeroed; C++ slot stagger handles collision avoidance) |
| `STOP_RETX_WALL_MS` | `200 ms` | `-Dbftsmart.stop_retx_wall_ms` | Wall-clock poll cadence for `SendStopTask` checking whether a STOP re-emission is due |
| `STOP_RETX_SIM_MS` | `200 ms` | `-Dbftsmart.stop_retx_sim_ms` | Minimum sim-time gap between successive STOP emissions from the same replica per regency (gates channel saturation) |

## BFT-SMaRt system.config values (fourway/config/system.config)

| Parameter | Value | What it controls |
|---|---:|---|
| `system.servers.num` | `16` | Total replica count |
| `system.servers.f` | `5` | Max tolerated Byzantine faults |
| `system.totalordermulticast.timeout` | `2,000 ms` (`2s`) | Request timeout triggering leader-change (see note ②) |
| `system.totalordermulticast.batchtimeout` | `-1` (disabled) | Batch accumulation window; disabled → next consensus fires immediately |
| `system.totalordermulticast.maxbatchsize` | `1` | Max requests per consensus batch |
| `system.client.invokeOrderedTimeout` | `60,000,000 ms` (`60000s`) | Client-side ordered-invoke deadline |
| `system.bft` | `true` | Byzantine fault tolerance mode enabled |
| `system.byzantine.maliciousReplicaIds` | _(empty)_ | Fault injection disabled by default |

## OMNeT PHY/MAC/connection parameters (fourway/omnetpp.ini — [General])

| Parameter | Value | What it controls |
|---|---:|---|
| `*.connectionManager.sendDirect` | `true` | Uses direct send path in connection manager |
| `*.connectionManager.maxInterfDist` | `2600m` | Maximum interference distance for connectivity/interference checks |
| `*.connectionManager.drawMaxIntfDist` | `false` | Disables drawing interference radius in visualization |
| `*.connectionManagerName` | `"connectionManager"` | Name used by nodes to bind to connection manager module |
| `*.**.nic.mac1609_4.useServiceChannel` | `false` | MAC stays on control channel (WithChannelSwitching sets false) |
| `*.**.nic.phy80211p.txPower` | `20mW` | PHY transmit power |
| `*.**.nic.mac1609_4.txPower` | `20mW` | MAC-level transmit power |
| `*.**.nic.mac1609_4.bitrate` | `6Mbps` | 802.11p MAC data rate |
| `*.**.nic.phy80211p.minPowerLevel` | `-110dBm` | Receiver minimum detectable power threshold |
| `*.**.nic.mac1609_4.queueSize` | `1024` | MAC transmit queue capacity |
| `*.**.nic.phy80211p.useNoiseFloor` | `true` | Enables fixed noise floor modeling |
| `*.**.nic.phy80211p.noiseFloor` | `-98dBm` | Configured background noise floor |

## Override / inheritance notes

- `consensusTimeoutSec`: NED default `40s` → overridden to `120s` in `SixteenVehiclesBFTOverV2V`.
- `stopSignTimeoutSec`: `BFTOverV2V` sets `10s` → overridden to `25s` in `SixteenVehiclesBFTOverV2V` → redundantly re-set to `25s` in `SixteenVehiclesAmbulanceBFT`. **Effective: 25s.**
- Constructor C++ defaults (`consensusTimeoutSec=80s`, `certCollectionTimeoutSec=1.5s`) are replaced during `initialize()` by NED/INI reads before any protocol runs.
- All `orderCollect*`, `jitter*`, `slot*` params are **NED defaults** — not set anywhere in `omnetpp.ini`. Partner must use the same `.ned` file.

### ① orderDelayGap INI typo (harmless)

`SixteenVehiclesBFTOverV2V` contains:
```
*.intersection[*].appl.orderDelayGap = 0.0s
```
This targets a non-existent `intersection` module (should be `*.node[*]`). The parameter never reaches the vehicles. Harmless because the NED default is also `0s`, so the effective value is correct. Fix the typo before setting a non-zero value here.

### ② totalordermulticast.timeout — memory vs. actual

A prior session recorded that this was raised to `4000ms`. Current `system.config` shows **`2000ms`**. If the raise was reverted, update memory. If the 4000ms config is the intended experiment value, restore it before running.
