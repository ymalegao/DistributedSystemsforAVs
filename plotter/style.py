"""The single visual theme for every generated figure.

Before this module each analyzer set its own DPI (5 different values across the
scripts), its own figure size (~15 variants) and its own colors, so no two
figures in the thesis matched. Everything visual is decided here.

Palette
-------
Arms were previously red #d1495b (without RSU) vs green #2e8b57 (with RSU).
That pair fails colorblind separation: OKLab deuteranopia dE 4.4, against a
target of >= 8. Red-green is the single most common CVD axis, so for roughly
8% of male readers the two arms of every ablation figure -- the entire claim --
were indistinguishable.

Blue/orange below scores dE 24.7 (protanopia), 33.6 normal vision, and passes
lightness, chroma and contrast. Markers differ per arm as well, so identity is
never carried by color alone.

These figures target print and PDF, so the theme deliberately commits to a
single light mode rather than adapting to a viewer theme.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")           # no display in batch/CI runs
import matplotlib.pyplot as plt  # noqa: E402

# ── surface & ink ────────────────────────────────────────────────────────────
SURFACE = "#fcfcfb"
INK_PRIMARY = "#1a1a19"
INK_SECONDARY = "#55554e"
GRID_ALPHA = 0.3

# ── categorical series ───────────────────────────────────────────────────────
# Fixed assignment by identity: an arm keeps its color no matter which arms a
# given figure happens to show. Never cycled, never reassigned by rank.
SERIES = {
    "OFF": dict(color="#2a78d6", marker="o", label="without RSU"),
    "ON":  dict(color="#eb6834", marker="s", label="with RSU"),
}
# Third slot, for figures that need one beyond the two arms.
SERIES_3 = dict(color="#1baf7a", marker="^")

DPI = 130
FIGSIZE = (7.2, 4.4)

# Mark specs: 2px lines, >=8px markers.
LINEWIDTH = 2.0
MARKERSIZE = 8.0
BAR_EDGE = SURFACE      # 2px surface gap between adjacent bars
BAR_EDGEWIDTH = 2.0


def series(arm: str, *, label_suffix: str = "") -> dict:
    """Plot kwargs for an arm. label_suffix carries the config, e.g. "(N=18)"."""
    spec = dict(SERIES[arm])
    if label_suffix:
        spec["label"] = f"{spec['label']} {label_suffix}"
    return spec


def figure(figsize=FIGSIZE):
    """A themed figure/axes pair. Every figure module starts here."""
    fig, ax = plt.subplots(figsize=figsize)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)
    ax.grid(alpha=GRID_ALPHA, linewidth=0.8)
    ax.set_axisbelow(True)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    for spine in ("left", "bottom"):
        ax.spines[spine].set_color(INK_SECONDARY)
    ax.tick_params(colors=INK_SECONDARY)
    # Text wears ink tokens, never a series color.
    ax.xaxis.label.set_color(INK_PRIMARY)
    ax.yaxis.label.set_color(INK_PRIMARY)
    return fig, ax


def finish(ax, *, title=None, xlabel=None, ylabel=None, legend=True,
           legend_above=False):
    """Apply the shared title/label/legend conventions.

    legend_above lifts the legend clear of the axes, for figures whose marks
    fill the plot area and leave no uncluttered corner inside it.
    """
    if title:
        # Extra pad when the legend sits above the axes, so the two do not stack.
        ax.set_title(title, fontsize=11, color=INK_PRIMARY,
                     pad=30 if legend_above else 10)
    if xlabel:
        ax.set_xlabel(xlabel)
    if ylabel:
        ax.set_ylabel(ylabel)
    # A legend is always present for >= 2 series, so identity is never
    # color-alone. Suppressed only when the caller draws a single series.
    if legend and ax.get_legend_handles_labels()[0]:
        kwargs = dict(fontsize=9, frameon=False, labelcolor=INK_PRIMARY)
        if legend_above:
            kwargs.update(loc="lower left", bbox_to_anchor=(0, 1.02), ncol=2)
        ax.legend(**kwargs)


def save(fig, name: str, out_dir) -> Path:
    """Write <name>.png and <name>.pdf at the shared DPI.

    Both formats every time: PNG for review, PDF for LaTeX inclusion, so the
    thesis never picks up a stale raster of a regenerated figure.
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    png = out_dir / f"{name}.png"
    fig.savefig(png, dpi=DPI, facecolor=fig.get_facecolor())
    fig.savefig(out_dir / f"{name}.pdf", facecolor=fig.get_facecolor())
    plt.close(fig)
    return png
