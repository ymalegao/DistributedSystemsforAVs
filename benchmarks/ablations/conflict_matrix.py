#!/usr/bin/env python3
"""
Define + visualize the intersection CONFLICT MATRIX (the scheduler's "smart contract").

Decoded from kSafe[12][4] in incubator-resilientdb/integration/omnet/
resdb_intersection_scheduler.cc (IsSafeToBatch). Two vehicle movements may cross the
intersection SIMULTANEOUSLY (same committed batch) iff this matrix marks them safe.

Encoding (from the code): lane 0=N,1=S,2=E,3=W ; direction 0=straight,1=left,2=right.
Rule: same lane never batches; left turns (dir=1) never batch; otherwise the pair must
appear in kSafe (opposing straights, any two rights, or a right + the opposing straight).

Writes figures/conflict_matrix.png. Usage: python3 conflict_matrix.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FIGS = os.path.join(HERE, "figures"); os.makedirs(FIGS, exist_ok=True)

# the exact kSafe[12][4] table from the scheduler
K_SAFE = [(0,0,1,0),(2,0,3,0),(0,2,1,2),(0,2,2,2),(0,2,3,2),(1,2,2,2),
          (1,2,3,2),(2,2,3,2),(0,2,1,0),(1,2,0,0),(2,2,3,0),(3,2,2,0)]

def is_safe(la, da, lb, db):
    if la == lb:                       # same approach lane never batches
        return False
    for p in K_SAFE:
        if (la==p[0] and da==p[1] and lb==p[2] and db==p[3]) or \
           (la==p[2] and da==p[3] and lb==p[0] and db==p[1]):
            return True
    return False

LANES = ["N","S","E","W"]           # 0,1,2,3
DIRS  = [("↑","straight"),("↰","left"),("↱","right")]  # 0,1,2
# 12 movements in (lane,dir) order
MOVES = [(l,d) for l in range(4) for d in range(3)]
LABELS = [f"{LANES[l]}{DIRS[d][0]}" for (l,d) in MOVES]


def main():
    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except Exception as e:
        print(f"(no matplotlib: {e})"); return
    n = len(MOVES)
    M = np.zeros((n, n))   # 1=safe, 0=conflict, -1=self
    for i,(la,da) in enumerate(MOVES):
        for j,(lb,db) in enumerate(MOVES):
            M[i,j] = -1 if i==j else (1 if is_safe(la,da,lb,db) else 0)

    from matplotlib.colors import ListedColormap
    cmap = ListedColormap(["#888888", "#d1495b", "#2e8b57"])  # -1 gray, 0 red, 1 green
    fig, ax = plt.subplots(figsize=(8.2, 7.2))
    ax.imshow(M, cmap=cmap, vmin=-1, vmax=1)
    ax.set_xticks(range(n)); ax.set_xticklabels(LABELS, rotation=90, fontsize=9)
    ax.set_yticks(range(n)); ax.set_yticklabels(LABELS, fontsize=9)
    for i in range(n):
        for j in range(n):
            txt = "–" if M[i,j]==-1 else ("✓" if M[i,j]==1 else "✗")
            ax.text(j, i, txt, ha="center", va="center", fontsize=8,
                    color="white" if M[i,j]!=1 else "white")
    ax.set_title("Intersection conflict matrix — which movements may cross together\n"
                 "✓ = safe to batch (parallel)   ✗ = conflict (must serialize)   – = same movement\n"
                 "lane N/S/E/W · ↑ straight ↰ left ↱ right", fontsize=10)
    # grid
    ax.set_xticks([x-.5 for x in range(1,n)], minor=True)
    ax.set_yticks([y-.5 for y in range(1,n)], minor=True)
    ax.grid(which="minor", color="w", lw=1.2)
    fig.tight_layout()
    fig.savefig(os.path.join(FIGS, "conflict_matrix.png"), dpi=140)

    # print the safe pairs (for the md doc + sanity)
    seen=set(); pairs=[]
    for i,(la,da) in enumerate(MOVES):
        for j,(lb,db) in enumerate(MOVES):
            if j>i and is_safe(la,da,lb,db):
                pairs.append(f"{LABELS[i]} + {LABELS[j]}")
    print(f"conflict_matrix.png written; {len(pairs)} safe-to-batch movement pairs:")
    for p in pairs: print("  ", p)


if __name__ == "__main__":
    main()
