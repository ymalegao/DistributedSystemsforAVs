"""Shared paper plots; all latency/throughput pairs come from the same run."""
import statistics
from .. import style


def tradeoff(ax, cells, role, label, *, annotate=True, label_offset=None):
    spec = style.series(role)
    means = []
    for n, recs in cells.items():
        pairs = [(r.mean_clearance_wait, r.throughput) for r in recs
                 if r.mean_clearance_wait is not None and r.throughput is not None]
        if not pairs:
            continue
        xs, ys = zip(*pairs)
        ax.scatter(xs, ys, s=19, alpha=.35, **spec, linewidths=0)
        x, y = statistics.mean(xs), statistics.mean(ys)
        means.append((x, y))
        if annotate:
            offset = label_offset or {'control': (5, 9), 'treatment': (5, -13), 'accent': (-17, 8)}[role]
            ax.annotate(str(n), (x, y), xytext=offset, textcoords='offset points',
                        fontsize=8, color=spec['color'])
    if means:
        xs, ys = zip(*means)
        ax.plot(xs, ys, label=label, **spec, markersize=6, linewidth=1.5,
                linestyle='--' if role == 'control' else '-')
    ax.margins(x=.20, y=.22)


def tradeoff_axes(ax, title):
    style.finish(ax, title=title, xlabel='Mean stop-to-clear latency (s)',
                 ylabel='Throughput (vehicles/s)')
