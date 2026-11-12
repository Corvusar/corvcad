"""Run selected upstream App test suites inside the F2 FreeCADCmd process."""
import json
import os
from pathlib import Path
import time
import unittest
import sys

import FreeCAD as App

started = time.monotonic()
names = ["BaseTests", "UnitTests", "Document", "Metadata", "StringHasher", "UnicodeTests", "TestPythonSyntax",
         "TestPartApp", "TestPartDesignApp", "TestSketcherApp",
         "TestSpreadsheet", "MeshTestsApp", "TestMaterialsApp"]
records = {}
for name in names:
    suite = unittest.defaultTestLoader.loadTestsFromName(name)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    records[name] = {"run": result.testsRun, "failures": len(result.failures),
                     "errors": len(result.errors), "skipped": len(result.skipped),
                     "expected_failures": len(result.expectedFailures),
                     "unexpected_successes": len(result.unexpectedSuccesses),
                     "passed": result.wasSuccessful()}
report = {"status": "passed" if all(r["passed"] and r["run"] for r in records.values()) else "failed",
          "freecad_version": App.Version(), "suites": records,
          "loaded_python_files": sorted({str(Path(module.__file__).resolve())
                                         for module in list(sys.modules.values())
                                         if getattr(module, "__file__", None) and Path(module.__file__).is_file()}),
          "elapsed_seconds": time.monotonic()-started,
          "import_coverage": "No upstream Import App test suite at this pin; F1 checks STEP export/reimport"}
Path(os.environ["CORVCAD_FREECAD_TEST_REPORT"]).write_text(json.dumps(report, indent=2)+"\n")
if report["status"] != "passed":
    # The outer runner checks this report. FreeCADCmd can retry a Python file
    # after a top-level exception, needlessly executing the entire suite twice.
    print("Upstream App tests failed; see report and process log", file=sys.stderr)
