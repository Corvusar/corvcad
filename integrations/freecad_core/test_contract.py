"""Regression checks runnable without either CAD runtime."""
import copy
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from checks import manifold
from inputs import validate
from run import run


def fixture():
    return {"schema": 1, "units": "mm", "bounds": [0, 0, 50, 30], "board_thickness": 1.6,
            "mounting_holes": [{"reference": "H1", "x": 10, "y": 10, "diameter": 3.2}]}


class ContractTests(unittest.TestCase):
    def test_dimensions_reject_nan_infinity_and_booleans(self):
        for bad in (math.nan, math.inf, -math.inf, True, "2"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                validate(fixture(), {"wall": bad})

    def test_invalid_parameters(self):
        for settings in ({"floor": 0}, {"wall": -1}, {"standoff_height": 0},
                         {"lid_gap": 100}, {"unknown": 1}, {"standoff_diameter": 3.2}):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                validate(fixture(), settings)

    def test_zero_clearances_allowed(self):
        validate(fixture(), {"margin": 0, "lid_gap": 0, "board_clearance": 0})

    def test_invalid_board(self):
        for changes in ({"schema": 2}, {"units": "in"}, {"board_thickness": 0},
                        {"bounds": [0, 0, 0, 30]}, {"bounds": [0, 0, math.nan, 30]}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate({**fixture(), **changes})

    def test_holes_cannot_escape_cavity_or_overlap(self):
        for changes in ({"x": -1}, {"x": 0}, {"diameter": -1}, {"diameter": math.inf}):
            data = fixture()
            data["mounting_holes"][0].update(changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate(data)
        data = fixture()
        data["mounting_holes"].append(copy.deepcopy(data["mounting_holes"][0]))
        with self.assertRaises(ValueError):
            validate(data)

    def test_sketch_arcs_and_box_fallback(self):
        import inputs
        bowl = inputs._shape({"kind": "polygon", "points": [[0, 0, 1], [2, 0], [2, 2], [0, 2]]})  # half circle below
        self.assertAlmostEqual(inputs.area(bowl), 4 + math.pi / 2)
        self.assertEqual(inputs.extent(bowl), (0.0, -1.0, 2.0, 2.0))
        with self.assertRaises(ValueError):  # bent back across its own side
            inputs._shape({"kind": "polygon", "points": [[0, 0], [4, 0], [4, 1, -1], [0, 1]]})
        data, settings = fixture(), validate(fixture())
        x0, y0, x1, y1 = data["bounds"]
        ring = {"face": "lid", "operation": "cut", "depth": None,
                "shapes": [{"kind": "rect", "x": x0 + 1, "y": y0 + 1, "width": 10, "height": 10},
                           {"kind": "circle", "x": x0 + 6, "y": y0 + 6, "diameter": 2}]}
        with self.assertRaisesRegex(ValueError, "overlap"):  # boxes without an exact test
            inputs.validate_openings(data, settings, [], [], None, [ring])
        inputs.validate_openings(data, settings, [], [], None, [ring], separate=lambda a, b: True)

    def test_no_holes_and_mixed_diameters(self):
        validate({**fixture(), "mounting_holes": []})
        data = fixture()
        data["mounting_holes"].append({"reference": "H2", "x": 40, "y": 20, "diameter": 2.5})
        validate(data)


class MeshTests(unittest.TestCase):
    points = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    faces = [(0, 2, 1), (0, 1, 3), (1, 2, 3), (2, 0, 3)]

    def test_closed_tetrahedron(self):
        self.assertAlmostEqual(manifold(self.points, self.faces)["signed_volume_mm3"], 1/6)

    def test_missing_face(self):
        with self.assertRaisesRegex(AssertionError, "open"):
            manifold(self.points, self.faces[:-1])

    def test_duplicate_face(self):
        with self.assertRaises(AssertionError):
            manifold(self.points, self.faces + [self.faces[0]])

    def test_reversed_face(self):
        with self.assertRaisesRegex(AssertionError, "winding"):
            manifold(self.points, [tuple(reversed(self.faces[0]))] + self.faces[1:])

    def test_inverted_volume(self):
        with self.assertRaisesRegex(AssertionError, "volume"):
            manifold(self.points, [tuple(reversed(f)) for f in self.faces])

    def test_degenerate_triangle(self):
        with self.assertRaisesRegex(AssertionError, "Degenerate"):
            manifold(self.points, [(0, 0, 1)])

    def test_non_finite_vertex(self):
        with self.assertRaisesRegex(AssertionError, "Non-finite"):
            manifold([(math.nan, 0, 0)] + self.points[1:], self.faces)


class RunnerTests(unittest.TestCase):
    def test_existing_directory_cannot_reuse_stale_success(self):
        with tempfile.TemporaryDirectory() as temp, self.assertRaises(FileExistsError):
            run(Path("unused"), Path("unused"), Path(temp))

    def test_zero_exit_without_report_is_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            with patch("run.subprocess.run") as command:
                command.return_value.returncode = 0
                command.return_value.stdout = b"Exception while processing file"
                with self.assertRaisesRegex(RuntimeError, "proof failed"):
                    run(Path("unused"), Path("unused"), Path(temp) / "fresh")


if __name__ == "__main__":
    unittest.main()
