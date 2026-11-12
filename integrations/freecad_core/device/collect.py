"""Collect development-only CorvProbe mechanical evidence; never accepts F5. MIT."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request

from packet import inside, record, save, verify

OUTPUTS = {"input.json", "enclosure.FCStd", "tray.step", "lid.step", "tray.stl", "lid.stl"}


class Probe:
    def __init__(self, host, token, port=8765):
        if not re.fullmatch(r"[A-Za-z0-9.-]+", host) or not token or not 0 < port < 65536:
            raise ValueError("Expected a LAN hostname/IP, token and valid port")
        self.base, self.token = f"http://{host}:{port}", token

    def call(self, route, data=None, timeout=600, **query):
        query["token"] = self.token
        request = urllib.request.Request(self.base + route + "?" + urllib.parse.urlencode(query),
                                         data=data, method="POST" if data is not None else "GET")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                body = response.read()
                return json.loads(body) if response.headers.get_content_type() == "application/json" else body
        except urllib.error.HTTPError as error:
            raise RuntimeError(f"CorvProbe {route}: HTTP {error.code}") from None
        except urllib.error.URLError:
            raise RuntimeError(f"CorvProbe {route}: connection failed") from None


def validate_mechanical_reply(reply, expected_input, hardware):
    if reply.get("schema") != 1 or reply.get("passed") is not True or reply.get("error"):
        raise ValueError("Mechanical diagnostic failed: " + str(reply.get("error", "missing success report")))
    if reply.get("hardware_class") != hardware:
        raise ValueError("Hardware classification mismatch; simulator results are not physical evidence")
    if reply.get("f5_accepted") is not False:
        raise ValueError("Device diagnostics cannot automatically accept F5")
    actual = reply.get("input", {})
    for name in ("schema", "units", "bounds", "board_thickness", "mounting_holes"):
        if actual.get(name) != expected_input.get(name):
            raise ValueError("Device mechanical input differs from accepted fixture: " + name)
    for name in ("name", "sha256"):
        if actual.get("source", {}).get(name) != expected_input.get("source", {}).get(name):
            raise ValueError("Device board provenance differs from uploaded bytes")
    if set(reply.get("files", {})) != OUTPUTS:
        raise ValueError("Missing or unexpected mechanical outputs")
    required = {"parameter_restore", "undo", "redo", "rejected_edit_rollback", "save_reopen_byte_preservation", "step", "stl"}
    if set(reply.get("operations", {})) != required or not all(reply["operations"].values()):
        raise ValueError("Incomplete mechanical operations")
    if not isinstance(reply.get("runtime_initialized_before"), bool):
        raise ValueError("Missing runtime initialization state")
    timings = reply.get("parameter_timings", [])
    if {item.get("parameter") for item in timings} != {
            "margin", "wall", "floor", "board_clearance", "standoff_height", "standoff_diameter", "lid_thickness", "lid_gap"}:
        raise ValueError("Missing parameter recompute timings")
    for entry in timings:
        for name in ("edit_recompute_ms", "restore_recompute_ms"):
            value = entry.get(name)
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError("Invalid parameter timing")


def validate_reply(reply, expected_input, hardware, metrics):
    validate_mechanical_reply(reply, expected_input, hardware)
    if hardware == "physical-device" and not str(metrics.get("machine", "")).startswith("iPad"):
        raise ValueError("Physical F5 requires an identified iPad")
    for name in ("footprint_before", "footprint_after", "footprint_peak_during", "wall_ms"):
        value = reply.get(name)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            raise ValueError("Missing actual process measurement: " + name)


def collect(packet, case_name, output, probe, hardware="physical-device", fresh_launch=False):
    packet, output = Path(packet), Path(output)
    manifest = verify(packet)
    if case_name not in manifest["cases"]:
        raise ValueError("Unknown packet case")
    case = manifest["cases"][case_name]
    remote = f"Gate5/{manifest['run_id']}/{case_name}"
    output.mkdir(parents=True, exist_ok=False)
    status = {"schema": 1, "gate": "F5-preparation", "f5_accepted": False, "status": "collecting",
              "expected_hardware": hardware, "operator_confirmed_fresh_launch": fresh_launch,
              "packet_sha256": record(packet / "packet.json")["sha256"], "case": case_name,
              "desktop_reimport": "pending", "slicer_review": "pending", "manual_device_ui": "pending"}
    try:
        metrics = probe.call("/metrics")
        save(output / "metrics-before.json", metrics)
        identity = probe.call("/run", command="mechanical-info")
        save(output / "identity-before.json", identity)
        if (identity.get("native_available") is not True or identity.get("hardware_class") != hardware
                or not isinstance(identity.get("process_id"), int)):
            raise ValueError("Expected native mechanical diagnostics on the requested hardware class")
        if hardware == "physical-device" and not str(metrics.get("machine", "")).startswith("iPad"):
            raise ValueError("Physical F5 requires an identified iPad")
        if fresh_launch and identity.get("runtime_initialized") is not False:
            raise ValueError("Runtime already initialized before collection")
        if probe.call("/ls", path=remote):
            raise ValueError("Refusing a reused remote input directory; prepare a new packet")
        for name, expected in case["files"].items():
            data = inside(packet, case_name + "/" + name).read_bytes()
            sent = probe.call("/put", data=data, path=remote + "/" + name)
            if sent.get("written") is not True or sent.get("bytes") != len(data):
                raise ValueError("Incomplete input upload")
            returned = probe.call("/get", path=remote + "/" + name)
            if not isinstance(returned, bytes) or returned != data:
                raise ValueError("Uploaded input read-back differs")
        reply = probe.call("/run", command="mechanical", path=remote + "/request.json")
        save(output / "probe-reply.json", reply)
        expected_input = json.loads(inside(packet, case_name + "/expected-input.json").read_text())
        validate_reply(reply, expected_input, hardware, metrics)
        if reply.get("process_id") != identity["process_id"]:
            raise ValueError("App restarted before the mechanical run")
        if fresh_launch and reply["runtime_initialized_before"]:
            raise ValueError("Runtime was already initialized; this is not a cold measurement")
        for name, expected in reply["files"].items():
            data = probe.call("/get", path=remote + "/outputs/" + name)
            if not isinstance(data, bytes) or {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()} != expected:
                raise ValueError("Downloaded output differs from device hash: " + name)
            (output / name).write_bytes(data)
        if json.loads((output / "input.json").read_text()) != reply["input"]:
            raise ValueError("Downloaded mechanical input differs from the device report")
        device_report = probe.call("/get", path=remote + "/outputs/device-report.json")
        if not isinstance(device_report, bytes):
            raise ValueError("Missing durable device report")
        report = json.loads(device_report)
        if any(reply.get(key) != value for key, value in report.items()):
            raise ValueError("Durable device report differs from notification reply")
        (output / "device-report.json").write_bytes(device_report)
        after = probe.call("/metrics")
        save(output / "metrics-after.json", after)
        after_identity = probe.call("/run", command="mechanical-info")
        save(output / "identity-after.json", after_identity)
        if after_identity.get("process_id") != identity["process_id"]:
            raise ValueError("App restarted during collection")
        if after.get("uptime_ms", -1) < metrics.get("uptime_ms", 0):
            raise ValueError("App restarted during collection")
        timeline = probe.call("/timeline", since=metrics.get("uptime_ms", 0))
        save(output / "memory-timeline.json", timeline)
        samples = timeline.get("samples", [])
        if timeline.get("interval_ms") != 100 or not isinstance(samples, list) or not samples:
            raise ValueError("Missing actual physical-footprint samples")
        previous = metrics.get("uptime_ms", 0)
        for sample in samples:
            if (not isinstance(sample, list) or len(sample) != 2
                    or any(isinstance(v, bool) or not isinstance(v, (int,float)) or not math.isfinite(v) or v < 0 for v in sample)
                    or sample[0] < previous):
                raise ValueError("Invalid physical-footprint timeline")
            previous = sample[0]
        save(output / "device-log.json", probe.call("/log", seconds=600, limit=2000))
        # A screenshot records the visible app; the diagnostic does not alter its open UI project.
        screenshot = probe.call("/snapshot")
        if not isinstance(screenshot, bytes) or not screenshot.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError("Missing app-window screenshot")
        (output / "app-window.png").write_bytes(screenshot)
        save(output / "packet.json", manifest)
        inputs = output / "inputs"
        inputs.mkdir()
        for name in case["files"]:
            (inputs / name).write_bytes(inside(packet, case_name + "/" + name).read_bytes())
        status["status"] = "collected; independent desktop and slicer/device review pending"
        status["hardware_class"] = reply["hardware_class"]
        status["cold_runtime_observed"] = not reply["runtime_initialized_before"]
    except Exception as error:
        status["status"] = "failed"
        # Never retain the token, full authenticated URL, or urllib exception chain.
        status["error"] = str(error).replace(probe.token, "[redacted]")
        raise
    finally:
        # Failed runs need the actual device logs and memory history as well. Preserve
        # partial outputs under the failed receipt; they cannot enter desktop acceptance.
        if status["status"] == "failed":
            for name, route, query in (
                ("metrics-after.json", "/metrics", {}),
                ("memory-timeline.json", "/timeline", {"since": 0}),
                ("device-log.json", "/log", {"seconds": 600, "limit": 2000}),
            ):
                try:
                    if not (output / name).exists():
                        save(output / name, probe.call(route, timeout=5, **query))
                except Exception:
                    status.setdefault("unavailable_failure_evidence", []).append(route)
            try:
                entries = probe.call("/ls", path=remote + "/outputs", timeout=5)
                for entry in entries:
                    name = entry.get("name")
                    if name in OUTPUTS | {"device-report.json"} and not (output / name).exists():
                        data = probe.call("/get", path=remote + "/outputs/" + name, timeout=5)
                        if isinstance(data, bytes):
                            (output / name).write_bytes(data)
            except Exception:
                status.setdefault("unavailable_failure_evidence", []).append("partial outputs")
        status["artifacts"] = {p.relative_to(output).as_posix(): record(p) for p in sorted(output.rglob("*")) if p.is_file()}
        save(output / "collection.json", status)
    return status


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packet", type=Path, required=True)
    parser.add_argument("--case", choices=("demo", "roundtrip"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--expect-hardware", choices=("physical-device", "simulator"), default="physical-device")
    parser.add_argument("--fresh-launch-confirmed", action="store_true")
    args = parser.parse_args()
    probe = Probe(args.host, args.token_file.read_text(encoding="utf-8-sig").strip())
    result = collect(args.packet, args.case, args.output, probe, args.expect_hardware, args.fresh_launch_confirmed)
    print(result["status"] + "; F5 remains pending")
