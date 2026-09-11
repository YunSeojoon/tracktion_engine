# CoCompose — external AI music editor

CoCompose is a Windows prototype for experienced DAW/VST users composing with an external AI. Edit project data from a script or agent; the open app updates the same Tracktion Edit and timeline in place. There is also a chat panel in the app: attach what you are looking at, ask about it, and apply a suggested change with one press. The model is reached by a separate bridge process, so the API key is never in the app.

Fork: https://github.com/YunSeojoon/tracktion_engine

Upstream: https://github.com/Tracktion/tracktion_engine

Development branch: `ai-editor`

## Run the Windows ZIP

Extract the entire `CoCompose-0.1.0-windows-x64.zip` archive and double-click `CoCompose.exe`. PowerShell, Python and Visual Studio are not required to run the app. Python is optional for the external editing helper. Default projects remain in the Windows Documents folder under `CoCompose`, outside the distribution folder.

To update, close the app and extract the new ZIP to a separate folder. This is an unsigned portable build; an installer and automatic updates are not included. Both the local package and the CI artifact have been extracted and verified: the whole integration suite passes against the packaged executable, and it starts by double-click with no developer tools on PATH.

## Build and run on Windows

Requires Visual Studio 2022 with Desktop development with C++, Windows SDK, CMake 3.22+, Git and the recorded JUCE submodule.

```powershell
git submodule update --init --recursive
cmake -S examples/CoCompose -B build-cocompose -G "Visual Studio 17 2022" -A x64
cmake --build build-cocompose --config Release --target CoCompose --parallel 4
& './build-cocompose/CoCompose_artefacts/Release/CoCompose.exe'
```

To build and package a portable ZIP after initializing submodules, run `./tools/build-windows.ps1` in PowerShell. It invokes CMake and CPack and writes `build-cocompose/dist/CoCompose-0.1.0-windows-x64.zip`. Pass `-Jobs 4` to select build parallelism.

`.github/workflows/cocompose-windows.yml` runs on manual dispatch only — the automatic triggers were removed so that a push never spends CI minutes on its own. It uploads the ZIP as `CoCompose-windows-x64`. A remote run has succeeded and its artifact was downloaded and tested.

`tools/test_portable_start.ps1 -Exe <extracted CoCompose.exe>` checks that an extracted build starts with only the stock Windows directories on PATH and restores the existing default project instead of reseeding the example.

The default session lives in the Windows Documents folder under `CoCompose`. Use `--project 'C:\absolute\song\project.json'` to select a different session folder. Use one folder per song.

Read fresh `state.json`, keep its session ID, revision and object IDs, modify the desired fields, and atomically replace `project.json`. The project file is input-only after initial sample creation; the app never overwrites it. Inspect `sync-status.json` and read `state.json` again to verify the actual engine result. Stale sessions and revisions are rejected. Every array is the complete desired state, so omitted objects are deleted.

## The project model

The document is schema 2 and separates five things:

