#!/usr/bin/env python3
"""
Analyze the 5 ablation studies -> benchmarks/ablations/figures/.

Each ablation uses the metric that actually shows its difference:
  1 RSU on/off        : consensus latency + messages (overhead) at k=0, and
                        commit-rate vs k (fault tolerance)
  2 vanilla vs our BFT: FALSE_PRIORITY_GRANTED under a fake_ambulance attack
                        (vanilla commits the fake ambulance; ours blocks it)
  3 baseline vs ours  : per-car wait time at the intersection (Resume - Stop)
  4 priority on/off   : the ambulance's (node[3]) wait time
  5 rollback on/off   : was the late ambulance safely re-ordered

Reads results/ab<N>_*.log. Usage: python3 analyze_ablations.py
"""
import os, re, sys, glob, statistics
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")
FIGS = os.path.join(HERE, "figures"); os.makedirs(FIGS, exist_ok=True)

RE_MSG   = re.compile(r"\[METRICS (\d+)\]\s+Messages_Sent:\s+(\d+)")
RE_BFT   = re.compile(r"PROPOSE_ALL_BFT\(sim\)=([\d.]+)s")
RE_ORD   = re.compile(r"Order_Decided_Time")
RE_FPG   = re.compile(r"FALSE_PRIORITY_GRANTED")
RE_STOP  = re.compile(r"\[METRICS (\d+)\]\s+Stop_Time:\s+([\d.]+)")
RE_RESUME= re.compile(r"\[METRICS (\d+)\]\s+Resume_Time:\s+([\d.]+)")
RE_ROLLB = re.compile(r"CANCEL-COMMIT|ROLLBACK-BEGIN")


def last_vals(path, rx):
    d = {}
    with open(path, errors="ignore") as fh:
        for line in fh:
            m = rx.search(line)
            if m:
                d[int(m.group(1))] = float(m.group(2))
    return d

def has(path, rx):
    with open(path, errors="ignore") as fh:
        return any(rx.search(l) for l in fh)

def count(path, rx):
    n = 0
    with open(path, errors="ignore") as fh:
        for l in fh:
            if rx.search(l): n += 1
    return n

def total_msgs(path):
    return sum(last_vals(path, RE_MSG).values())

def mean_latency(path):
    lat = []
    with open(path, errors="ignore") as fh:
        for l in fh:
            m = RE_BFT.search(l)
            if m: lat.append(float(m.group(1)))
    return statistics.mean(lat) if lat else None

def wait_times(path):
    """per-replica wait = Resume_Time - Stop_Time (both baseline & BFT log these)."""
    stop = last_vals(path, RE_STOP); res = last_vals(path, RE_RESUME)
    return {r: res[r] - stop[r] for r in res if r in stop and res[r] >= stop[r]}

def g(files, key): return sorted(glob.glob(os.path.join(RES, files)), key=key)


def plt_setup():
    import matplotlib; matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    return plt

RED, GRN = "#d1495b", "#2e8b57"


