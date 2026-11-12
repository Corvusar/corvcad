"""Content-verified checkpoints for one native compiler unit. MIT.

Only this helper's successful compiler results create checkpoints. Existing object
files without checkpoints are never imported. A cache miss recompiles the unit;
an invalid fresh compiler result fails without replacing a previous checkpoint.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import tempfile

SCHEMA = 1


class InvalidCheckpoint(ValueError):
    pass


def _digest(path, lf=False):
    data = path.read_bytes()
    if lf:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def _roots(roots):
    result = {}
    for label, path in roots:
        if not isinstance(label, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", label) or label in result:
            raise InvalidCheckpoint("Invalid or duplicate compiler dependency root label")
        result[label] = Path(path).resolve()
    return result


def _dependency_path(key, roots):
    if not isinstance(key, str) or "\\" in key or ":" in key:
        raise InvalidCheckpoint("Nonportable compiler dependency key")
    parts = key.split("/")
    if len(parts) < 2 or any(part in ("", ".", "..") for part in parts):
        raise InvalidCheckpoint("Compiler dependency key escapes its root")
    label, tail = parts[0], PurePosixPath(*parts[1:])
    if label not in roots or tail.is_absolute():
        raise InvalidCheckpoint("Unknown compiler dependency root")
    path = (roots[label] / Path(*tail.parts)).resolve(strict=True)
    if not path.is_relative_to(roots[label]) or not path.is_file():
        raise InvalidCheckpoint("Compiler dependency path escapes its root or is not a file")
    return path


def _source_path(unit):
    _, row, command, _, _ = unit
    indices = [i for i, value in enumerate(command) if value == "-c"]
    if len(indices) != 1 or indices[0] + 1 >= len(command):
        raise InvalidCheckpoint("Expected one explicit compiler source after -c")
    source = Path(command[indices[0] + 1])
    if not source.is_absolute():
        source = Path(row["directory"]) / source
    return source.resolve(strict=True)


def _identity(unit, roots, output):
    key, row, command, obj, _ = unit
    if not isinstance(key, str) or not key or not isinstance(command, list) or not all(isinstance(x, str) for x in command):
        raise InvalidCheckpoint("Invalid compiler unit source or command")
    obj = Path(obj)
    destination = obj.resolve()
    objects = output / "objects"
    if obj.is_symlink() or not destination.is_relative_to(objects) or destination == objects:
        raise InvalidCheckpoint("Compiler object path escapes the output objects directory")
    return {"source": key, "command": command, "directory": str(Path(row["directory"]).resolve()),
            "object": str(destination), "roots": {label: str(path) for label, path in sorted(roots.items())}}


def _validate_result(unit, identity, record, dependencies, roots):
    if not isinstance(record, dict) or not isinstance(dependencies, dict) or not dependencies:
        raise InvalidCheckpoint("Missing compiler result or actual dependency hashes")
    if record.get("source") != identity["source"] or record.get("command") != identity["command"]:
        raise InvalidCheckpoint("Compiler result source or command mismatch")
    keys = record.get("dependencies")
    if not isinstance(keys, list) or not all(isinstance(x, str) for x in keys) or len(set(keys)) != len(keys) or set(keys) != set(dependencies):
        raise InvalidCheckpoint("Compiler dependency list differs from actual hash inventory")
    obj = record.get("object", {})
    if not isinstance(obj, dict) or not isinstance(obj.get("path"), str):
        raise InvalidCheckpoint("Missing compiler object metadata")
    path = Path(obj["path"])
    if path.is_symlink() or str(path.resolve(strict=True)) != identity["object"] or not path.is_file():
        raise InvalidCheckpoint("Compiler object does not match the requested output")
    if obj.get("sha256") != _digest(path):
        raise InvalidCheckpoint("Compiler object content changed")
    source = _source_path(unit)
    source_seen = False
    for key, expected in dependencies.items():
        path = _dependency_path(key, roots)
        if not isinstance(expected, dict) or expected.get("sha256") != _digest(path):
            raise InvalidCheckpoint("Compiler dependency content changed: " + key)
        if "sha256_lf" in expected and expected["sha256_lf"] != _digest(path, lf=True):
            raise InvalidCheckpoint("Compiler dependency normalized hash changed: " + key)
        source_seen |= path == source
    if not source_seen:
        raise InvalidCheckpoint("Actual compiler dependencies omit the command's source file")


def _save(path, payload):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", prefix=path.stem + "-",
                                         suffix=".tmp", dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(payload, stream, sort_keys=True, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def compile_cached(unit, roots, generated, output, compiler_callback):
    """Return compile_one's unchanged (record, dependencies), reusing verified bytes.

    compiler_callback receives exactly (unit, roots, generated, output). Its result
    must inventory the real -c source (including regenerated protobuf paths), the
    requested object, and all compiler dependencies with raw SHA-256 hashes.
    """
    output = Path(output).resolve()
    mapped = _roots(roots)
    identity = _identity(unit, mapped, output)
    checkpoint_directory = output / "checkpoints"
    checkpoint_directory.mkdir(parents=True, exist_ok=True)
    if checkpoint_directory.resolve() != checkpoint_directory or not checkpoint_directory.is_dir():
        raise InvalidCheckpoint("Checkpoint directory escapes the output directory")
    checkpoint = checkpoint_directory / (hashlib.sha256(identity["source"].encode()).hexdigest() + ".json")
    if checkpoint.is_file() and not checkpoint.is_symlink():
        try:
            payload = json.loads(checkpoint.read_text(encoding="utf-8"))
            if not isinstance(payload, dict) or payload.get("schema") != SCHEMA or payload.get("identity") != identity:
                raise InvalidCheckpoint("Checkpoint recipe mismatch")
            _validate_result(unit, identity, payload["record"], payload["dependencies"], mapped)
            return payload["record"], payload["dependencies"]
        except (OSError, ValueError, KeyError, TypeError):
            # Missing/stale/corrupt evidence is a miss, never proof of compilation.
            pass
    record, dependencies = compiler_callback(unit, roots, generated, output)
    _validate_result(unit, identity, record, dependencies, mapped)
    _save(checkpoint, {"schema": SCHEMA, "identity": identity, "record": record, "dependencies": dependencies})
    return record, dependencies
