#!/usr/bin/env python3
"""Tests for the checked-in Phase 2 actual-movement conflict table."""

from pathlib import Path
import unittest

from generate_phase2_movement_conflicts import MOVEMENTS, build_table


HERE = Path(__file__).resolve().parent


class Phase2MovementConflictTableTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        _, cls.table, cls.rows = build_table(
            HERE / "bft_intersection.net.xml",
            HERE / "phase2_mixed_route_manifest.csv",
        )
        cls.index = {f"{approach}-{direction}": i for i, (approach, direction) in enumerate(MOVEMENTS)}

    def conflict(self, first: str, second: str) -> bool:
        return self.table[self.index[first]][self.index[second]]

    def test_complete_symmetric_table_with_conflicting_diagonal(self) -> None:
        self.assertEqual(len(self.rows), 144)
        self.assertEqual(self.table, [list(row) for row in zip(*self.table)])
        self.assertTrue(all(self.table[i][i] for i in range(12)))

    def test_calibration_pairs(self) -> None:
        self.assertTrue(self.conflict("N-S", "E-S"))
        self.assertFalse(self.conflict("N-S", "S-S"))

    def test_expected_cell_counts(self) -> None:
        conflict_cells = sum(sum(row) for row in self.table)
        self.assertEqual(conflict_cells, 92)
        self.assertEqual(144 - conflict_cells, 52)


if __name__ == "__main__":
    unittest.main()
