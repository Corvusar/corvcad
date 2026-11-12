# Showcase video

The feature video is cut to a piece of music: a rendered intro on a real KiCad board, then takes
recorded from the iPad simulator on the iMac, captioned and timed to the beat.

On the iMac, in a work folder with a Python venv (`numpy<2`, `scipy`, `librosa` with
`llvmlite==0.42.0 numba==0.59.1` on Intel, `pillow`, `skia-python`) and FFmpeg:

1. `analyze.py music/<track>.mp3` writes `music/analysis.json`: beats, downbeats, loudness,
   sections.
2. `board.py <board>.kicad_pcb board.json` flattens the board and picks the orb's route: current
   through a chain of parts along their real copper.
3. `intro.py board.json music/analysis.json out/intro.mp4` renders the intro (`--stills t1,t2` for
   review frames). It lands in the iPad frame defined in `layout.py`.
4. Build the `NATIVE_KICAD` and `NATIVE_FREECAD` simulator editions (Debug), then record each take of
   `app/UITests/ShowcaseTests.swift`:
   `record.py testA_SchematicToBoard <DerivedData> takes --seed` (the first take seeds the projects;
   `SHOWCASE_GERBERS=<folder>` stages Gerbers for the toolbox take). Each take writes its MP4 and
   a JSON of its marks.
5. `compose.py music takes out/intro.mp4 out/showcase.mp4` cuts the video (`--stills` or
   `--from/--to` for previews). The edit list at its top maps take spans between marks onto
   downbeats; time-lapsed spans are marked with their speed on screen.

The takes are the app running in the simulator; nothing in them is mocked. The intro and the
finale are rendered.
