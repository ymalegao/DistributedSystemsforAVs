"""Matched departure timeline and run-level latency–throughput tradeoff."""
from statistics import mean
from ..io import discover
from .. import style
from . import _lanes, _paper

NAME = 'ab5_rollback'
TITLE = 'Emergency Rollback Effectiveness'
STUDY = 5
SUBPLOTS = (1,2)
FIGSIZE = (9.0,3.7)
ARMS = (('rollback_off','control','Rollback off'),('rollback_on','treatment','Rollback on'))


def load(runs,lane):
    grouped={}
    for a,_,_ in ARMS:
        grouped[a] = {n: discover.cell(runs,5,_lanes.arm(a,lane),n=n)
                      for n in discover.ns(runs,_lanes.arm(a,lane),5)}
    counts=sorted(set(grouped['rollback_on']) & set(grouped['rollback_off']))
    if not counts: return {}
    cells={a:grouped[a][counts[0]] for a,_,_ in ARMS}
    reps=set.intersection(*[{r.key.rep for r in rs} for rs in cells.values()])
    if not reps: return {}
    return dict(cells=cells, grouped=grouped, counts=counts, rep=min(reps))


def build(data,axes,lane):
    left,right=axes
    left.figure.suptitle(TITLE)
    selected={a:next(r for r in rs if r.key.rep==data['rep']) for a,rs in data['cells'].items()}
    # Report absence from departure logs, not an invented departure at the horizon.
    known=set().union(*(set(r.stop_at)|set(r.depart_at)|set(r.role) for r in selected.values()))
    ambulance=set().union(*(set(r.ambulance_ids()) for r in selected.values()))
    max_t=max((t for r in selected.values() for t in r.depart_at.values()),default=1)
    for y,(a,role,label) in enumerate(ARMS):
        rec=selected[a]; batches={}
        for v,t in rec.depart_at.items(): batches.setdefault(t,[]).append(v)
        for t,vs in batches.items():
            for j,v in enumerate(sorted(vs)):
                off=(j-(len(vs)-1)/2)*.10
                amb=v in ambulance
                left.scatter(t,y+off,s=100 if amb else 22,marker='*' if amb else 'o',
                             color=style.ACCENT if amb else style.series(role)['color'],zorder=3)
                # Star + legend already identify the ambulance; no per-point label.
        missing=sorted(known-set(rec.depart_at))
        text=('' if not missing else 'No recorded clearance: '+', '.join(
            ('Ambulance' if v in ambulance else f'veh{v}') for v in missing))
        if text:
            left.text(.02,y+.27,text,transform=left.get_yaxis_transform(),fontsize=7)

    left.set_yticks([0,1],[f'{label} ({len(selected[a].depart_at)} cleared)' for a,_,label in ARMS])
    left.set_ylim(1.55,-.5); left.margins(x=.10)
    style.finish(left,title='(a) Departures',
                 xlabel='Simulation time (s)',legend=False)
    left.scatter([],[],marker='*',s=80,color=style.ACCENT,label='Ambulance')
    left.legend(frameon=False,fontsize=8,loc='upper left')
    incomplete = 0
    for a,role,label in ARMS:
        xs,ys=[],[]
        for n in data['counts']:
            valid=[]
            for rec in data['grouped'][a][n]:
                expected=set(range(rec.expected_vehicles or 0))
                # Neither the app's CAR-METRICS (depart_at) nor the scenario
                # manager's RUN-COMPLETION (confirmed_clearance_ids) alone
                # covers every vehicle: the manager has a late-spawn tracking
                # gap that occasionally omits the ambulance, and the app
                # occasionally never writes CAR-METRICS for a late-spawned
                # non-emergency vehicle. If BOTH sources still miss a
                # vehicle, it genuinely didn't cross; treat the union as
                # authoritative for completeness.
                crossed = set(rec.depart_at) | set(rec.confirmed_clearance_ids)
                complete=not rec.teleported_vehicle_ids and expected and crossed==expected
                if not complete or not rec.priority_spawn_at:
                    incomplete += 1
                    continue
                response=mean(rec.depart_at[v]-t for v,t in rec.priority_spawn_at.items())
                valid.append((response,rec.throughput))
                right.scatter(response,rec.throughput,color=style.series(role)['color'],alpha=.3,s=18)
            if valid:
                x,y=mean(v[0] for v in valid),mean(v[1] for v in valid)
                xs.append(x);ys.append(y)
        right.plot(xs,ys,marker='o' if a=='rollback_off' else 's',color=style.series(role)['color'],label=label)
    style.finish(right,title='(b) Emergency response and throughput',
                 xlabel='Mean ambulance spawn-to-clear time (s)',
                 ylabel='Throughput (vehicles/s)')
    if incomplete:
        right.text(.02,.02,f'{incomplete} incomplete/unverified runs; see report',transform=right.transAxes,fontsize=7)
