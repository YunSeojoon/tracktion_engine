# CoCompose — external AI music editor

CoCompose is a Windows prototype for experienced DAW/VST users composing with an external AI. Edit project data from a script or agent; the open app updates the same Tracktion Edit and timeline in place. In-app chat is deferred.

Fork: https://github.com/YunSeojoon/tracktion_engine

Upstream: https://github.com/Tracktion/tracktion_engine

Development branch: `ai-editor`

## Build and run on Windows

Requires Visual Studio 2022 with Desktop development with C++, Windows SDK, CMake 3.22+, Git and the recorded JUCE submodule.

```powershell
git submodule update --init --recursive
cmake -S examples/CoCompose -B build-cocompose -G "Visual Studio 17 2022" -A x64
cmake --build build-cocompose --config Release --target CoCompose --parallel 4
& './build-cocompose/CoCompose_artefacts/Release/CoCompose.exe'
```

The default session lives in the Windows Documents folder under `CoCompose`. Use `--project 'C:\absolute\song\project.json'` to select a different session folder. Use one folder per song.

Read fresh `state.json`, keep its session ID, revision and object IDs, modify the desired fields, and atomically replace `project.json`. The project file is input-only after initial sample creation; the app never overwrites it. Inspect `sync-status.json` and read `state.json` again to verify the actual engine result. Stale sessions and revisions are rejected. Track/clip/note arrays represent the complete desired MIDI state, so omitted objects are deleted.

Supported data includes MIDI tracks/clips/notes, tempo, gain/mute/solo, and parameters exposed by existing plugins. UI changes are published to `state.json` and the native session. Invalid input is rejected before mutation; live updates preserve the open Edit. Persistence failures are reported as `applied_unpersisted`, not as successful saves. C++ binary hot replacement is not supported.

Use the standard-library Python helper while the app is running:

```powershell
python tools/cocompose.py --project 'C:\absolute\song\project.json' inspect
python tools/cocompose.py --project 'C:\absolute\song\project.json' tempo 108
python tools/cocompose.py --project 'C:\absolute\song\project.json' play
```

It also supports transpose, clear-notes, gain, parameter, stop, undo, redo and quit. Transport/history commands use `control.json` and request-correlated `control-status.json` acknowledgements.

## Documentation

- [Windows 실행 및 외부 AI 협업 가이드](docs/windows-guide.ko.md)
- [작업 단위와 검증 기록](docs/worklog.ko.md)

The earlier DemoRunner remains available in `examples/DemoRunner`; CoCompose is now the editor entry point. Windows Release built with MSVC 19.44.35223, and all 10 real-app integration checks passed. Run `python tools/test_live_sync.py` with other CoCompose instances closed to repeat them in a new test folder. Details are in the work log. Real audio listening and third-party VST3 compatibility remain unverified. Graph changes may briefly interrupt playback before it resumes on the next UI tick; live sync does not guarantee gapless audio.

Keep upstream license notices intact. Tracktion Engine and JUCE have separate licenses; see the upstream README and JUCE license files.