def ablation1(plt):
    # overhead at k=0 + commit-rate vs k
    runs = defaultdict(list)  # (arm,k)->[(msgs,lat,committed)]
    for p in glob.glob(os.path.join(RES, "ab1_*_k*_rep*.log")):
        m = re.search(r"ab1_(OFF|ON)_k(\d+)_rep\d+\.log", os.path.basename(p))
        if not m: continue
        runs[(m.group(1), int(m.group(2)))].append(
            (total_msgs(p), mean_latency(p), 1 if has(p, RE_ORD) else 0))
    if not runs: return None
    ks = sorted({k for _, k in runs})
    def mean(arm, k, i):
        v = [r[i] for r in runs.get((arm,k),[]) if r[i] is not None]
        return statistics.mean(v) if v else None
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.4))
    # commit rate vs k
    a1.plot(ks, [ (mean("OFF",k,2) or 0)*100 for k in ks], "o-", color=RED, label="without RSU (N=4,f=1)")
    a1.plot(ks, [ (mean("ON", k,2) or 0)*100 for k in ks], "s-", color=GRN, label="with 4 RSU (N=8,f=2)")
    a1.set_xlabel("PBFT-silent replicas (k)"); a1.set_ylabel("runs reaching consensus (%)")
    a1.set_title("Fault tolerance", fontsize=11); a1.set_xticks(ks); a1.set_ylim(-5,105)
    a1.legend(fontsize=8); a1.grid(alpha=.3)
    # overhead at k=0, RELATIVE to without-RSU (single %-axis: no dual-axis / no faded colors)
    om, nm = mean("OFF",0,0), mean("ON",0,0)
    ol, nl = mean("OFF",0,1), mean("ON",0,1)
    groups = []  # (label, without%=100, with%)
    if om and nm: groups.append(("messages", nm/om*100, nm/om))
    if ol and nl: groups.append(("latency",  nl/ol*100, nl/ol))
    xs = list(range(len(groups))); w = .36
    a2.axhline(100, color="#888", lw=.9, ls="--", zorder=0)
    a2.bar([i-w/2 for i in xs], [100]*len(groups), w, color=RED, label="without RSU (baseline)")
    a2.bar([i+w/2 for i in xs], [gg[1] for gg in groups], w, color=GRN, label="with RSU")
    for i, gg in enumerate(groups):
        a2.text(i+w/2, gg[1]+2, f"+{gg[1]-100:.0f}%", ha="center", fontsize=10, color=GRN, weight="bold")
    a2.set_xticks(xs); a2.set_xticklabels([gg[0] for gg in groups])
    a2.set_ylabel("cost relative to without-RSU (%)")
    a2.set_ylim(0, (max(gg[1] for gg in groups)*1.25) if groups else 140)
    a2.set_title("Overhead at k=0 (RSU cost)", fontsize=11); a2.legend(fontsize=8, loc="lower right")
    fig.suptitle("Ablation 1 — RSU units: +fault tolerance at a small overhead cost", fontsize=12)
    fig.tight_layout(); fig.savefig(os.path.join(FIGS,"ab1_rsu.png"), dpi=130)
    return {k: (mean("OFF",k,2), mean("ON",k,2)) for k in ks}


def ablation2(plt):
    # attack success rate: FALSE_PRIORITY_GRANTED present => attack committed
    arm_succ = {}
    for arm in ("ours","vanilla"):
        fs = glob.glob(os.path.join(RES, f"ab2_{arm}_rep*.log"))
        if not fs: continue
        succ = [1 if count(p, RE_FPG) > 0 else 0 for p in fs]
        arm_succ[arm] = (statistics.mean(succ)*100, len(fs))
    if not arm_succ: return None
    fig, ax = plt.subplots(figsize=(6,4.4))
    labels = [("ours (f+1 firewall)","ours",GRN), ("vanilla BFT (no f+1)","vanilla",RED)]
    xs = range(len([l for l in labels if l[1] in arm_succ]))
    i=0; ticks=[]
    for name,key,col in labels:
        if key not in arm_succ: continue
        v,n = arm_succ[key]
        ax.bar(i, v, .6, color=col); ax.text(i, v+2, f"{v:.0f}%\n(n={n})", ha="center", fontsize=9)
        ticks.append(name); i+=1
    ax.set_xticks(range(len(ticks))); ax.set_xticklabels(ticks)
    ax.set_ylabel("runs where fake-ambulance attack COMMITTED (%)"); ax.set_ylim(0,115)
    ax.set_title("Ablation 2 — the f+1 pre-verification firewall blocks an attack\n"
                 "vanilla BFT commits the fake ambulance (unsafe); ours rejects it", fontsize=10)
    ax.grid(alpha=.3, axis="y")
    fig.tight_layout(); fig.savefig(os.path.join(FIGS,"ab2_attack.png"), dpi=130)
    return arm_succ


