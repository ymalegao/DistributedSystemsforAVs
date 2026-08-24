import math
import unittest

from fourway import generate_perception_matrices as matrices


class PerceptionMatrixTest(unittest.TestCase):
    def test_geometry_width_is_stable(self):
        self.assertAlmostEqual(matrices.lane_width_m(), 3.2, places=9)

    def test_rows_are_normalized(self):
        for sigma in matrices.SIGMAS:
            values = matrices.matrix(sigma)
            for row in range(4):
                self.assertTrue(math.isclose(sum(values[row * 4:(row + 1) * 4]), 1.0))

    def test_generation_is_deterministic(self):
        for sigma in matrices.SIGMAS:
            self.assertEqual(matrices.matrix(sigma), matrices.matrix(sigma))

    def test_checked_in_catalog_matches_generator(self):
        catalog = matrices.read_catalog()
        self.assertEqual(set(catalog), set(matrices.SIGMAS))
        for sigma in matrices.SIGMAS:
            for generated, checked_in in zip(matrices.matrix(sigma), catalog[sigma]):
                self.assertTrue(math.isclose(generated, checked_in,
                                             rel_tol=1e-11, abs_tol=1e-14))

    def test_zero_sigma_is_identity(self):
        values = matrices.matrix(0.0)
        self.assertEqual(values, [1.0 if row == col else 0.0
                                  for row in range(4) for col in range(4)])


if __name__ == "__main__":
    unittest.main()
