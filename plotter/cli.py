"""Command line for building figures.

    python -m plotter list
    python -m plotter build ab1_rsu
    python -m plotter build-all
"""
import argparse
from pathlib import Path

from . import style
from .io import discover
from .figures import _registry

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_RESULTS = REPO_ROOT / "benchmarks" / "ablations" / "results"
DEFAULT_OUT = REPO_ROOT / "figures"


def _build_one(module, runs, out_dir) -> bool:
    data = module.load(runs) if getattr(module, "NEEDS_RUNS", True) else module.load()
    if data is None or len(data) == 0:
        print(f"  {module.NAME:<24} SKIP  no run logs for this ablation")
        return False

    fig, axes = style.figure(getattr(module, "FIGSIZE", style.FIGSIZE),
                             getattr(module, "SUBPLOTS", (1, 1)))
    module.build(data, axes)
    path = style.save(fig, module.NAME, out_dir)
    print(f"  {module.NAME:<24} OK    {path.name}")
    return True


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="plotter", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="list available figures")
    for name, help_text in (("build", "build one figure"),
                            ("build-all", "build every figure that has data")):
        p = sub.add_parser(name, help=help_text)
        if name == "build":
            p.add_argument("name")
        p.add_argument("--results", default=DEFAULT_RESULTS,
                       help="ablation results directory")
        p.add_argument("--out", default=DEFAULT_OUT)

    args = parser.parse_args(argv)

    if args.command == "list":
        for module in _registry.FIGURE_MODULES:
            source = (f"ablation {module.STUDY}" if hasattr(module, "STUDY")
                      else "derived from the protocol")
            print(f"  {module.NAME:<24} {module.TITLE}")
            print(f"  {'':<24} ({source})")
        return 0

    # Parsed once and shared: every ablation figure reads the same results tree,
    # and the logs are large enough that re-globbing per figure is wasteful.
    runs = discover.load_runs(args.results)
    modules = ([_registry.get(args.name)] if args.command == "build"
               else _registry.FIGURE_MODULES)

    if not runs and any(getattr(m, "NEEDS_RUNS", True) for m in modules):
        print(f"No ablation run logs in {args.results}")
        print("Run benchmarks/ablations/run_ablations.sh first.")

    print(f"Building into {args.out}")
    built = sum(_build_one(m, runs, args.out) for m in modules)
    print(f"{built}/{len(modules)} built")
    return 0 if built else 1