def ablation3(plt):
    # per-car wait time: baseline vs ours
    arm_wait = {}
    for arm in ("baseline","ours"):
        fs = glob.glob(os.path.join(RES, f"ab3_{arm}_rep*.log"))
        vals=[]
        for p in fs:
            w = wait_times(p)
            if w: vals.append(statistics.mean(w.values()))
        if vals: arm_wait[arm] = (statistics.mean(vals), min(vals), max(vals), len(vals))
    if not arm_wait: return None
    fig, ax = plt.subplots(figsize=(6,4.4))
    order=[("SUMO all-way-stop","baseline",RED),("our BFT protocol","ours",GRN)]
    i=0; ticks=[]
    for name,key,col in order:
        if key not in arm_wait: continue
        mn,lo,hi,n = arm_wait[key]
        ax.bar(i, mn, .6, color=col, yerr=[[mn-lo],[hi-mn]], capsize=4)
        ax.text(i, mn, f"{mn:.1f}s", ha="center", va="bottom", fontsize=9)
        ticks.append(name); i+=1
    ax.set_xticks(range(len(ticks))); ax.set_xticklabels(ticks)
    ax.set_ylabel("mean per-car wait at intersection (s)")
    ax.set_title("Ablation 3 — conventional all-way-stop vs our protocol\n(lower wait = faster throughput)", fontsize=10)
    ax.grid(alpha=.3, axis="y")
    fig.tight_layout(); fig.savefig(os.path.join(FIGS,"ab3_baseline.png"), dpi=130)
    return arm_wait


def ablation4(plt):
    # ambulance (node[3]) wait time: priority on vs off
    arm_w = {}
    for arm in ("prio","noprio"):
        fs = glob.glob(os.path.join(RES, f"ab4_{arm}_rep*.log"))
        vals=[]
        for p in fs:
            w = wait_times(p)
            if 3 in w: vals.append(w[3])
        if vals: arm_w[arm] = (statistics.mean(vals), min(vals), max(vals), len(vals))
    if not arm_w: return None
    fig, ax = plt.subplots(figsize=(6,4.4))
    order=[("no priority (FIFO)","noprio",RED),("ambulance priority","prio",GRN)]
    i=0; ticks=[]
    for name,key,col in order:
        if key not in arm_w: continue
        mn,lo,hi,n = arm_w[key]
        ax.bar(i, mn, .6, color=col, yerr=[[mn-lo],[hi-mn]], capsize=4)
        ax.text(i, mn, f"{mn:.1f}s", ha="center", va="bottom", fontsize=9)
        ticks.append(name); i+=1
    ax.set_xticks(range(len(ticks))); ax.set_xticklabels(ticks)
    ax.set_ylabel("ambulance wait at intersection (s)")
    ax.set_title("Ablation 4 — priority scheduling gets the ambulance through sooner", fontsize=10)
    ax.grid(alpha=.3, axis="y")
    fig.tight_layout(); fig.savefig(os.path.join(FIGS,"ab4_priority.png"), dpi=130)
    return arm_w


def ablation5(plt):
    # late ambulance safely handled: rollback on vs off
    arm = {}
    for a in ("on","off"):
        fs = glob.glob(os.path.join(RES, f"ab5_rollback_{a}_rep*.log"))
        if not fs: continue
        handled = [1 if has(p, RE_ROLLB) else 0 for p in fs]
        arm[a] = (statistics.mean(handled)*100, len(fs))
    if not arm: return None
    fig, ax = plt.subplots(figsize=(6,4.4))
    order=[("rollback OFF","off",RED),("rollback ON","on",GRN)]
    i=0; ticks=[]
    for name,key,col in order:
        if key not in arm: continue
        v,n = arm[key]
        ax.bar(i, v, .6, color=col); ax.text(i, v+2, f"{v:.0f}%\n(n={n})", ha="center", fontsize=9)
        ticks.append(name); i+=1
    ax.set_xticks(range(len(ticks))); ax.set_xticklabels(ticks); ax.set_ylim(0,115)
    ax.set_ylabel("runs where late ambulance was re-ordered (%)")
    ax.set_title("Ablation 5 — rollback safely admits a late ambulance\n(without it, the committed order can't be undone)", fontsize=10)
    ax.grid(alpha=.3, axis="y")
    fig.tight_layout(); fig.savefig(os.path.join(FIGS,"ab5_rollback.png"), dpi=130)
    return arm


def cert_latency(path):
    v = [float(m) for m in re.findall(r"Cert_Creation_Latency:\s+([\d.]+)", open(path, errors='ignore').read())]
    return statistics.mean(v) if v else None

def pbft_latency(path):
    m = re.search(r"PROPOSE_ALL_BFT\(sim\)=([\d.]+)s", open(path, errors='ignore').read())
    return float(m.group(1)) if m else None


