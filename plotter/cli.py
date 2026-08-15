"""Command line for building figures.

    python -m plotter list
    python -m plotter build 18veh_msg_cost
    python -m plotter build-all
"""
import argparse
from pathlib import Path

from . import style
from .figures import _registry

REPO_ROOT = Path(__file__).resolve().parent.parent
# Result trees still live beside the runners that produce them.
DEFAULT_RESULTS_ROOT = REPO_ROOT / "benchmarks" / "rsu_ablation"
DEFAULT_OUT = REPO_ROOT / "figures"


def _results_dir(module, override) -> Path:
    return Path(override) if override else DEFAULT_RESULTS_ROOT / module.RESULTS_DIRNAME


def _build_one(module, results_override, out_dir) -> bool:
    results_dir = _results_dir(module, results_override)
    data = module.load(results_dir)
    if not len(data):
        print(f"  {module.NAME:<28} SKIP  no run logs in {results_dir}")
        return False
    fig, ax = style.figure(getattr(module, "FIGSIZE", style.FIGSIZE))
    module.build(data, ax)
    path = style.save(fig, module.NAME, out_dir)
    print(f"  {module.NAME:<28} OK    {path}")
    return True


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="plotter", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="list available figures")

    p_build = sub.add_parser("build", help="build one figure")
    p_build.add_argument("name")
    p_build.add_argument("--results", help="override the results directory")
    p_build.add_argument("--out", default=DEFAULT_OUT)

    p_all = sub.add_parser("build-all", help="build every figure that has data")
    p_all.add_argument("--out", default=DEFAULT_OUT)

    args = parser.parse_args(argv)

    if args.command == "list":
        for module in _registry.FIGURE_MODULES:
            print(f"  {module.NAME:<28} {module.TITLE}")
            print(f"  {'':<28} reads {module.RESULTS_DIRNAME}/")
        return 0

    if args.command == "build":
        return 0 if _build_one(_registry.get(args.name), args.results, args.out) else 1

    print(f"Building {len(_registry.FIGURE_MODULES)} figures into {args.out}")
    built = sum(_build_one(m, None, args.out) for m in _registry.FIGURE_MODULES)
    print(f"{built}/{len(_registry.FIGURE_MODULES)} built")
    return 0