- `channels` — one instrument each, with `gain_db`, `pan`, `mute`, `solo`, a mixer `insert` number, the `instrument` it plays (`4osc`, `sampler`, or a scanned plugin's identifier), a `sample` path for the sampler, the `step_pitch` and `step_length` its step grid writes, and the `parameters` its plugins expose.
- `patterns` — named note collections. A pattern holds one `sequences` entry per channel that plays in it, each with its own `notes`.
- `playlist.lanes` and `playlist.clips` — a clip is a *placement* of a pattern on a lane at a beat position. Several clips can reference one pattern. A clip's `offset` says how far into the pattern it starts, so a clip can be a slice of a pattern; a clip longer than its pattern repeats it.
- `playlist.audio` — audio clips on a lane, each with its file, `start`, `length`, `offset` into the file, `gain_db`, `fade_in`, `fade_out` and `speed`. A `missing` flag says the file is not where the project last saw it.
- `automation.curves` — one curve per plugin parameter, naming the channel or insert that owns the plugin and holding `points` of `time` (in beats), a normalised `value` and a `curve` shape. `engine_points` reports how many of them the engine is playing.
- `mixer.inserts` — numbered strips that channels are assigned to. Each has a fader, pan, mute, an `output` naming the insert it feeds (or `master`), an ordered `effects` chain and a list of `sends` that tap a copy of it into another insert.

Because a clip only references a pattern, editing that pattern changes every placement of it, and one Undo restores all of them. To change a single placement, copy its pattern under a new id with new note ids and point the clip at the copy (`tools/cocompose.py make-unique`).

The engine MIDI clips are derived from that model, never authored directly. `state.json` reports them under a read-only `engine` key: it is ignored when a request is applied, and it is how a script confirms an edit actually reached playback.

Documents written for the earlier flat `tracks` model, and sessions saved by that build, are converted on load with their track, clip and note ids intact.

UI changes are published to `state.json` and the native session, and every panel writes through the same undo manager an external edit uses. Invalid input is rejected before mutation; live updates preserve the open Edit. Persistence failures are reported as `applied_unpersisted`, not as successful saves. C++ binary hot replacement is not supported.

## The work surface

Browser on the left, Channel Rack above the Mixer in the centre, Pattern picker above the Playlist on the right. Drag the bars between them to resize, and use the View menu (or `Alt+1`...`Alt+5`) to hide and restore a panel; `F6` moves keyboard focus to the next one. Panel sizes, visibility and the current selection are stored in the session, so a reopened project comes back to the same surface.

`Space` starts and stops, `Ctrl+L` switches between Song and Pattern loops, `Ctrl+M` toggles the metronome, `Ctrl+T` adds a channel, `Ctrl+P` a pattern, `Ctrl+B` places the selected pattern, and `Ctrl+U` gives the selected placement its own copy.

The Channel Rack carries a sixteenth-note step grid for the selected pattern: click or drag across it to write notes at the channel's step pitch, and the button beside the insert number sets that pitch and the step length. `Piano roll` opens the note editor for the selected channel and pattern — click to add, drag to move, drag a note's right edge to resize, Alt-drag for velocity, right-click to delete, drag the background to rubber-band select, and use `Ctrl+D` to duplicate, `Q` to quantise and `Delete` to remove. Clicking the keyboard previews through the channel's real instrument.

The Playlist is a bar grid with one row per lane. Drag a pattern from the picker onto a lane to place it, or click an empty spot to drop the selected pattern there. Drag a clip to move it between lanes and bars, drag its right edge to resize, Ctrl-drag to copy, right-click to delete, Alt-drag the background to rubber-band select, and double-click a clip to split it where you clicked. `Ctrl+E` splits at the playhead, `Ctrl+R` duplicates, `Ctrl+U` detaches, `Delete` removes. Drag along the ruler to set the loop range, and Ctrl-scroll to zoom.

Drop a WAV onto a lane — from the Browser or from Explorer — and it becomes an audio clip that moves, resizes, splits and fades like any other. The Browser lists the project's own objects and samples, the folders you add, favourites and recently previewed files, and can preview a sample or go looking for one a clip has lost. `File > Collect samples` copies every sample the project uses into a `samples` folder beside the session, so the folder can be moved whole.

Every channel plays through its numbered insert, an insert plays through whatever it is routed to, and everything reaches the master. `FX` on a strip adds an effect — the six built in (EQ, limiter, saturation, delay, chorus, reverb) or any scanned VST3 — opens its window, bypasses it, moves it along the chain or removes it, and sets up a send; the button under it chooses where the strip is routed. Routing that would feed a signal back into itself is refused. Only saturation had to be written; the rest are the engine's own plugins.

A curve automates any parameter `state.json` reports, and the engine plays it. `Automate`
in the Playlist toolbar lists the selected channel's instrument and fader and the effects
on the insert it plays through; choosing one opens a curve row under the arrangement.
Click an empty spot to add a point, drag it to move it in time and value, right-click to
take one away, and `Delete` removes the selected one. Hold `Ctrl` or `Alt` and drag the
line between two points to bend that segment; the row draws what the engine will play
rather than a straight line between the points. `P` on a channel row saves and recalls
that instrument's settings — a preset only loads onto the same kind of
instrument, and says what it is for when it does not fit — and `Tools`
has the same two as commands. `Tools > Arm channel` points the enabled inputs at the selected channel, `Ctrl+Shift+R` records into the armed channels with an optional bar of count-in, and a take becomes a pattern placed where it was played — so it is edited like anything else. Every way of stopping goes through the same path, so a take is kept whether the app, a shortcut, an outside `stop` or closing the window ended the recording.

The project keeps rolling backups beside the session, written every minute while it changes and immediately after a take is kept or the app closes. `File > Restore a backup...` picks one to open next time, keeping the current session beside it. If the session file is gone or unreadable, the newest backup that opens is loaded instead and `sync-status.json` says which one. That file also lists the backups and names any sample a clip can no longer find. `File > Export WAV` renders the arrangement, or the loop range when one is set over it, and `File > Export stems` renders one file per channel through that channel's own chain. Renders run on their own thread, from a copy of the project taken when the render
started, so the arrangement can keep being edited while one is running. The file it
writes appears only once a whole file exists, so a failed export never costs the last
good one. `render-status.json` reports the result, which revision it rendered, whether
every file was written, and where they are.

## Asking in the app

The AI chat panel talks about the song you have open. `Ctrl+K` attaches the selected bars,
`Ctrl+Shift+K` the notes selected in the piano roll, `Ctrl+Alt+K` the mixer insert. An
attachment is frozen where it was taken: the selection moves on, the attachment does not,
and only its `Update` button re-takes it. `What gets sent` shows exactly what an
attachment carries before it goes anywhere.

The app never talks to a provider. It writes the question beside the project and a
separate bridge picks it up, so the key lives in the bridge's environment and is not in
the app, the project, the conversation or a log:

```powershell
python tools/cocompose_bridge.py --project 'C:\absolute\song\project.json' --provider echo
ollama serve
python tools/cocompose_bridge.py --project 'C:\absolute\song\project.json' --provider ollama --model llama3.1:8b
$env:OPENAI_API_KEY = '<key>'
python tools/cocompose_bridge.py --project 'C:\absolute\song\project.json' --provider openai
```

`echo` is not a model. It answers without a network, says so in every reply, and exists to
check the plumbing. The panel names whichever provider is listening, so an echo answer
cannot be mistaken for a real one. The connection is verified end to end against a model
running on this machine through ollama — `llama3.1:8b`, twice, no failures — by
`python tools/test_ai_connection.py --model llama3.1:8b`, which asserts nothing about the
words and everything about what must hold whatever a model writes: that the answer is
about what was attached, that nothing claims to have heard audio that was never sent, that
a follow-up sees the earlier exchange, that playback continued, and that asking moved no
revision. With no model serving it exits 2 rather than passing. The conversation belongs to the project rather than to
the run, so closing the app, closing the panel or changing provider does not start it over.

An answer may arrive with a change worked out. A model proposes by writing its answer as
usual and putting one fenced ```cocompose-change block at the end — `description`, `keeps`
and the notes it would touch, named by the ids the attachment printed. The bridge lifts
that block out, so the panel shows the sentence rather than the JSON; a missing block, a
malformed one, or one that changes nothing all mean no change rather than a guessed edit.
The change itself is shown as what each note is and would become, and nothing moves until
someone presses `Apply change`. With the piano roll open it is also drawn where the notes
are: an outline where each note would go, a line from a note to where it would move, a
line through one that would go. A filled note exists, an outline does not, and nothing
drawn can be clicked. Since a note change edits the pattern, the panel also says how many
places that pattern is played in, because all of them change. What it may touch comes
from what was attached, what the person said to keep is measured rather than trusted, and
both are checked again at the moment it is applied — so a change worked out against music
that has since moved is refused rather than applied. One Apply is one Undo, whatever it
touched.

Proposals cover notes and parameter values inside an attachment: pitch, start, length,
velocity, adding and removing notes, and a plugin parameter's value. Anything outside the
attachment is refused as `OUT_OF_SCOPE`, and moving clips, making patterns, adding effects
and changing routing are not proposable. Notes are fenced in by the pattern and channel an
attachment names; a parameter change has no equivalent, so an insert must actually be
attached (`Ctrl+Alt+K`) before a proposal may touch it — an empty scope means no insert,
not any insert. A parameter change goes into the same transaction as the notes, so one
Apply is still one Undo even when it moved a fader as well. Whatever proposed it — the echo bridge's
mechanical suggestion or a model's block — goes through the same scope, keeps and revision
checks in the app. A real model has now written one: the local model wrote the block
itself, the app checked it, and Apply moved exactly the four notes it named, kept the
rhythm, and touched nothing outside the attachment. Two limits remain. The hosted
`--provider openai` path has still never been exercised from here; only the local ollama
one has. And a small local model is not reliable at musical judgement — this one read "up
one tone" as one semitone. The app's promises held either way, which is the point; the
musical quality of a suggestion is the model's and is not claimed here.

## Working from outside

`sync-status.json` reports what an applied request actually changed, read back from
the engine rather than echoed from the file: a `change` object with the ids added,
removed and changed in each section, and whether the tempo moved. Check that, not the
file you wrote.

A request that names a stale revision or a finished session is refused and the project
is left alone, so the answer is always to read `state.json` again and redo the edit on
top of what is there. `tools/cocompose.py` does that for you:

```python
from cocompose import apply_change

def busier(state):
    notes = state["patterns"][0]["sequences"][0]["notes"]
    notes.append({"id": "extra", "pitch": 38, "velocity": 90, "start": 2.0, "length": 0.25})

state, _ = apply_change("C:/song/project.json", busier)
```

`apply_change` reads the live state, lets you edit it, submits it, and starts over from
a fresh read if someone edited first. It never forces an old snapshot over a newer one.

One request is one undo, whatever it touched, so a person can hear a change and take it
back in a single step.

A parameter with an automation curve is driven by that curve; the stored value is
written only when nothing is automating it, so an edit and a curve never fight over
the same control. Removing a curve hands the parameter back.

A parameter can also be moved by a MIDI controller. `Automate > MIDI learn` picks the
parameter, the next controller that moves becomes its control, and the mapping is kept
in the project so it comes back with it. `state.json` reports what is mapped under
`automation.midi_mappings`. Dedicated control surfaces are not wired up.

Use the standard-library Python helper while the app is running:

```powershell
python tools/cocompose.py --project 'C:\absolute\song\project.json' inspect
python tools/cocompose.py --project 'C:\absolute\song\project.json' tempo 108
python tools/cocompose.py --project 'C:\absolute\song\project.json' play
```

`tools/cocompose_recipes.py` puts five of the usual call orders over those same tools in one place — `diagnose-region`, `rewrite-melody`, `tidy-velocity`, `review-insert`, and `move-clip`, which reports `UNSUPPORTED` because this build cannot propose that kind of change. A recipe knows nothing about notes and cannot get past a check the app makes; none of them applies itself, and `--apply` is one undo.

It also supports transpose, clear-notes, place, make-unique, gain, parameter, stop, undo, redo and quit. `transpose` and `clear-notes` take a pattern id and therefore change every placement of that pattern. Transport/history commands use `control.json` and request-correlated `control-status.json` acknowledgements.

## Documentation

- [Windows 실행 및 외부 AI 협업 가이드](docs/windows-guide.ko.md)
- [작업 단위와 검증 기록](docs/worklog.ko.md)
- [AI 도구 계약 스키마](docs/ai-tool-contract.schema.json)
- [배포 방식·코드 서명·업데이트 채널 결정](docs/release.ko.md)
- [릴리스 후보](docs/release-candidate.ko.md)
- [이 기계의 VST3 호환성 표](docs/compatibility-2026-09-10.ko.md)
- [수용 검사 기록](docs/acceptance-2026-09-10.ko.md)
- [변경 기록](docs/CHANGELOG.md)

The earlier DemoRunner remains available in `examples/DemoRunner`; CoCompose is now the editor entry point. Windows Release built with MSVC 19.44.35223, and all 28 real-app integration checks passed, the packaged ZIP installs, updates and uninstalls without touching the user's projects (docs/release.ko.md), and every VST3 on the development machine was checked against the real app (docs/compatibility-2026-09-10.ko.md). Run `python tools/test_live_sync.py` with other CoCompose instances closed to repeat them in a new test folder. `python tools/test_ai_chat.py` covers the chat — attachments, a conversation that survives a restart, and proposals being refused, applied and undone — and `python tools/test_tool_contract.py` checks the app's real answers against `docs/ai-tool-contract.schema.json` (it needs the `jsonschema` package). CI runs only when someone asks it to (`gh workflow run cocompose-windows.yml --ref ai-editor`); a push does not trigger it. Details are in the work log. Third-party VST3 compatibility is recorded per plugin for the development machine; listening to the output is a person's judgement and is not claimed. Graph changes may briefly interrupt playback before it resumes on the next UI tick; live sync does not guarantee gapless audio.

Keep upstream license notices intact. Tracktion Engine and JUCE have separate licenses; see the upstream README and JUCE license files.