def ablation6(plt):
    # vanilla BFT (--no-firewall) vs ours (firewall) — normal operation, msg + latency.
    # In normal ops the firewall passes everything, so it is verification-only: expect
    # ~identical msgs/latency (the addition is FREE here; its payoff is Ablation 2).
    def arm_stats(arm):
        fs = glob.glob(os.path.join(RES, f"ab6_{arm}_rep*.log"))
        if not fs: return None
        msgs = statistics.mean(total_msgs(p) for p in fs)
        lat = [pbft_latency(p) for p in fs if pbft_latency(p) is not None]
        return msgs, (statistics.mean(lat) if lat else 0), len(fs)
    ov, va = arm_stats("ours"), arm_stats("vanilla")
    if not ov or not va: return None
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4.2))
    for ax, idx, lab, unit in ((a1,0,"messages per run",""),(a2,1,"consensus latency (s)","")):
        ax.bar(0, va[idx], .5, color=RED, label="vanilla BFT (no f+1)")
        ax.bar(1, ov[idx], .5, color=GRN, label="our BFT (f+1 firewall)")
        ax.set_xticks([0,1]); ax.set_xticklabels(["vanilla","ours"]); ax.set_ylabel(lab)
        ax.grid(alpha=.3, axis="y")
    a1.set_title("messages", fontsize=11); a2.set_title("consensus latency", fontsize=11)
    a1.legend(fontsize=8)
    fig.suptitle("Vanilla BFT vs our BFT (normal operation): the f+1 firewall is ~free\n"
                 "identical msgs & latency — its cost is only paid rejecting attacks (Ablation 2)", fontsize=11)
    fig.tight_layout(); fig.savefig(os.path.join(FIGS,"ab6_vanilla_vs_ours.png"), dpi=130)
    return {"vanilla":(va[0],va[1]), "ours":(ov[0],ov[1])}


def decomposition(plt):
    # where our end-to-end consensus time goes: arrival-cert formation (ours) + PBFT
    # ordering (vanilla core). Decomposition for understanding (from ab6 'ours' logs).
    fs = glob.glob(os.path.join(RES, "ab6_ours_rep*.log")) or glob.glob(os.path.join(RES, "ab1_ON_k0_rep*.log"))
    if not fs: return None
    cert = statistics.mean([c for c in (cert_latency(p) for p in fs) if c])
    pbft = statistics.mean([c for c in (pbft_latency(p) for p in fs) if c])
    fig, ax = plt.subplots(figsize=(5.5, 5))
    ax.bar(0, pbft*1000, .55, color="#5b8fd1", label="PBFT ordering (vanilla core)")
    ax.bar(0, cert*1000, .55, bottom=pbft*1000, color=GRN, label="arrival-cert f+1 (our addition)")
    ax.text(0, pbft*1000/2, f"{pbft*1000:.0f} ms", ha="center", va="center", color="white", fontsize=10)
    ax.text(0, pbft*1000+cert*1000/2, f"+{cert*1000:.0f} ms", ha="center", va="center", color="white", fontsize=10)
    ax.set_xticks([0]); ax.set_xticklabels(["our protocol"]); ax.set_xlim(-.8,.8)
    ax.set_ylabel("consensus latency (ms)")
    ax.set_title("Latency decomposition: what our arrival-cert layer\nadds on top of vanilla PBFT ordering", fontsize=10)
    ax.legend(fontsize=8); ax.grid(alpha=.3, axis="y")
    fig.tight_layout(); fig.savefig(os.path.join(FIGS,"decomposition.png"), dpi=130)
    return {"pbft_ms": pbft*1000, "cert_ms": cert*1000}


def main():
    plt = plt_setup()
    out = {}
    for name, fn in (("1",ablation1),("2",ablation2),("3",ablation3),("4",ablation4),
                     ("5",ablation5),("6-vanilla",ablation6),("decomposition",decomposition)):
        try:
            r = fn(plt)
            out[name] = r
            print(f"{name}: {'OK -> figure written' if r else 'no logs'}  {r if r else ''}")
        except Exception as e:
            print(f"{name}: ERROR {e}")
    print(f"\nFigures in {FIGS}")


if __name__ == "__main__":
    main()
