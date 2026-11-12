# Create a KiCad project for CorvCAD round-trip testing

Use KiCad 10 if available. Save the entire project folder so the `.kicad_pro`, `.kicad_sch`, and `.kicad_pcb` files stay together. This example is a compatibility test, not a circuit intended for manufacture.

1. In KiCad, choose **File → New Project**, name it `corvcad-interop`, and let KiCad create a new folder.
2. Open **Schematic Editor**. Add (`A`) these symbols: `Connector_Generic:Conn_01x02` as `J1`, `Device:R` as `R1`, and `Device:LED` as `D1`. Set `R1`'s value to `1k` and `D1`'s value to `LED` in each symbol's properties (`E`).
3. Wire (`W`) a single series loop: `J1` pin 1 → one end of `R1` → the LED anode (`A`); then LED cathode (`K`) → `J1` pin 2. Use separate wire segments if needed, but make each endpoint snap to a pin. Add a net label `SIGNAL` to the wire between the resistor and LED. Save.
4. In symbol properties, set these footprint IDs exactly:
   - `J1`: `Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical`
   - `R1`: `Resistor_SMD:R_0805_2012Metric`
   - `D1`: `LED_SMD:LED_0805_2012Metric`
5. Run **Inspect → Electrical Rules Checker**. Save or screenshot the results; fix accidental disconnected pins, but keep the report if you are unsure about a warning. Do not disable a rule to hide it.
6. Open **PCB Editor**, then **Tools → Update PCB from Schematic** (`F8`). Accept the update, place the three footprints, and draw one rectangle on `Edge.Cuts` around them (roughly 40 × 25 mm, with comfortable space between parts and edge). Save.
7. Route (`X`) the three connection groups with front copper tracks. Add a `MountingHole:MountingHole_3.2mm_M3` footprint inside the outline, away from the edge and traces. Save. A copper zone or via is optional for this first file; if you add one, fill zones with `B` before saving.
8. Run **Inspect → Design Rules Checker** and save or screenshot its report. If it reports unconnected pads, check the three tracks and zone fill. An imperfect first attempt is useful test data, so keep the file and report.
9. Close KiCad, zip the complete project folder, and copy the zip to this PC or attach it in the chat. Include the ERC/DRC reports or screenshots if available. Exclude backups if the zip becomes large. CorvCAD will compare its parsed model with KiCad's own board model and validate edited copies; the original project will remain untouched.

## Touch-authored round trip (the other direction)

`engine/tests/fixtures/roundtrip` is a project CorvCAD built entirely by touch, in the compatible (default) edition. `app/UITests/RoundTripTests.swift` creates it from the home screen. On the schematic it places three parts and rotates R3. It draws one wire the router detours around both part bodies, a second wire, and a third wire ending inside the second, which writes a tee junction. Two `NET1` labels join R3 pin 2 to R1 pin 1. On the board it places three footprints, rotates R3, and adds two M3 holes. The **Touch-authored round-trip project** workflow runs that test in a GitHub-hosted iPad simulator and pushes the project to the `ci/round-trip` branch.

Check a project with KiCad 10 on a desktop:

```sh
python integrations/kicad_cli/round_trip.py engine/tests/fixtures/roundtrip build/round-trip
```

The script runs ERC and DRC, re-saves both files with KiCad's writer (`kicad-cli ... upgrade --force`) in a copy, and requires the same netlist, footprint positions and violations afterwards. The netlist must also match `expected.json` exactly. With KiCad 10.0.6, the first run (2026-10-22, iOS 26.5 simulator, commit `c843020`) gives:

- no ERC or DRC violations;
- exactly the nets {R1.1, R2.2, R3.2} and {R1.2, R2.1, R3.1};
- an unchanged re-save.

The detour runs at y = 48.26 mm and crosses R3's vertical wire with no junction. KiCad correctly keeps those two nets apart.

`integrations/kicad_native/bridge.py audit` loads the board with KiCad's own model. The footprints, references, positions and the rotated R3 match the schematic. Right after authoring, it reports `consistent=False` because the six resistor pads carry no nets. The holes H1 and H2 exist only on the board, as intended, which the audit allows. The test then taps **Update from schematic**. On the run for commit `e5a755a` (2026-10-23), that left:

- the audit reporting `consistent=True`;
- DRC with `--schematic-parity` showing no violations and no parity differences;
- only 4 unconnected items, the expected ratsnest of an unrouted board.

The test then routes both nets with **Track** and **Via**. One leg of `/NET1` crosses through B.Cu between two vias. On the run for commit `ca7e28e`, KiCad 10.0.6 DRC with `--schematic-parity --severity-all` reported 0 violations, 0 unconnected items and 0 parity differences. The audit was still consistent, and `round_trip.py` still passed.

The committed fixture keeps the board from before the update, because the engine tests start from it.

This run has not yet been repeated on a physical iPad or with the `NATIVE_KICAD` edition.

The [official KiCad getting-started guide](https://docs.kicad.org/10.0/en/getting_started_in_kicad/getting_started_in_kicad.pdf) covers the same schematic → footprint → PCB workflow in more detail.
