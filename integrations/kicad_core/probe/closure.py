#!/usr/bin/env python3
"""Replay static-archive resolution for the probe and explain why each member loads.

Reads defined/undefined global symbols of the probe object and every staged archive member
(via nm), then pulls members the way ld does: a member is loaded when it defines a symbol
some already-loaded object still needs. Prints the measured closure, the unresolved
frontier, and the chain that pulled in any member you ask about.

  closure.py <build-dir> [--probe LIB] [--stage DIR ...] [--why MEMBER ...] [--exclude REGEX]
             [--cut MEMBER_REGEX:SYMBOL_REGEX ...] [--edges] [--frontier]
  Board probe: closure.py build/kicad-probe --probe pcb_probe_main --stage stage_pcb stage
"""
import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path


# Editor UI translation units (flattened member names); the iPad slice must not need them.
GUI = re.compile(
    r"^(?!generated__|.*headless)(?:.*)(__dialogs__|__widgets__(?!report_severity)|__tool__|__tools__|__gal__(opengl|cairo)__|__preview_items__|__dialog_about__|"
    r"__pcm__|__symbol_editor__(?!symbol_editor_settings)|__libview__|__printing__|__sync_sheet_pin__|"
    r"__footprint_editor__|_frame\.|frame_|__panel_|draw_panel|preview_panel|kiway_player|"
    r"dialog_shim|grid_tricks|hotkey|menubar|toolbars_|status_popup|cross-probing|scintilla|textentry_tricks|"
    r"lib_table_notebook|lib_table_grid|kidialog|confirm\.|validators|lib_tree_model|symbol_tree|picksymbol|"
    r"net_navigator|fields_grid_table|fields_data_model|__view__(view_controls|wx_view_controls|zoom_controller)\.|__properties__pg_|background_jobs_monitor|notifications_manager|"
    r"eda_dde|"
    # eeschema files that implement SCH_EDIT_FRAME / SCH_BASE_FRAME members or editor flows.
    r"eeschema__(annotate|bus-wire-junction|eeschema_config|files-io|sheet|schematic_undo_redo|sch_view|"
    r"sch_design_block_utils|remote_symbol_[a-z_]+|project_rescue|symbol_library_manager|sim__simulator_)\.)")


def symbols(obj: Path):
    out = subprocess.run(["nm", "-g", str(obj)], capture_output=True, text=True).stdout
    defined, undefined = set(), set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == "U":
            undefined.add(parts[1])
        elif len(parts) == 3 and parts[1] not in ("U",):
            defined.add(parts[2])
    return defined, undefined


def demangle(names):
    if not names:
        return []
    out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout
    return out.splitlines()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--probe", default="probe_main", help="probe object library")
    parser.add_argument("--stage", nargs="*", default=["stage"], help="staged archive members, in link order")
    parser.add_argument("--why", nargs="*", default=[])
    parser.add_argument("--exclude", help="regex of members to treat as absent")
    parser.add_argument("--frontier", action="store_true", help="list unresolved symbols")
    parser.add_argument("--cut", nargs="*", default=[],
                        help="simulate a patch: members matching MEMBER_REGEX no longer reference "
                             "symbols whose demangled name matches SYMBOL_REGEX")
    parser.add_argument("--edges", action="store_true",
                        help="list every reference from a loaded non-GUI member into a GUI member")
    parser.add_argument("--root", nargs="*", default=[],
                        help="members loaded like the probe (linked as objects, e.g. self-registering "
                             "DRC providers and board generators); regexes over member names")
    args = parser.parse_args()

    probe = next(args.build.glob(f"CMakeFiles/{args.probe}.dir/**/*.cpp.o"))
    members = [m for stage in args.stage for m in sorted((args.build / stage).glob("*.o"))]
    if args.exclude:
        members = [m for m in members if not re.search(args.exclude, m.name)]
    table = {m.name[:-2]: symbols(m) for m in members}
    provider = {}
    for name, (defined, _) in table.items():
        for sym in defined:
            provider.setdefault(sym, name)
    for rule in args.cut:
        member_re, symbol_re = rule.split(":", 1)
        for name in [n for n in table if re.search(member_re, n)]:
            defined, undefined = table[name]
            names = sorted(undefined)
            kept = {s for s, pretty in zip(names, demangle(names)) if not re.search(symbol_re, pretty)}
            table[name] = (defined, kept)

    pdef, pundef = symbols(probe)
    need = collections.deque((s, "probe.cpp") for s in sorted(pundef))
    have = set(pdef)
    unresolved = collections.defaultdict(set)
    loaded, parent = {}, {}
    for name in [n for n in table if any(re.search(r, n) for r in args.root)]:
        loaded[name], parent[name] = "(root)", "probe.cpp"
        have |= table[name][0]
        need.extend((s, name) for s in sorted(table[name][1]))
    while need:
        sym, by = need.popleft()
        if sym in have:
            continue
        member = provider.get(sym)
        if member is None:
            unresolved[sym].add(by)
            continue
        if member not in loaded:
            loaded[member] = sym
            parent[member] = by
            defined, undefined = table[member]
            have |= defined
            need.extend((s, member) for s in sorted(undefined))
        have.add(sym)

    print(f"closure: {len(loaded)} of {len(table)} members")
    for member in args.why:
        chain, node = [], member
        while node in parent:
            chain.append(f"{node}  [for {demangle([loaded[node]])[0][:90]}]")
            node = parent[node]
        print("\n".join(["why " + member + ":"] + ["   <- " + c for c in chain] + ["   <- probe.cpp"])
              if chain else f"{member}: not loaded")
    if args.frontier:
        # Symbols nothing in the slice defines; most come from external libraries.
        names = sorted(unresolved)
        for name, pretty in zip(names, demangle(names)):
            print(f"{pretty[:110]}  <- {sorted(unresolved[name])[0]}")
    if args.edges:
        # The patch list: where model code first reaches GUI code.
        gui = [m for m in loaded if GUI.search(m)]
        print(f"gui members loaded: {len(gui)}")
        edges = collections.defaultdict(set)
        for member in loaded:
            if GUI.search(member):
                continue
            for sym in table[member][1]:
                target = provider.get(sym)
                if target in loaded and GUI.search(target):
                    edges[member].add(sym)
        for member in sorted(edges):
            names = sorted(edges[member])
            print(f"{member}:")
            for pretty in sorted(set(p[:110] for p in demangle(names))):
                print(f"    {pretty}")
    (args.build / f"{args.probe}.closure_model.txt").write_text("\n".join(sorted(loaded)) + "\n")


if __name__ == "__main__":
    sys.exit(main())
