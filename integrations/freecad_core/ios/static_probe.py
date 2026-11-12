"""Executed by a static FreeCADCmd; no successful report until all imports pass."""
import importlib.util
import json
import os
from pathlib import Path
import sys

names = ["FreeCAD", "__FreeCADBase__", "Materials", "Part", "Mesh", "Sketcher", "_PartDesign", "Spreadsheet", "Import"]
origins = {}
for name in names:
    module = __import__(name)
    spec = importlib.util.find_spec(name)
    origins[name] = spec.origin if spec else None
    if origins[name] != "built-in":
        raise AssertionError(f"{name} did not resolve from the inittab: {origins[name]}")
if "FreeCADGui" in sys.modules:
    raise AssertionError("GUI module loaded by static headless profile")
Path(os.environ["CORVCAD_STATIC_REPORT"]).write_text(json.dumps({
    "schema": 1, "passed": True, "platform": sys.platform, "extension_origins": origins,
    "scope": "Static FreeCAD modules; SDK/runtime acceptance is separate"
}, indent=2) + "\n")
