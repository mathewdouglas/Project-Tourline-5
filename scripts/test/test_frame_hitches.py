import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('frame_hitches', Path(__file__).parents[1]/'diag/frame_hitches.py')
h = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(h)


def row(t, present=1):
    result = dict(unix_ms=100000+t, qpc_ms=t, batch_ms=5, draws=10,
                  present=present, foreground=1, present_ms=1,
                  gpu_previous_ms=2, pending=0)
    result.update({k:t for k in (*h.STAGES, *h.COUNTERS)})
    return result


class TraceTests(unittest.TestCase):
    def test_offscreen_batches_join_present_interval_and_shot_overlap(self):
        rows = [row(10), row(25, 0), row(110), row(130)]
        rows[1]['foreground'] = 0
        report = h.summarize(rows, 100000, [dict(file='intro.png', start_ms=100105, end_ms=100120)])
        event = report['hitches'][0]
        self.assertEqual(report['presented_intervals'], 2)
        self.assertEqual(event['interval_ms'], 100)
        self.assertEqual(event['deltas']['driver_us'], 100)
        self.assertEqual(event['present_ms'], 2)
        self.assertFalse(event['foreground'])
        self.assertEqual(event['shot_overlaps'], ['intro.png'])

    def test_malformed_reset_and_nonfinite_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'trace.csv'
            for field, value in [('driver_calls', 0), ('qpc_ms', 5), ('batch_ms', float('nan'))]:
                rows = [row(10), row(20)]
                rows[1][field] = value
                with path.open('w', newline='') as f:
                    writer = csv.DictWriter(f, fieldnames=rows[0])
                    writer.writeheader(); writer.writerows(rows)
                with self.assertRaises(ValueError):
                    h.read_trace(path)
            path.write_text('unix_ms,qpc_ms\n1\n')
            with self.assertRaises(ValueError):
                h.read_trace(path)

    def test_killed_writer_tail_is_explicit_and_bounded(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'trace.csv'
            rows = [row(10), row(20)]
            with path.open('w', newline='') as f:
                writer = csv.DictWriter(f, fieldnames=rows[0])
                writer.writeheader(); writer.writerows(rows)
            with path.open('ab') as f:
                f.write(b'100040,40,')
            with self.assertRaises(ValueError):
                h.read_trace(path)
            prefix = h.read_trace(path, allow_incomplete_tail=True)
            self.assertEqual(len(prefix), 2)
            self.assertEqual(prefix.discarded_tail_bytes, 10)
            self.assertEqual(h.summarize(prefix)['discarded_tail_bytes'], 10)


if __name__ == '__main__':
    unittest.main()
