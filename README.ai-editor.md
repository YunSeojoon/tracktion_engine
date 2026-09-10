# CoCompose — external AI music editor

CoCompose is a Windows prototype for experienced DAW/VST users composing with an external AI. Edit project data from a script or agent; the open app updates the same Tracktion Edit and timeline in place. In-app chat is deferred.

Fork: https://github.com/YunSeojoon/tracktion_engine

Upstream: https://github.com/Tracktion/tracktion_engine

Development branch: `ai-editor`

## Run the Windows ZIP

Extract the entire `CoCompose-0.1.0-windows-x64.zip` archive and double-click `CoCompose.exe`. PowerShell, Python and Visual Studio are not required to run the app. Python is optional for the external editing helper. Default projects remain in the Windows Documents folder under `CoCompose`, outside the distribution folder.

To update, close the app and extract the new ZIP to a separate folder. This is an unsigned portable build; an installer and automatic updates are not included. Both the local package and the CI artifact have been extracted and verified: all 10 integration checks pass against the packaged executable, and it starts by double-click with no developer tools on PATH.

## Build and run on Windows

Requires Visual Studio 2022 with Desktop development with C++, Windows SDK, CMake 3.22+, Git and the recorded JUCE submodule.

```powershell
git submodule update --init --recursive
cmake -S examples/CoCompose -B build-cocompose -G "Visual Studio 17 2022" -A x64
cmake --build build-cocompose --config Release --target CoCompose --parallel 4
& './build-cocompose/CoCompose_artefacts/Release/CoCompose.exe'
```

To build and package a portable ZIP after initializing submodules, run `./tools/build-windows.ps1` in PowerShell. It invokes CMake and CPack and writes `build-cocompose/dist/CoCompose-0.1.0-windows-x64.zip`. Pass `-Jobs 4` to select build parallelism.

`.github/workflows/cocompose-windows.yml` builds on relevant `ai-editor` pushes, pull requests targeting that branch, and manual dispatch. It uploads the ZIP as `CoCompose-windows-x64`. A remote run has succeeded and its artifact was downloaded and tested.

`tools/test_portable_start.ps1 -Exe <extracted CoCompose.exe>` checks that an extracted build starts with only the stock Windows directories on PATH and restores the existing default project instead of reseeding the example.

The default session lives in the Windows Documents folder under `CoCompose`. Use `--project 'C:\absolute\song\project.json'` to select a different session folder. Use one folder per song.

Read fresh `state.json`, keep its session ID, revision and object IDs, modify the desired fields, and atomically replace `project.json`. The project file is input-only after initial sample creation; the app never overwrites it. Inspect `sync-status.json` and read `state.json` again to verify the actual engine result. Stale sessions and revisions are rejected. Every array is the complete desired state, so omitted objects are deleted.

## The project model

The document is schema 2 and separates five things:

- `channels` — one instrument each, with `gain_db`, `pan`, `mute`, `solo`, a mixer `insert` number, the `instrument` it plays (`4osc`, `sampler`, or a scanned plugin's identifier), a `sample` path for the sampler, the `step_pitch` and `step_length` its step grid writes, and the `parameters` its plugins expose.
- `patterns` — named note collections. A pattern holds one `sequences` entry per channel that plays in it, each with its own `notes`.
- `playlist.lanes` and `playlist.clips` — a clip is a *placement* of a pattern on a lane at a beat position. Several clips can reference one pattern. A clip's `offset` says how far into the pattern it starts, so a clip can be a slice of a pattern; a clip longer than its pattern repeats it.
- `mixer.inserts` — numbered strips that channels are assigned to. Modelled and saved, but the audio still runs from each channel to the master; routing arrives with the mixer work.

Because a clip only references a pattern, editing that pattern changes every placement of it, and one Undo restores all of them. To change a single placement, copy its pattern under a new id with new note ids and point the clip at the copy (`tools/cocompose.py make-unique`).

The engine MIDI clips are derived from that model, never authored directly. `state.json` reports them under a read-only `engine` key: it is ignored when a request is applied, and it is how a script confirms an edit actually reached playback.

Documents written for the earlier flat `tracks` model, and sessions saved by that build, are converted on load with their track, clip and note ids intact.

UI changes are published to `state.json` and the native session, and every panel writes through the same undo manager an external edit uses. Invalid input is rejected before mutation; live updates preserve the open Edit. Persistence failures are reported as `applied_unpersisted`, not as successful saves. C++ binary hot replacement is not supported.

## The work surface

Browser on the left, Channel Rack above the Mixer in the centre, Pattern picker above the Playlist on the right. Drag the bars between them to resize, and use the View menu (or `Alt+1`...`Alt+5`) to hide and restore a panel; `F6` moves keyboard focus to the next one. Panel sizes, visibility and the current selection are stored in the session, so a reopened project comes back to the same surface.

`Space` starts and stops, `Ctrl+L` switches between Song and Pattern loops, `Ctrl+M` toggles the metronome, `Ctrl+T` adds a channel, `Ctrl+P` a pattern, `Ctrl+B` places the selected pattern, and `Ctrl+U` gives the selected placement its own copy.

The Channel Rack carries a sixteenth-note step grid for the selected pattern: click or drag across it to write notes at the channel's step pitch, and the button beside the insert number sets that pitch and the step length. `Piano roll` opens the note editor for the selected channel and pattern — click to add, drag to move, drag a note's right edge to resize, Alt-drag for velocity, right-click to delete, drag the background to rubber-band select, and use `Ctrl+D` to duplicate, `Q` to quantise and `Delete` to remove. Clicking the keyboard previews through the channel's real instrument.

The Playlist is a bar grid with one row per lane. Drag a pattern from the picker onto a lane to place it, or click an empty spot to drop the selected pattern there. Drag a clip to move it between lanes and bars, drag its right edge to resize, Ctrl-drag to copy, right-click to delete, Alt-drag the background to rubber-band select, and double-click a clip to split it where you clicked. `Ctrl+E` splits at the playhead, `Ctrl+R` duplicates, `Ctrl+U` detaches, `Delete` removes. Drag along the ruler to set the loop range, and Ctrl-scroll to zoom.

The sample browser and mixer routing with effects are not built yet.

Use the standard-library Python helper while the app is running:

```powershell
python tools/cocompose.py --project 'C:\absolute\song\project.json' inspect
python tools/cocompose.py --project 'C:\absolute\song\project.json' tempo 108
python tools/cocompose.py --project 'C:\absolute\song\project.json' play
```

It also supports transpose, clear-notes, place, make-unique, gain, parameter, stop, undo, redo and quit. `transpose` and `clear-notes` take a pattern id and therefore change every placement of that pattern. Transport/history commands use `control.json` and request-correlated `control-status.json` acknowledgements.

## Documentation

- [Windows 실행 및 외부 AI 협업 가이드](docs/windows-guide.ko.md)
- [작업 단위와 검증 기록](docs/worklog.ko.md)

The earlier DemoRunner remains available in `examples/DemoRunner`; CoCompose is now the editor entry point. Windows Release built with MSVC 19.44.35223, and all 17 real-app integration checks passed. Run `python tools/test_live_sync.py` with other CoCompose instances closed to repeat them in a new test folder. Details are in the work log. Real audio listening and third-party VST3 compatibility remain unverified. Graph changes may briefly interrupt playback before it resumes on the next UI tick; live sync does not guarantee gapless audio.

Keep upstream license notices intact. Tracktion Engine and JUCE have separate licenses; see the upstream README and JUCE license files.
