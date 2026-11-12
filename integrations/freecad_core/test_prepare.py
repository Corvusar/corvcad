"""Run with KiCad 10's Python: board extraction must be read-only and fail closed."""
import hashlib
from pathlib import Path
import tempfile
import unittest

import pcbnew
from prepare import extract

ROOT = Path(__file__).resolve().parents[2]


def loop(board, points):
    for a, b in zip(points, points[1:] + points[:1]):
        edge = pcbnew.PCB_SHAPE()
        edge.SetShape(pcbnew.SHAPE_T_SEGMENT)
        edge.SetLayer(pcbnew.Edge_Cuts)
        edge.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(a[0]), pcbnew.FromMM(a[1])))
        edge.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(b[0]), pcbnew.FromMM(b[1])))
        board.Add(edge)


class PrepareTests(unittest.TestCase):
    def test_real_fixtures_and_no_source_changes(self):
        for name, bounds, holes in (("demo.kicad_pcb", [10, 20, 60, 50], 1),
                                   ("roundtrip/RoundTrip.kicad_pcb", [0, 0, 80, 60], 2)):
            path = ROOT / "engine/tests/fixtures" / name
            before = path.read_bytes()
            data = extract(path)
            self.assertEqual(data["bounds"], bounds)  # excludes Edge.Cuts stroke width
            self.assertEqual(len(data["mounting_holes"]), holes)  # TP1 drill is not a mount
            self.assertEqual(data["source"]["sha256"], hashlib.sha256(before).hexdigest())
            self.assertEqual(path.read_bytes(), before)

    def assert_rejected(self, board):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "unsupported.kicad_pcb"
            pcbnew.SaveBoard(str(path), board)
            with self.assertRaises(ValueError):
                extract(path)

    def test_no_outline(self):
        self.assert_rejected(pcbnew.BOARD())

    def test_non_rectangle(self):
        board = pcbnew.BOARD()
        loop(board, [(0, 0), (20, 0), (10, 10)])
        self.assert_rejected(board)

    def test_slot(self):
        board = pcbnew.BOARD()
        loop(board, [(0, 0), (20, 0), (20, 20), (0, 20)])
        loop(board, [(5, 5), (10, 5), (10, 10), (5, 10)])
        self.assert_rejected(board)

    def test_disconnected_boards(self):
        board = pcbnew.BOARD()
        loop(board, [(0, 0), (20, 0), (20, 20), (0, 20)])
        loop(board, [(30, 0), (40, 0), (40, 10), (30, 10)])
        self.assert_rejected(board)


if __name__ == "__main__":
    unittest.main()
