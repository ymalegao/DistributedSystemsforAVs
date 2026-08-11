#!/usr/bin/env python3

import unittest

from bft_quorum import intersection_holds, q


class BftQuorumTest(unittest.TestCase):
    def test_known_quorums(self):
        self.assertEqual(q(16, 5), 11)
        self.assertEqual(q(18, 5), 12)
        self.assertEqual(q(20, 6), 14)
        self.assertEqual(q(20, 1), 11)

    def test_intersection_invariant_for_valid_pairs(self):
        for n_active in range(1, 65):
            for f in range((n_active - 1) // 3 + 1):
                quorum = q(n_active, f)
                self.assertGreaterEqual(2 * quorum - n_active, f + 1)
                self.assertTrue(intersection_holds(n_active, f))

    def test_invalid_f_rejected(self):
        with self.assertRaises(ValueError):
            q(18, 6)
        with self.assertRaises(ValueError):
            q(0, 0)
        with self.assertRaises(ValueError):
            q(20, -1)


if __name__ == "__main__":
    unittest.main()
