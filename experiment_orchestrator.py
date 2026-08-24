#!/usr/bin/env python3
"""Compatibility entry point for the experiment dispatcher.

The implementation lives in ``tools/experiment_orchestrator.py``; keeping
this tiny wrapper preserves existing commands and recorded experiment notes.
"""
from pathlib import Path
import importlib.util
import sys

_impl_path = Path(__file__).resolve().parent / "tools" / "experiment_orchestrator.py"
_spec = importlib.util.spec_from_file_location("_picabft_experiment_orchestrator", _impl_path)
_impl = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = _impl
assert _spec.loader is not None
_spec.loader.exec_module(_impl)
globals().update({name: value for name, value in vars(_impl).items() if not name.startswith("__")})

if __name__ == "__main__":
    raise SystemExit(_impl.main())
