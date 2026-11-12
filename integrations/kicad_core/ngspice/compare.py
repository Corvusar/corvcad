"""Compare ngspice ASCII raw files: the executable's reference (all plots in one file) with the
probe's (one file per plot). Plots are matched by their Plotname and must have the same vectors.

- Same points: values must agree within ngspice's default RELTOL (1e-3) of the vector's largest
  magnitude, or of ngspice's absolute tolerance (VNTOL 1e-6 V, ABSTOL 1e-12 A) for a vector
  that never exceeds it. Builds for different platforms differ in the last digits.
- A transient with a different number of points (another platform's rounding took another
  adaptive time-step path): the probe is interpolated onto the reference's time points, and each
  vector's RMS difference must be within 1% of its largest magnitude. Switching edges then land
  a step apart, so the largest single difference is reported but not judged.

The probe can also be the text cc_sch_session_simulate returns (one plot, KiCad's netlist): it
is compared with the reference's single plot, and every reference vector must be in it; the
session's extra vectors (KiCad saves currents and power the plain netlist export doesn't) are
counted, not compared. Names are compared in ngspice's own form, as the session reports them:
a raw file's v(node) is node, i(device) is device#branch.

Vectors whose names match --ignore are left out: ngspice seeds its noise sources (TRNOISE,
TRRANDOM) from the process id, so those differ from run to run.
  compare.py [--ignore REGEX] <reference.raw> <probe.0.raw|session.txt> [<probe.1.raw>...]
Prints one line; exits 1 on a mismatch."""
import math
import re
import sys

RTOL = 1e-3  # ngspice's default RELTOL
RMS_TOL = 1e-2
EXACT = 1e-12


def internal(name):
    """A raw file's vector name in ngspice's own form."""
    name = name.lower()
    if m := re.fullmatch(r"v\((.*)\)", name):
        return m.group(1)
    if m := re.fullmatch(r"i\((.*)\)", name):
        return m.group(1) + "#branch"
    return name


def session(text):
    """[(None, {vector: [values]})] from cc_sch_session_simulate's text."""
    lines = text.splitlines()
    analysis, plot, points, kept = lines[0].split("\t")
    assert points == kept, "a decimated result can't be compared point by point"
    values = {}
    for line in lines[1:]:
        name, kind, data = (line.split("\t") + [""])[:3]
        # An XSPICE port's current is <instance>#branch_<port>_<index> inside ngspice; a raw
        # file names it i(<instance>).
        values[re.sub(r"#branch_\d+_\d+$", "#branch", name.lower())] = [complex(*map(float, v.split(","))) if kind == "complex" else float(v)
                                for v in data.split()]
    return [(None, values)]


def plots(path):
    """[(plotname, {vector: [values]})] from an ASCII raw file or a session's text."""
    out = []
    text = open(path, encoding="latin-1").read()
    if not text.startswith("Title:"):
        return session(text)
    for chunk in re.split(r"^Title:", text, flags=re.M)[1:]:
        head, _, body = chunk.partition("\nValues:")
        header = dict(l.split(":", 1) for l in head.splitlines() if ":" in l and not l.startswith("\t"))
        header = {k.strip(): v.strip() for k, v in header.items()}
        nvars, npoints = int(header["No. Variables"]), int(header["No. Points"])
        names = [internal(l.split()[1]) for l in head.split("\nVariables:", 1)[1].splitlines() if l.strip()]
        assert len(names) == nvars, path
        tokens = body.split()
        assert len(tokens) == npoints * (nvars + 1), path
        complex_ = "complex" in header.get("Flags", "")
        # A netlist that saves a current twice (".save i(l1)" and ".probe alli") has two columns
        # with one internal name; they hold the same vector, and the first is kept.
        first = {}
        for k, n in enumerate(names):
            first.setdefault(n, k)
        values = {n: [] for n in first}
        for p in range(npoints):
            for n, k in first.items():
                v = tokens[p * (nvars + 1) + 1 + k]
                values[n].append(complex(*map(float, v.split(","))) if complex_ else float(v))
        out.append((header["Plotname"], values))
    return out


def resample(times, values, at):
    """values(times) linearly interpolated at the times in at (both ascending)."""
    out, j = [], 0
    for t in at:
        while j + 2 < len(times) and times[j + 1] < t:
            j += 1
        t0, t1 = times[j], times[j + 1]
        f = (t - t0) / (t1 - t0) if t1 != t0 else 0.0
        out.append(values[j] + f * (values[j + 1] - values[j]))
    return out


def main():
    args = sys.argv[1:]
    ignore = None
    if args[0] == "--ignore":
        ignore, args = re.compile(args[1]), args[2:]
    reference = plots(args[0])
    probe = [p for path in args[1:] for p in plots(path)]
    from_session = probe and probe[0][0] is None
    if from_session:
        if len(reference) != 1:
            print(f"the reference has {len(reference)} plots; a session returns one")
            return 1
        probe = [(reference[0][0], probe[0][1])]
    if sorted(n for n, _ in reference) != sorted(n for n, _ in probe):
        print(f"plots differ: {[n for n, _ in reference]} vs {[n for n, _ in probe]}")
        return 1
    worst = worst_rms = 0.0
    points, notes, ok = 0, "", True
    for name, ref in reference:
        got = next(v for n, v in probe if n == name)
        missing = sorted(set(ref) - set(got))
        extra = sorted(set(got) - set(ref))
        if missing or (extra and not from_session):
            print(f"{name}: vectors differ: missing {missing}, extra {extra}")
            return 1
        if extra:
            notes += f", {len(extra)} more vectors in the session"
        shifted = "time" in ref and len(ref["time"]) != len(got["time"])
        if shifted:
            notes += f", {name} interpolated ({len(got['time'])} points onto {len(ref['time'])})"
            got = {v: resample(got["time"], got[v], ref["time"]) for v in ref}
        for vector, a in ref.items():
            if ignore and ignore.search(vector):
                continue
            b = got[vector]
            if len(a) != len(b):
                print(f"{name} {vector}: {len(a)} points vs {len(b)}")
                return 1
            # Below ngspice's absolute tolerances (VNTOL for voltages, ABSTOL otherwise) a vector
            # is numerically zero, and its noise is not a difference.
            voltage = not re.search(r"[#:(@\[]", vector) and vector not in ("time", "frequency")
            scale = max(max(abs(x) for x in a), 1e-6 if voltage else 1e-12)
            d = [abs(x - y) / scale for x, y in zip(a, b)]
            rms = math.sqrt(sum(x * x for x in d) / len(d))
            worst, worst_rms = max(worst, max(d)), max(worst_rms, rms)
            ok = ok and (rms <= RMS_TOL if shifted else max(d) <= RTOL)
            points += len(a)
    verdict = ("same" if worst <= EXACT else "agree") if ok else "DIFFERENT"
    print(f"{verdict}: {len(reference)} plots, {points} values, largest relative difference "
          f"{worst:.3g}, largest RMS {worst_rms:.3g}{notes}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
