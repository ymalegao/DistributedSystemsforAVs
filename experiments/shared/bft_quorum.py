#!/usr/bin/env python3
"""Canonical BFT quorum sizing for arbitrary active membership."""


def q(n_active: int, f: int) -> int:
    if n_active <= 0:
        raise ValueError("n_active must be positive")
    if f < 0:
        raise ValueError("f must be non-negative")
    max_f = (n_active - 1) // 3
    if f > max_f:
        raise ValueError(f"f={f} invalid for n_active={n_active}; max={max_f}")
    return (n_active + f + 2) // 2


def intersection_holds(n_active: int, f: int) -> bool:
    quorum = q(n_active, f)
    return 2 * quorum - n_active >= f + 1
