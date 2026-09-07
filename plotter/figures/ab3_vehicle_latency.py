"""Every measured vehicle latency; stars identify the same designated vehicle."""
import random
from . import ab3_baseline, _lanes
from .. import style

NAME = 'ab3_vehicle_latency'
TITLE = 'Priority Scheduling Effectiveness'
STUDY = 3
SUBPLOTS = (1, 1)
FIGSIZE = (6.5, 3.7)
load = ab3_baseline.load


def build(data, ax, lane):
    ns = list(data['tl'])
    rng = random.Random(0)
    missing = 0
    designated = {(n, rec.key.rep): set(rec.ambulance_ids())
                  for n, recs in data['prio'].items() for rec in recs}
    for j, (_, arm, role, label) in enumerate(ab3_baseline.ARMS):
        spec = style.series(role)
        xs, ys, stars = [], [], []
        for i, n in enumerate(ns):
            for rec in data[arm][n]:
                waits = rec.clearance_waits()
                missing += len(set(range(n)) - set(waits))
                for v, wait in sorted(waits.items()):
                    x = i + (j-1)*.24 + rng.uniform(-.055,.055)
                    if v in designated[n, rec.key.rep]:
                        stars.append((x,wait))
                    else:
                        xs.append(x); ys.append(wait)
        ax.scatter(xs, ys, s=16, alpha=.55, label=label, **spec, linewidths=0)
        if stars:
            x,y=zip(*stars)
            ax.scatter(x,y,s=72,marker='*',color=spec['color'],edgecolors=style.INK_PRIMARY,
                       linewidths=.35,zorder=4)
    ax.scatter([],[],s=72,marker='*',color=style.INK_SECONDARY,label='Priority vehicle (same vehicle in all methods)')
    ax.set_xticks(range(len(ns)), ns)
    highest = max((w for cells in data.values() for recs in cells.values()
                   for rec in recs for w in rec.clearance_waits().values()), default=1)
    ax.set_ylim(0, highest * 1.28)
    style.finish(ax,title=TITLE,xlabel='Number of vehicles',
                 ylabel='Time from stopping to clearing (s)',legend=False)
    ax.legend(fontsize=8,frameon=False,ncol=2,loc='upper left')
    if missing:
        ax.text(.99,.02,f'{missing} vehicle records lack complete latency',
                transform=ax.transAxes,ha='right',fontsize=7)
