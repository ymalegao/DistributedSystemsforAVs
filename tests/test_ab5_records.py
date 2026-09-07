import tempfile
import unittest
from pathlib import Path
from plotter.io.logparse import parse_log

class RollbackRecordsTest(unittest.TestCase):
    def test_priority_arrival_survives_epoch_change_and_incomplete_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ab5_rollback_on_n18_rep1.log"
            path.write_text("[PRIORITY-SPAWN] vehicle=veh17 t=20 route=rE position=200\n"
                "[CAR-METRICS] veh17 role=ambulance epoch=1 stop_time=25 depart_time=33 wait_stop_to_departure_sec=8\n"
                "[RUN-COMPLETION] expected=18 cleared=1 t=90 ids=veh17,\n"
                "[VEHICLE-INCOMPLETE] vehicle=veh15 t=90\n")
            record = parse_log(path)
            self.assertEqual(record.depart_at[17] - record.priority_spawn_at[17], 13)
            self.assertEqual(record.expected_vehicles, 18)
            self.assertEqual(record.incomplete_vehicle_ids, [15])
            self.assertEqual(record.confirmed_clearance_ids, [17])
            self.assertNotIn(15, record.depart_at)

if __name__ == "__main__":
    unittest.main()
