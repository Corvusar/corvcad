"""Real-FreeCAD mechanical session regressions; no substitute geometry backend."""
import copy
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import session_runtime as runtime
from inputs import DEFAULTS
from checks import expected, manifold
import FreeCAD as App
import Mesh
import Part

FIXTURES = Path(__file__).resolve().parent.parent / "fixtures"


class MechanicalSessions(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="corvcad-session-")
        self.root = Path(self.temporary.name)
        self.sessions = []

    def tearDown(self):
        for session in self.sessions:
            session.close()
        self.temporary.cleanup()

    def session(self, fixture="demo", name="enclosure", data=None, settings=None):
        payload = data or json.loads((FIXTURES / (fixture + ".json")).read_text())
        session = runtime.Session(str(self.root / (name + ".FCStd")), json.dumps(payload),
                                  DEFAULTS if settings is None else settings)
        self.sessions.append(session)
        return session

    def test_both_real_fixtures_parameters_undo_redo_and_reopen(self):
        changes = dict(margin=4.0, wall=2.6, floor=2.7, board_clearance=3.1,
                       standoff_height=5.1, standoff_diameter=6.8, lid_thickness=2.6, lid_gap=0.5)
        for fixture in ("demo", "roundtrip"):
            with self.subTest(fixture=fixture):
                session = self.session(fixture, fixture)
                baseline = (session._doc.Tray.Shape.Volume, session._doc.Lid.Shape.Volume)
                for name, value in changes.items():
                    before = session.get_settings()
                    session.set_parameter(name, value)
                    self.assertEqual(session.get_settings()[name], value)
                    self.assertTrue(session.can_undo())
                    session.undo()
                    self.assertEqual(session.get_settings(), before)
                    self.assertTrue(session.can_redo())
                    session.redo()
                    self.assertEqual(session.get_settings()[name], value)
                self.assertNotEqual(baseline, (session._doc.Tray.Shape.Volume, session._doc.Lid.Shape.Volume))
                wanted = session.get_settings()
                session.save()
                checksum = runtime._hash(session._path)
                session.close()
                reopened = self.session(fixture, fixture, settings={"invalid_initial_settings_are_ignored": 1})
                self.assertEqual(reopened.get_settings(), wanted)
                self.assertFalse(reopened.can_undo())
                self.assertEqual(runtime._hash(reopened._path), checksum)
                reopened.recompute()

    def test_features_json_lists_the_real_tree_and_refuses_other_methods(self):
        session = self.session("demo")
        reply = json.loads(session.call_json("features", "{}"))
        names = [(f["body"], f["name"], f["type"]) for f in reply["features"]]
        self.assertEqual(names[:3], [("Tray", "OuterPad", "Pad"), ("Tray", "Cavity", "Pocket"),
                                     ("Tray", "BossPad", "Pad")])
        self.assertIn(("Tray", "MountingBore1", "Hole"), names)
        self.assertEqual(names[-1], ("Lid", "LidPad", "Pad"))
        self.assertFalse(any(f["suppressed"] for f in reply["features"]))
        self.assertEqual(reply["features"][0]["profile"], "OuterOutline")
        self.assertIn("Parameters.Floor", reply["features"][0]["expressions"]["Length"])
        for method, arguments in (("save", "{}"), ("_json_features", "{}"), ("features", "[]")):
            with self.subTest(method=method), self.assertRaises(ValueError):
                session.call_json(method, arguments)

    def test_wall_cutouts_edit_undo_save_reopen(self):
        session = self.session("demo")
        x0, y0, x1, y1 = session._data["bounds"]
        height = json.loads(session.call_json("openings", "{}"))["tray_height"]
        cuts = [dict(side="front", center=(x0+x1)/2, width=9.0, bottom=height-4, height=10.0),
                dict(side="left", center=(y0+y1)/2, width=8.0, bottom=2.0, height=4.0)]
        session._doc.addObject("App::FeaturePython", "UserNote")  # not the model's: must survive
        baseline = session._doc.Tray.Shape.Volume
        reply = json.loads(session.call_json("set_openings", json.dumps({"cutouts": cuts})))
        self.assertEqual(reply["cutouts"], cuts)
        features = [f["name"] for f in json.loads(session.call_json("features", "{}"))["features"]]
        self.assertEqual(features[-3:], ["Cutout1", "Cutout2", "LidPad"])
        targets = expected(session._data, session.get_settings(), cuts)
        self.assertAlmostEqual(session._doc.Tray.Shape.Volume, targets["Tray"]["volume"], places=6)
        session.set_parameter("wall", 2.6)  # the cutouts follow the wall
        self.assertAlmostEqual(session._doc.Tray.Shape.Volume,
                               expected(session._data, session.get_settings(), cuts)["Tray"]["volume"], places=6)
        session.undo()
        session.undo()
        self.assertEqual(json.loads(session.call_json("openings", "{}"))["cutouts"], [])
        self.assertAlmostEqual(session._doc.Tray.Shape.Volume, baseline, places=6)
        session.redo()
        self.assertEqual(json.loads(session.call_json("openings", "{}"))["cutouts"], cuts)
        for bad in ([dict(cuts[0], center=x0 - 20)], [dict(cuts[0], side="top")], [dict(cuts[0], bottom=-1.0)]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                session.set_cutouts(bad)
            self.assertEqual(json.loads(session.call_json("openings", "{}"))["cutouts"], cuts)
        self.assertIsNotNone(session._doc.getObject("UserNote"))
        session.save()
        session.close()
        reopened = self.session("demo")
        self.assertEqual(json.loads(reopened.call_json("openings", "{}"))["cutouts"], cuts)
        self.assertIsNotNone(reopened._doc.getObject("UserNote"))
        reopened.set_cutouts([])
        self.assertAlmostEqual(reopened._doc.Tray.Shape.Volume, baseline, places=6)

    def test_cutout_dimensions_beyond_the_cells_precision(self):
        # A connector's pad span is rarely a short decimal: 13.000600000000006 mm (KiCad's
        # kit-dev-coldfire J201) used to come back from its 12-digit cell as 13.0006, and the
        # session refused its own document as changed outside it.
        session = self.session("demo")
        x0, y0, x1, y1 = session._data["bounds"]
        height = json.loads(session.call_json("openings", "{}"))["tray_height"]
        cut = dict(side="front", center=(x0 + x1) / 2 + 0.1 / 3, width=13.000600000000006, bottom=height - 2.5, height=5.0)
        reply = json.loads(session.call_json("set_openings", json.dumps({"cutouts": [cut], "vents": [], "sketches": [], "screws": None})))
        self.assertAlmostEqual(reply["cutouts"][0]["width"], cut["width"], places=9)
        session.set_parameter("wall", 2.6)  # a later change still finds the document as it left it
        session.undo()
        session.undo()
        self.assertEqual(json.loads(session.call_json("openings", "{}"))["cutouts"], [])

    def test_vent_rows_on_lid_and_wall_with_a_cutout(self):
        session = self.session("demo")
        x0, y0, x1, y1 = session._data["bounds"]
        height = json.loads(session.call_json("openings", "{}"))["tray_height"]
        cut = dict(side="front", center=(x0+x1)/2, width=9.0, bottom=height-4, height=10.0)
        vents = [dict(face="lid", center=(x0+x1)/2, across=(y0+y1)/2, count=8, width=2.0, length=20.0, pitch=5.0),
                 dict(face="right", center=(y0+y1)/2, across=2.0, count=5, width=2.0, length=5.0, pitch=4.0)]
        session.set_cutouts([cut])
        reply = json.loads(session.call_json("set_openings", json.dumps({"vents": vents})))
        self.assertEqual((reply["cutouts"], reply["vents"]), ([cut], vents))
        names = [f["name"] for f in json.loads(session.call_json("features", "{}"))["features"]]
        self.assertEqual(names[-6:], ["Cutout1", "Vent2", "Vent2Pattern", "LidPad", "Vent1", "Vent1Pattern"])
        targets = expected(session._data, session.get_settings(), [cut], vents)
        for body in ("Tray", "Lid"):
            self.assertAlmostEqual(session._doc.getObject(body).Shape.Volume, targets[body]["volume"], places=6)
        session.set_parameter("lid_thickness", 3.0)  # the lid slots follow the lid
        self.assertAlmostEqual(session._doc.Lid.Shape.Volume,
                               expected(session._data, session.get_settings(), [cut], vents)["Lid"]["volume"], places=6)
        session.undo()
        session.undo()
        self.assertEqual(json.loads(session.call_json("openings", "{}"))["vents"], [])
        session.redo()
        bad = [[dict(vents[1], face="front", center=(x0+x1)/2, across=height-3, count=1)],  # overlaps the cutout
               [dict(vents[0], across=y1 + 4)]]                                            # off the lid
        for rows in bad:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                session.set_vents(rows)
            self.assertEqual(json.loads(session.call_json("openings", "{}"))["vents"], vents)
        session.save()
        session.close()
        reopened = self.session("demo")
        self.assertEqual(json.loads(reopened.call_json("openings", "{}"))["vents"], vents)
        reopened.set_openings([], [])
        self.assertEqual([f["name"] for f in json.loads(reopened.call_json("features", "{}"))["features"]][-1], "LidPad")

    def test_lid_screws_raise_the_margin_in_one_step(self):
        session = self.session("demo")
        x0, y0, x1, y1 = session._data["bounds"]
        before = session.get_settings()
        posts = json.loads(session.call_json("openings", "{}"))["screw_posts"]
        screws = {"screw": "M3", "insert": True}
        with self.assertRaisesRegex(ValueError, "margin"):  # 3 mm default margin is too small
            session.call_json("set_openings", json.dumps({"screws": screws}))
        reply = json.loads(session.call_json("set_openings", json.dumps(
            {"screws": screws, "settings": {"margin": posts["M3-insert"]}})))
        self.assertEqual(reply["screws"], screws)
        self.assertEqual(session.get_settings()["margin"], posts["M3-insert"])
        names = [f["name"] for f in json.loads(session.call_json("features", "{}"))["features"]]
        self.assertEqual(names[-7:-2], ["ScrewPost", "ScrewPosts", "ScrewHole", "ScrewHoles", "LidPad"])
        self.assertEqual(names[-2:], ["LidScrewHole", "LidScrewHoles"])
        targets = expected(session._data, session.get_settings(), [], [], screws)
        for body in ("Tray", "Lid"):
            self.assertAlmostEqual(session._doc.getObject(body).Shape.Volume, targets[body]["volume"], places=6)
        session.set_parameter("wall", 2.6)  # posts and holes follow the cavity
        # A deeper tray deepens the holes too (to their 8 mm cap): before, the holes kept the depth
        # they had when the screws were added and the session's own check refused the change.
        session.set_parameter("board_clearance", 9.1)
        targets = expected(session._data, session.get_settings(), [], [], screws)
        self.assertAlmostEqual(session._doc.getObject("Tray").Shape.Volume, targets["Tray"]["volume"], places=6)
        session.set_parameter("margin", 9.0)
        with self.assertRaisesRegex(ValueError, "margin"):  # the posts need their margin
            session.set_parameter("margin", 4.0)
        lid_slot_on_hole = dict(face="lid", center=x0 - 9 + posts["M3-insert"]/2, across=(y0+y1)/2, count=1,
                                width=2.0, length=y1 - y0 + 10, pitch=5.0)
        with self.assertRaisesRegex(ValueError, "screw holes"):
            session.set_vents([lid_slot_on_hole])
        session.undo()
        session.undo()
        session.undo()
        session.undo()  # screws and their margin go together
        self.assertEqual(session.get_settings(), before)
        self.assertIsNone(json.loads(session.call_json("openings", "{}"))["screws"])
        session.redo()
        self.assertEqual(json.loads(session.call_json("openings", "{}"))["screws"], screws)
        session.save()
        session.close()
        reopened = self.session("demo")
        self.assertEqual(json.loads(reopened.call_json("openings", "{}"))["screws"], screws)
        reopened.call_json("set_openings", json.dumps({"screws": None}))
        self.assertEqual([f["name"] for f in json.loads(reopened.call_json("features", "{}"))["features"]][-1], "LidPad")

    def test_sketches_cut_and_extrude_on_every_face(self):
        session = self.session("demo")  # board 10..60 x 20..50, tray 9.6 high, walls 5 mm out
        sketches = [
            dict(face="lid", operation="cut", depth=None, shapes=[
                dict(kind="circle", x=35.0, y=35.0, diameter=6.0),
                dict(kind="polygon", points=[[45.0, 25.0], [55.0, 25.0], [55.0, 30.0], [50.0, 30.0], [50.0, 40.0], [45.0, 40.0]])]),
            dict(face="lid", operation="extrude", depth=2.0, shapes=[dict(kind="rect", x=15.0, y=40.0, width=10.0, height=8.0)]),
            dict(face="front", operation="extrude", depth=3.0, shapes=[dict(kind="rect", x=20.0, y=2.0, width=12.0, height=4.0)]),
            dict(face="back", operation="cut", depth=None, shapes=[dict(kind="polygon", points=[[30.0, 2.0], [40.0, 2.0], [35.0, 7.0]])]),
            dict(face="right", operation="cut", depth=1.0, shapes=[dict(kind="circle", x=35.0, y=5.0, diameter=4.0)]),
            dict(face="left", operation="extrude", depth=2.0, shapes=[
                dict(kind="polygon", points=[[25.0, 1.0], [35.0, 1.0], [33.0, 6.0], [27.0, 6.0]])])]
        baseline = (session._doc.Tray.Shape.Volume, session._doc.Lid.Shape.Volume)
        reply = json.loads(session.call_json("set_openings", json.dumps({"sketches": sketches})))
        self.assertEqual(reply["sketches"], sketches)
        names = [f["name"] for f in json.loads(session.call_json("features", "{}"))["features"]]
        self.assertIn("UserSketch3Extrude", names)
        self.assertEqual(names[-2:], ["UserSketch1Cut", "UserSketch2Extrude"])
        targets = expected(session._data, session.get_settings(), sketches=sketches)
        for body in ("Tray", "Lid"):  # _check_document already compared volumes and bounds exactly
            self.assertAlmostEqual(session._doc.getObject(body).Shape.Volume, targets[body]["volume"], places=6)
        self.assertAlmostEqual(session._doc.Tray.Shape.BoundBox.YMin, 15.0 - 3.0, places=6)
        self.assertAlmostEqual(session._doc.Lid.Shape.BoundBox.ZMax, 9.6 + 0.25 + 2.0 + 2.0, places=6)
        for obj in session._doc.Objects:
            if obj.Name.startswith("UserSketch") and obj.TypeId == "Sketcher::SketchObject":
                self.assertTrue(obj.FullyConstrained, obj.Name)
        session.set_parameter("wall", 2.6)  # through cuts follow the wall, extrusions its face
        session.undo()
        for bad in ([dict(sketches[0], shapes=[dict(kind="circle", x=35.0, y=35.0, diameter=60.0)])],        # off the lid
                    [sketches[1], dict(sketches[1], shapes=[dict(kind="circle", x=20.0, y=44.0, diameter=2.0)])],  # overlap
                    [dict(sketches[2], depth=None)],                                                          # no depth
                    [dict(sketches[4], depth=5.0)],                                                           # deeper than the wall
                    [dict(sketches[3], shapes=[dict(kind="polygon", points=[[30.0, 2.0], [40.0, 7.0], [40.0, 2.0], [30.0, 7.0]])])]):  # crosses itself
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                session.set_openings(sketches=bad)
            self.assertEqual(json.loads(session.call_json("openings", "{}"))["sketches"], sketches)
        session.undo()
        self.assertEqual(json.loads(session.call_json("openings", "{}"))["sketches"], [])
        self.assertAlmostEqual(session._doc.Tray.Shape.Volume, baseline[0], places=6)
        session.redo()
        for part in (0, 1):  # STEP reimports are checked against the sketched solids
            session.export_step(part, str(self.root / f"part{part}.step"))
        session.save()
        session.close()
        reopened = self.session("demo")
        self.assertEqual(json.loads(reopened.call_json("openings", "{}"))["sketches"], sketches)
        reopened.set_openings(sketches=[])
        self.assertAlmostEqual(reopened._doc.Lid.Shape.Volume, baseline[1], places=6)

    def test_sketch_arcs_and_exact_separation(self):
        session = self.session("demo")
        ell = dict(kind="polygon", points=[[45.0, 25.0], [55.0, 25.0], [55.0, 30.0], [50.0, 30.0], [50.0, 40.0], [45.0, 40.0]])
        bowl = dict(kind="polygon", points=[[20.0, 25.0, 0.5], [30.0, 25.0], [30.0, 35.0], [20.0, 35.0]])  # bulges down 2.5 mm
        notch = dict(kind="polygon", points=[[40.0, 1.0], [50.0, 1.0, -1.0], [50.0, 6.0], [40.0, 6.0]])   # half circle inward
        in_ell_box = dict(kind="circle", x=53.0, y=36.0, diameter=3.0)  # inside the L's box, 1.5 mm from the L
        sketches = [dict(face="lid", operation="cut", depth=None, shapes=[ell, bowl, in_ell_box]),
                    dict(face="front", operation="extrude", depth=2.0, shapes=[notch])]
        session.set_openings(sketches=sketches)  # _check_document compares exact volumes with the arcs
        targets = expected(session._data, session.get_settings(), sketches=sketches)
        self.assertAlmostEqual(session._doc.Lid.Shape.Volume, targets["Lid"]["volume"], places=6)
        self.assertAlmostEqual(session._doc.Tray.Shape.Volume, targets["Tray"]["volume"], places=6)
        self.assertAlmostEqual(session._doc.Lid.Shape.BoundBox.ZMin, 9.6 + 0.25, places=6)
        self.assertFalse(session._doc.Lid.Shape.isInside(App.Vector(25.0, 23.0, 9.6 + 0.25 + 1.0), 1e-7, True),
                         "the arc's bulge is cut")
        for obj in session._doc.Objects:
            if obj.Name.startswith("UserSketch") and obj.TypeId == "Sketcher::SketchObject":
                self.assertTrue(obj.FullyConstrained, obj.Name)
        touching = dict(kind="rect", x=55.0, y=25.0, width=3.0, height=3.0)  # shares the L's edge
        inside = dict(kind="circle", x=47.5, y=35.0, diameter=2.0)            # within the L
        for shape in (touching, inside):
            with self.subTest(shape=shape), self.assertRaisesRegex(ValueError, "overlap"):
                session.set_openings(sketches=[dict(sketches[0], shapes=[ell, shape])])
        with self.assertRaisesRegex(ValueError, "half circle"):
            session.set_openings(sketches=[dict(sketches[1], shapes=[dict(notch, points=[[40.0, 1.0], [50.0, 1.0, -1.5], [50.0, 6.0], [40.0, 6.0]])])])
        solved = json.loads(session.call_json("solve_sketch", json.dumps({"points": notch["points"], "edge": 2, "length": 8.0})))["points"]
        for actual, wanted in zip(solved, [[42, 1], [50, 1, -1], [50, 6], [42, 6]]):
            self.assertEqual(len(actual), len(wanted))
            for a, w in zip(actual, wanted):
                self.assertAlmostEqual(a, w, places=6)
        with self.assertRaisesRegex(ValueError, "arc"):
            session.call_json("solve_sketch", json.dumps({"points": notch["points"], "edge": 1, "length": 8.0}))
        session.save()
        session.close()
        self.assertEqual(json.loads(self.session("demo").call_json("openings", "{}"))["sketches"], sketches)

    def test_mechanical_only_box(self):
        """A project without a board: a 60 x 40 x 20 mm space as bounds and thickness, no holes."""
        box = {"schema": 1, "units": "mm", "coordinate_system": "KiCad XY unchanged; enclosure Z up",
               "source": {"name": "corvcad.project.json", "sha256": "0" * 64, "kicad_version": "none"},
               "bounds": [0.0, 0.0, 60.0, 40.0], "board_thickness": 20.0, "mounting_holes": []}
        settings = dict(DEFAULTS, margin=0.0, board_clearance=0.0, standoff_height=0.0)
        session = self.session(data=box, settings=settings)
        tray = session._doc.Tray.Shape.BoundBox
        self.assertAlmostEqual(tray.ZLength, settings["floor"] + 20.0, places=6)
        self.assertAlmostEqual(tray.XLength, 60.0 + 2 * settings["wall"], places=6)
        self.assertNotIn("BossPad", [f["name"] for f in json.loads(session.call_json("features", "{}"))["features"]])
        session.call_json("set_openings", json.dumps({"screws": {"screw": "M3", "insert": False}, "settings": {"margin": 6.0},
                                                      "sketches": [dict(face="front", operation="cut", depth=None,
                                                                        shapes=[dict(kind="circle", x=30.0, y=12.0, diameter=8.0)])]}))
        session.save()
        session.close()
        reopened = self.session(data=box, settings=settings)
        self.assertEqual(reopened.get_settings()["margin"], 6.0)
        with self.assertRaises(ValueError):  # a board with holes still needs standoffs
            validate_board = json.loads((FIXTURES / "demo.json").read_text())
            runtime.validate(validate_board, settings)

    def test_solve_sketch_moves_only_the_edges_end(self):
        session = self.session("demo")
        ell = [[45.0, 25.0], [55.0, 25.0], [55.0, 30.0], [50.0, 30.0], [50.0, 40.0], [45.0, 40.0]]
        points = json.loads(session.call_json("solve_sketch", json.dumps({"points": ell, "edge": 0, "length": 8.0})))["points"]
        for actual, wanted in zip(points, [[45, 25], [53, 25], [53, 30], [50, 30], [50, 40], [45, 40]]):
            self.assertAlmostEqual(actual[0], wanted[0], places=6)
            self.assertAlmostEqual(actual[1], wanted[1], places=6)
        trapezoid = [[25.0, 1.0], [35.0, 1.0], [33.0, 6.0], [27.0, 6.0]]
        points = json.loads(session.call_json("solve_sketch", json.dumps({"points": trapezoid, "edge": 1, "length": 7.0})))["points"]
        self.assertAlmostEqual(math.dist(points[1], points[2]), 7.0, places=6)
        self.assertAlmostEqual(points[2][1], points[3][1], places=6)  # still horizontal
        self.assertEqual(points[:2], trapezoid[:2])
        for arguments in ({"points": ell, "edge": 6, "length": 8.0}, {"points": ell, "edge": 0, "length": 0.1},
                          {"points": ell[:2], "edge": 0, "length": 8.0}):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                session.call_json("solve_sketch", json.dumps(arguments))

    def test_invalid_changes_preserve_solids_history_and_redo(self):
        session = self.session()
        session.set_parameter("wall", 2.6)
        session.undo()
        baseline = session.get_settings()
        shapes = tuple(session._part(part).Shape.exportBrepToString() for part in (0, 1))
        for name, value in [("wall", -1), ("floor", 0), ("margin", float("nan")),
                            ("wall", float("inf")), ("wall", True), ("lid_gap", 1000),
                            ("standoff_diameter", 3.2), ("unknown", 3)]:
            with self.subTest(name=name, value=value):
                with self.assertRaises((ValueError, TypeError)):
                    session.set_parameter(name, value)
                self.assertEqual(session.get_settings(), baseline)
                self.assertEqual(tuple(session._part(part).Shape.exportBrepToString() for part in (0, 1)), shapes)
                self.assertTrue(session.can_redo())
                self.assertFalse(session.can_undo())
        session.redo()
        self.assertEqual(session.get_settings()["wall"], 2.6)

    def test_recompute_failure_aborts_transaction_without_history_change(self):
        session = self.session()
        baseline = session.get_settings()
        volume = session._doc.Tray.Shape.Volume
        original = session._check_document
        calls = []

        def fail_after_real_recompute():
            original()
            calls.append(True)
            if len(calls) == 2:
                raise RuntimeError("injected failure after actual geometric recompute")
        with patch.object(session, "_check_document", fail_after_real_recompute):
            with self.assertRaisesRegex(RuntimeError, "injected failure"):
                session.set_parameter("wall", 2.6)
        self.assertEqual(session.get_settings(), baseline)
        self.assertAlmostEqual(session._doc.Tray.Shape.Volume, volume)
        self.assertFalse(session.can_undo())
        self.assertFalse(session.can_redo())
        session.recompute()

    def test_preview_and_separate_exports_use_the_same_exact_solids(self):
        for fixture in ("demo", "roundtrip"):
            session = self.session(fixture, fixture)
            session.set_parameter("wall", 2.7)
            targets = expected(session._data, session.get_settings())
            for part, name in ((0, "Tray"), (1, "Lid")):
                triangles = session.triangles(part)
                self.assertTrue(triangles)
                self.assertTrue(all(len(t) == 9 and all(math.isfinite(v) for v in t) for t in triangles))
                vertices = [tuple(t[i:i+3]) for t in triangles for i in (0, 3, 6)]
                preview = manifold(vertices, [(i, i+1, i+2) for i in range(0, len(vertices), 3)])
                path = self.root / f"{fixture}-{name}.step"
                session.export_step(part, str(path))
                solid = Part.Shape()
                solid.read(str(path))
                runtime._shape_check(solid, targets[name])
                stl = path.with_suffix(".stl")
                session.export_stl(part, str(stl))
                mesh = Mesh.Mesh(str(stl))
                self.assertTrue(mesh.isSolid())
                report = manifold(*mesh.Topology)
                self.assertAlmostEqual(preview["signed_volume_mm3"], report["signed_volume_mm3"], delta=0.01)
                self.assertEqual(preview["triangles"], report["triangles"])
            self.assertEqual(sorted(path.suffix for path in self.root.glob(".corvcad-*")), [])

    def test_unknown_properties_and_custom_objects_survive_save_reopen(self):
        session = self.session()
        doc = session._doc
        doc.Tray.addProperty("App::PropertyString", "UserNote")
        doc.Tray.UserNote = "preserve me"
        custom = doc.addObject("App::FeaturePython", "UserMetadata")
        custom.addProperty("App::PropertyString", "Value")
        custom.Value = "opaque extension data"
        session.save()
        session.close()
        reopened = self.session()
        reopened.set_parameter("floor", 3)
        reopened.save()
        reopened.close()
        final = self.session()
        self.assertEqual(final._doc.Tray.UserNote, "preserve me")
        self.assertEqual(final._doc.UserMetadata.Value, "opaque extension data")
        self.assertFalse(list(self.root.glob(".corvcad-*")))

    def test_external_document_and_export_edits_are_never_overwritten(self):
        session = self.session()
        session.save()
        document = session._path
        session.set_parameter("wall", 2.6)
        document.write_bytes(document.read_bytes() + b"external edit")
        checksum = runtime._hash(document)
        with self.assertRaisesRegex(ValueError, "externally modified"):
            session.save()
        self.assertEqual(runtime._hash(document), checksum)
        for suffix in ("step", "stl"):
            export = getattr(session, "export_" + suffix)
            unknown = self.root / ("unknown." + suffix)
            unknown.write_text("user data")
            with self.assertRaisesRegex(ValueError, "unknown"):
                export(0, str(unknown))
            self.assertEqual(unknown.read_text(), "user data")
            own = self.root / ("own." + suffix)
            export(0, str(own))
            export(0, str(own))
            own.write_bytes(own.read_bytes() + b"external edit")
            digest = runtime._hash(own)
            with self.assertRaisesRegex(ValueError, "externally modified"):
                export(0, str(own))
            self.assertEqual(runtime._hash(own), digest)

    def test_failed_write_and_new_file_race_leave_existing_outputs_intact(self):
        session = self.session()
        session.save()
        checksum = runtime._hash(session._path)

        def broken_writer(path):
            path.write_bytes(b"incomplete")
            raise RuntimeError("injected writer failure")
        with self.assertRaisesRegex(RuntimeError, "writer failure"):
            session._publish(session._path, broken_writer, checksum)
        self.assertEqual(runtime._hash(session._path), checksum)
        raced = self.root / "raced.step"

        def raced_writer(path):
            path.write_text("candidate")
            raced.write_text("external writer")
        with self.assertRaises(FileExistsError):
            session._publish(raced, raced_writer, None)
        self.assertEqual(raced.read_text(), "external writer")
        self.assertFalse(list(self.root.glob(".corvcad-*")))

    def test_mismatched_board_partial_model_and_wrong_provenance_rejected(self):
        session = self.session()
        session.save()
        session.close()
        checksum = runtime._hash(session._path)
        data = copy.deepcopy(session._data)
        data["source"]["name"] = "another.kicad_pcb"
        with self.assertRaisesRegex(ValueError, "mechanical input mismatch"):
            self.session(data=data)
        data = copy.deepcopy(session._data)
        data["bounds"][2] += 1
        with self.assertRaisesRegex(ValueError, "mechanical input mismatch"):
            self.session(data=data)
        self.assertEqual(runtime._hash(session._path), checksum)
        reopened = self.session()
        reopened._doc.removeObject("LidPad")
        # Simulate an external FreeCAD save of an incomplete feature graph.
        reopened._doc.saveAs(str(self.root / "partial.FCStd"))
        reopened.close()
        with self.assertRaisesRegex(ValueError, "Partial or incompatible"):
            self.session(name="partial")
        unrelated = App.newDocument("Unrelated")
        try:
            unrelated.addObject("Part::Feature", "Shape").Shape = Part.makeBox(1, 1, 1)
            unrelated.saveAs(str(self.root / "unknown.FCStd"))
        finally:
            App.closeDocument(unrelated.Name)
        with self.assertRaisesRegex(ValueError, "provenance mismatch"):
            self.session(name="unknown")

    def test_two_documents_have_independent_parameters_history_and_exports(self):
        first, second = self.session(name="first"), self.session("roundtrip", "second")
        second_baseline = second.get_settings()
        first.set_parameter("wall", 2.8)
        self.assertEqual(second.get_settings(), second_baseline)
        self.assertFalse(second.can_undo())
        second.set_parameter("floor", 2.9)
        first.undo()
        self.assertEqual(second.get_settings()["floor"], 2.9)
        first.close()
        second.recompute()
        second.save()
        with self.assertRaisesRegex(ValueError, "closed"):
            first.get_settings()
        for part in (-1, 2, True, "Tray"):
            with self.assertRaises(ValueError):
                second.triangles(part)

    def test_same_path_cannot_share_or_close_another_sessions_document(self):
        first = self.session()
        with self.assertRaisesRegex(ValueError, "already open"):
            self.session()
        first.recompute()
        first.save()
        with self.assertRaisesRegex(ValueError, "already open"):
            self.session()
        first.recompute()
        first.close()
        reopened = self.session()
        reopened.recompute()

    def test_constructor_failure_cleans_only_its_new_document(self):
        existing = self.session(name="keep")
        before = set(App.listDocuments())

        def fail_build(data, settings, cutouts=(), vents=(), screws=None, sketches=()):
            App.newDocument("InterruptedBuild")
            raise RuntimeError("injected construction failure")
        with patch.object(runtime.model, "build", fail_build):
            with self.assertRaisesRegex(RuntimeError, "construction failure"):
                self.session(name="retry")
        self.assertEqual(set(App.listDocuments()), before)
        existing.recompute()
        retry = self.session(name="retry")
        retry.recompute()

    def test_legacy_f1_adoption_is_read_only_until_explicit_save(self):
        data = json.loads((FIXTURES / "demo.json").read_text())
        doc = runtime.model.build(data)
        try:
            doc.saveAs(str(self.root / "legacy.FCStd"))
        finally:
            App.closeDocument(doc.Name)
        path = self.root / "legacy.FCStd"
        checksum = runtime._hash(path)
        session = self.session(name="legacy")
        self.assertEqual(runtime._hash(path), checksum)
        session.set_parameter("wall", 2.6)
        session.save()
        self.assertNotEqual(runtime._hash(path), checksum)

    def test_electronics_revision_keeps_model_and_updates_provenance_only_on_save(self):
        session = self.session()
        session.set_parameter("wall", 2.6)
        session.save()
        original_sha = session._data["source"]["sha256"]
        session.close()
        checksum = runtime._hash(session._path)
        data = copy.deepcopy(session._data)
        data["source"]["sha256"] = "0" * 64
        reopened = self.session(data=data)
        self.assertEqual(reopened.get_settings()["wall"], 2.6)
        self.assertEqual(reopened._doc.InputProvenance.BoardSHA256, original_sha)
        self.assertEqual(runtime._hash(reopened._path), checksum)
        reopened.recompute()
        reopened.save()
        self.assertEqual(reopened._doc.InputProvenance.BoardSHA256, "0" * 64)
        reopened.close()
        final = self.session(data=data)
        self.assertEqual(final._doc.InputProvenance.BoardSHA256, "0" * 64)
        self.assertEqual(final.get_settings()["wall"], 2.6)
        final.close()
        changed = copy.deepcopy(data)
        changed["mounting_holes"][0]["x"] += 1
        with self.assertRaisesRegex(ValueError, "mechanical input mismatch"):
            self.session(data=changed)


if __name__ == "__main__":
    unittest.main(verbosity=2)
