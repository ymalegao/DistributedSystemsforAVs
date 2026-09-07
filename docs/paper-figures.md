# Compact paper figures

Each lane configuration is exported separately. There are ten PNGs (five types
per lane), with eight axes per lane instead of the previous 27. PNG exports are
300 DPI. Use `python3 -m plotter list`, `build <name>`, or `build-all`.
`python3 -m plotter summary` writes the supporting results table.

## Baseline and priority

`ab3_baseline_{1lane,2lane}.png`: a single latency–throughput axis. Color/shape
identifies actuated signal, BFT without priority, and BFT with priority. Large
markers are means of paired run-level latency/throughput measurements; faint
markers are individual repetitions. Numeric labels are vehicle counts, joined
in increasing load order (not a fitted capacity curve). Lower latency and higher
throughput are preferable. Repetition IDs are intersected across conditions.

The signal uses study 3 `tl`; BFT uses study 4 `noprio` and `prio`. The runner
uses identical base scenarios and repetition seeds for the two BFT conditions,
with only the ambulance override added for priority. The signal baseline uses
the matched signal scenario. Study 3 `ours` is not pooled with `noprio`.

`ab3_vehicle_latency_{1lane,2lane}.png`: every measured vehicle/repetition latency
at each load. Horizontal jitter is deterministic and has no numerical meaning.
Stars identify the ambulance ID recorded in each priority-on run and that same
vehicle in both matched controls. Two older one-lane N=8 runs designate veh6
and veh2, respectively, rather than N−1; no fixed-ID assumption is used. Dots are observations within runs,
not independent simulation repetitions. Vehicles lacking a complete stop/clear
pair are counted on the figure rather than assigned zero latency.

Latency is clearance time minus stop time. Throughput is recorded vehicles
cleared divided by the interval from first stop to last recorded clearance.
It includes arrival timing and fallback movement, so compare conditions at
matched load. Neither metric alone proves consensus success. Latency includes
only vehicles with complete timestamps; report non-clearing vehicles separately.

## Rollback

`ab5_rollback_{1lane,2lane}.png`: two panels. The first shows departure events
from the lowest repetition ID shared by both conditions, without averaging
vehicle timestamps. Simultaneous departures are separated vertically; stars
identify ambulances. Row labels report actual recorded clearance counts.
Missing clearances are listed explicitly, without inventing a time of departure.
The second panel shows latency and throughput across matched repetitions using
the same definitions as the baseline. Do not claim equal clearance counts unless
the measured counts actually agree. Additional costs remain in `results.md`.

## RSU

`ab1_rsu_{1lane,2lane}.png`: two panels. The percentage grid shows repetitions
with at least one observed ORDER decision, by silent-vehicle count and RSU
condition. A dash is unmeasured, not zero success. Each current cell has three
repetitions, so percentages have coarse resolution. All four RSUs and the vehicle
primary remain active; only vehicle replicas 1 through k are made PBFT-silent.
This is observed silent-replica tolerance, not an arbitrary Byzantine guarantee.

Interleaved log lines can remove per-vehicle decision evidence. Requiring every
vehicle's intact decision line would misclassify missing evidence as failure.
The grid therefore reports the narrower, explicit at-least-one-decision metric.
An audit of per-vehicle coverage must not be interpreted as a failure rate.
The second panel is latency–throughput at k=0; labels identify vehicle counts.

## Protocol cost

`an_cost_decomposition_{1lane,2lane}.png`: two panels using RSU-off, k=0 runs.
Both communication series now use messages per intersection round. Arrival
certificates include types 1/4/5; PBFT includes 8/11. Gossip and recovery traffic
are excluded, so the two bars are not a complete total of network traffic.
The timing panel compares announcement-to-certificate latency and ORDER latency
using the existing aggregation definitions; these timings are not additive.

## Retired outputs

Ablation 2 has no figure module, registered CLI name, discovered input cells,
or runner option. Its raw logs remain intact. Standalone priority figures are
also retired, but study 4 remains runnable because it supplies baseline data.
`build-all` prunes obsolete PNGs. The protocol's fault-injection implementation
is unchanged.
