"""Observed silent-vehicle tolerance and zero-fault operating cost."""
import numpy as np
from ..io import discover
from .. import style
from . import _lanes, _paper

NAME = 'ab1_rsu'
TITLE = 'Scalability'
STUDY = 1
SUBPLOTS = (1, 2)
FIGSIZE = (9.0, 3.7)
ARMS = (('OFF', 'control', 'No RSU'), ('ON', 'treatment', '4 RSUs'))


def all_active_decided(rec):
    # The runner silences vehicle replicas 1..k, leaving the primary active.
    # Require actual decision evidence for EVERY other vehicle, not any commit.
    expected = set(range(rec.key.n)) - set(range(1, (rec.key.k or 0)+1))
    return bool(expected) and expected <= set(rec.order_decided_at)


def load(runs,lane):
    ns=sorted(set(discover.ns(runs,study=1,arm=_lanes.arm('OFF',lane))) |
              set(discover.ns(runs,study=1,arm=_lanes.arm('ON',lane))))
    ns=[n for n in ns if n != 12]
    if not ns: return {}
    ks=sorted({k for s,a,k,n in runs if s==1 and a in
               [_lanes.arm(b,lane) for b,_,_ in ARMS] and k is not None})
    cells={(n,a,k): discover.cell(runs,1,_lanes.arm(a,lane),k,n)
           for n in ns for a,_,_ in ARMS for k in ks}
    return dict(ns=ns,ks=ks,cells=cells)


def build(data,axes,lane):
    left,right=axes
    left.figure.suptitle(TITLE)
    rows=[(n,a) for n in data['ns'] for a,_,_ in ARMS]
    values=np.full((len(rows),len(data['ks'])),np.nan)
    for i,(n,a) in enumerate(rows):
        for j,k in enumerate(data['ks']):
            recs=data['cells'][n,a,k]
            if recs: values[i,j]=100*sum(r.committed for r in recs)/len(recs)
    cmap=style.plt.get_cmap('Blues').copy(); cmap.set_bad('#eeeeee')
    left.imshow(values,aspect='auto',vmin=0,vmax=100,cmap=cmap)
    for (i,j),v in np.ndenumerate(values):
        left.text(j,i,'—' if np.isnan(v) else f'{v:.0f}',ha='center',va='center',
                  fontsize=7,color='white' if v>65 else style.INK_PRIMARY)
    left.grid(False)
    left.set_xticks(range(len(data['ks'])),data['ks'])
    left.set_yticks(range(len(rows)),[f'{n}: '+('no RSU' if a=='OFF' else '+4 RSUs') for n,a in rows])
    for i in range(1,len(data['ns'])): left.axhline(2*i-.5,color='white',lw=2)
    style.finish(left,title='(a) Runs with an ORDER decision (%)',xlabel='Silent vehicles (k)',
                 ylabel='Vehicle count / RSU condition',legend=False)
    for a,role,label in ARMS:
        _paper.tradeoff(right,{n:data['cells'][n,a,0] for n in data['ns']},role,label)
    _paper.tradeoff_axes(right,'(b) Zero-fault performance')
