"""A plain board for a simulation demo, for the app's UI test of the electrified 3D view: every
part of the KiCad netlist as a row of through-hole pads, and each net's pads joined by tracks.
  fixture_board.py <kicad-netlist.net> <out.kicad_pcb>
The netlist is `kicad-cli sch export netlist --format kicadsexpr`."""
import re
import sys


def main():
    source, out = sys.argv[1:]
    text = re.sub(r"\s+", " ", open(source, encoding="utf-8").read())
    nets, pads = {}, {}  # net name -> code; (ref, pin) -> net name
    for block in text.split("(net (code")[1:]:
        name = re.search(r'\(name "([^"]*)"\)', block).group(1)
        nets.setdefault(name, len(nets) + 1)
        for ref, pin in re.findall(r'\(node \(ref "([^"]*)"\) \(pin "([^"]*)"', block):
            pads[ref, pin] = name
    assert pads, "no nets in " + source
    parts = sorted({ref for ref, _ in pads})
    where, footprints = {}, []
    for row, ref in enumerate(parts):
        pins = sorted((pin for r, pin in pads if r == ref), key=lambda p: (len(p), p))
        body = ""
        for column, pin in enumerate(pins):
            net = pads[ref, pin]
            where[ref, pin] = (20 + column * 10, 20 + row * 10)
            body += (f' (pad "{pin}" thru_hole circle (at {column * 10} 0) (size 2 2) (drill 1) '
                     f'(layers "*.Cu" "*.Mask") (net {nets[net]} "{net}"))')
        footprints.append(f'(footprint "Fixture:{ref}" (layer "F.Cu") (at 20 {20 + row * 10}) '
                          f'(property "Reference" "{ref}" (at 0 -3 0) (layer "F.SilkS")){body})')
    tracks = []
    for name, code in nets.items():
        members = [where[key] for key in sorted(where) if pads[key] == name]
        for a, b in zip(members, members[1:]):
            tracks.append(f'(segment (start {a[0]} {a[1]}) (end {b[0]} {b[1]}) (width 0.5) (layer "F.Cu") (net {code}))')
    width, height = 60, 20 + 10 * len(parts) + 10
    declared = "\n".join(f'(net {code} "{name}")' for name, code in nets.items())
    open(out, "w", encoding="utf-8").write(
        f'(kicad_pcb (version 20241229) (generator "corvcad_fixture")\n(general (thickness 1.6))\n(net 0 "")\n{declared}\n'
        f'(gr_rect (start 5 5) (end {width} {height}) (stroke (width 0.1) (type default)) (fill no) (layer "Edge.Cuts"))\n'
        + "\n".join(footprints + tracks) + "\n)\n")
    print(f"{len(parts)} parts, {len(nets)} nets, {len(tracks)} tracks")


if __name__ == "__main__":
    main()
