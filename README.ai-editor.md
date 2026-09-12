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

`Space` starts and stops, `Home` goes back to the beginning, `Ctrl+L` switches between Song and Pattern loops, `Ctrl+M` toggles the metronome, `Ctrl+T` adds a channel, `Ctrl+P` a pattern, `Ctrl+B` places the selected pattern, `Ctrl+U` gives the selected placement its own copy, and `Enter` opens the selected clip's notes. The full list is kept by the app rather than by this file: every command, its category and its keys are written to `shortcuts.json` beside the project at startup, generated from the commands themselves so it cannot disagree with what is bound — 46 of them today. A check reads that file and refuses two commands on one key, because one of them would silently never run and which one survives depends on registration order. Stop is its own command rather than half of the Play/Stop toggle: pressing it while already stopped returns to where playing last began. Space, Return and Home are the three commands bound to a bare key, and all three stand down while a text box has the keyboard, so a space typed into the chat box is a space, a Return is a new line and Home is the start of the line — only Space used to, which is why Return in the middle of a question opened the piano roll. The rule asks whether the command came from a key press, so a menu item clicked while a name is selected in the arrangement still runs.

The Channel Rack carries a sixteenth-note step grid for the selected pattern: click or drag across it to write notes at the channel's step pitch, and the button beside the insert number sets that pitch and the step length. Four things open the note editor: double-clicking a clip in the Playlist, `Enter` with a clip selected, `Open in the piano roll` in a clip's menu, and `Piano roll` in the Channel Rack. The first three follow the music — if the selected channel has no part in the pattern being opened, one the pattern does play is shown instead, so a drum clip does not present the bass's empty grid. Opening is not an edit: no revision, no undo. The editor's first line says which pattern, which channel, and in how many places that pattern is played (`played in 2 places, and editing changes all of them`), and it keeps saying it while the window is open. Select and Draw are separate tools and **only Draw writes notes**; in Select, dragging empty grid is a rubber-band selection rather than a note plus a selection. Drag a note to move it, drag its right edge to resize, Alt-drag for velocity, and use `Ctrl+D` to duplicate, `Q` to quantise and `Delete` to remove. Right-clicking a note opens a menu — semitone and octave up and down, quantise, ask the AI about it, delete — rather than deleting it where it stands. Clicking the keyboard previews through the channel's real instrument, and a held preview stops if the window loses the keyboard rather than sounding forever.

The Playlist is a bar grid with one row per lane. Drag a pattern from the picker onto a lane to place it. It has four tools — Select, Draw, Erase and Split — and **Select is the default**: clicking an empty spot drops the selected pattern only when Draw is chosen, and dragging empty space is a selection. Erase and Split act on the clip they are pointed at the moment they are clicked, which is safe precisely because choosing them is a deliberate act, and the cursor changes with the tool so you can see which one you are holding before you press. The toolbar box and the empty-grid menu set each other, so they cannot disagree. Drag a clip to move it between lanes and bars, drag its right edge to resize, Ctrl-drag to copy, hold Alt to suspend snapping while you drag, and double-click a clip to open its notes — it used to split there, which `Ctrl+E` and the clip menu still do. Alt means velocity in the note editor and snap-off here; the two windows differ on purpose rather than by oversight. `Ctrl+E` splits at the playhead, `Ctrl+R` duplicates, `Ctrl+U` detaches, `Delete` removes. Right-clicking a clip opens a menu — open in the piano roll, duplicate, make unique, split, ask the AI, delete — instead of deleting it outright; right-clicking inside a selection keeps the selection and one command covers all of it in one undo, and right-clicking outside one moves to what was pointed at. Menus exist for pattern clips, notes, the empty grid, the channel header, an effect in a mixer chain and audio clips, and knobs and faders have a value menu of their own; lane headers do not have one yet. Opening a menu is not an edit anywhere: a mis-aimed right-click moves no clip, no note, and no revision.

The ruler does three things. Left-click seeks to where you clicked, at a free position rather than the editing grid, and dragging shows where the playhead would land and seeks once when you let go. Dragging a loop handle moves that end of the loop and nothing else. Shift-drag marks a stretch of time, which is neither the loop nor a set of clips but a third thing meaning "this part of the song" — it is drawn in its own colour, and it is what the chat attaches when nothing else is picked out. Marking a range and selecting clips clear each other. Seeking touches the transport only: it moves no revision and spends no undo. Both grids share one wheel contract: scroll for vertical, Shift-scroll for horizontal, Ctrl-scroll to zoom **about the pointer**, so what you were looking at stays where it was. Over the Playlist's lane names, Ctrl-scroll changes how tall the lanes are instead (16 to 96 pixels, clamped rather than obeyed) — the pointer is on the thing being resized, which is the rest of the contract's rule too. Lane height lives in the view and not in the project: it is how somebody is looking at the music, so changing it moves no revision, and a check confirms that. `Fit whole song`, `Fit selection` and `Back to previous zoom` are in the View menu, and `Piano roll` is in Edit; all four are still callable by name (`--ui-script`'s `{"command": "Fit whole song"}`). The middle button pans, and dragging a clip or a selection box scrolls the view when the pointer nears an edge — without that a clip cannot be moved further than one windowful and a selection stops at the border.

Drop a WAV onto a lane — from the Browser or from Explorer — and it becomes an audio clip that moves, resizes, splits and fades like any other. Right-clicking one offers what audio has — level, fade in, fade out, speed, and back to how it was recorded — beside the duplicate, split, ask and delete it shares with a pattern clip; it used to offer a pattern clip's menu, where `Open in the piano roll` and `Make unique` mean nothing on a recording. The Browser lists the project's own objects and samples, the folders you add, favourites and recently previewed files, and can preview a sample or go looking for one a clip has lost. `File > Collect samples` copies every sample the project uses into a `samples` folder beside the session, so the folder can be moved whole.

Every channel plays through its numbered insert, an insert plays through whatever it is routed to, and everything reaches the master. `FX` on a strip adds an effect — the six built in (EQ, limiter, saturation, delay, chorus, reverb) or any scanned VST3 — opens its window, bypasses it, moves it along the chain or removes it, and sets up a send; the button under it chooses where the strip is routed. Routing that would feed a signal back into itself is refused. Only saturation had to be written; the rest are the engine's own plugins. The chain a strip draws in order (`eq > delay > reverb`) is something you can point at: the name under the pointer highlights, and double-clicking it opens that plugin's window through the same call the menu's `Open` makes — one door, not two. Right-clicking an effect in that chain offers open, bypass, move up, move down and remove, sending the same items the `FX` menu does, and right-clicking the empty part of a chain offers the whole chain's menu, which is where `Add effect` lives — so an empty chain answers too. The channel header has a menu as well: rename, instrument, its window, a sample, presets, step settings, mute, solo, asking about the insert it feeds, and removing the channel, every item driving a control already on the row or a command already in the menu bar, so the two cannot come to disagree. Knobs and faders — the rack's gain and pan, the mixer's fader and pan — double-click back to their default and right-click for a typed number; a box left empty or holding something that is not a number does nothing rather than dropping to zero. Selecting a channel also selects the insert it feeds, so the mixer is already looking at what was picked.

A curve automates any parameter `state.json` reports, and the engine plays it. `Automate`
in the Playlist toolbar lists the selected channel's instrument and fader and the effects
on the insert it plays through; choosing one opens a curve row under the arrangement.
Click an empty spot to add a point, drag it to move it in time and value, and `Delete`
removes the selected one. Right-click no longer takes a point away — the last place that
still destroyed on right-click. Hold `Ctrl` or `Alt` and drag the
line between two points to bend that segment; the row draws what the engine will play
rather than a straight line between the points. `P` on a channel row saves and recalls
that instrument's settings — a preset only loads onto the same kind of
instrument, and says what it is for when it does not fit — and `Tools`
has the same two as commands. `Tools > Arm channel` points the enabled inputs at the selected channel, `Ctrl+Shift+R` records into the armed channels with an optional bar of count-in, and a take becomes a pattern placed where it was played — so it is edited like anything else. Every way of stopping goes through the same path, so a take is kept whether the app, a shortcut, an outside `stop` or closing the window ended the recording.

The project keeps rolling backups beside the session, written every minute while it changes and immediately after a take is kept or the app closes. `File > Restore a backup...` picks one to open next time, keeping the current session beside it. If the session file is gone or unreadable, the newest backup that opens is loaded instead and `sync-status.json` says which one. That file also lists the backups and names any sample a clip can no longer find. `File > Export WAV` renders the arrangement, or the loop range when one is set over it, and `File > Export stems` renders one file per channel through that channel's own chain. Renders run on their own thread, from a copy of the project taken when the render
started, so the arrangement can keep being edited while one is running. The file it
writes appears only once a whole file exists, so a failed export never costs the last
good one. `render-status.json` reports the result, which revision it rendered, whether
every file was written, and where they are. That copy is also what makes an A/B cheap:
a preview is the same copy with a proposal applied to it before the render opens it, so
the two files differ by the change and by nothing else, and the song pays nothing for
either of them.

## Asking in the app

The AI chat panel talks about the song you have open. `Ctrl+K` attaches a stretch of the
arrangement — the selected clips if there are any, otherwise the range marked on the ruler,
otherwise the loop, which comes last because a loop may have been sitting there since
yesterday. `Ctrl+Shift+K` attaches the notes selected in the piano roll, `Ctrl+Alt+K` the
mixer insert. An attachment is frozen where it was taken: the selection moves on, the
attachment does not, and only its `Update` button re-takes it. A region carries the notes
played in it, not just clip names and lengths — two clips with the same name and length can
be a bass line and a cluster chord — on a note budget the region proper spends before its
surrounding context, with anything left out counted rather than dropped in silence. An
insert carries its sends and says how many parameters it did not name. `What gets sent`
shows exactly what an attachment carries before it goes anywhere.

A follow-up question carries what the earlier ones were about: the kind, ids and beat range
of each earlier attachment travel with the history, along with the revision they were read
at, so "make that part less busy" has a referent and a target the music has moved past is
described as it was rather than asserted to be that way still.

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
cannot be mistaken for a real one. The bridge writes a heartbeat every couple of seconds,
including while a provider call is in flight, so a bridge that was killed rather than
closed is seen as gone instead of believed: a question left waiting on one ends, says so,
and comes back into the box with its text intact. A bridge restarted in a folder where a
question is still sitting does not ask it again — the reply beside the request is the
record and outlives the process — and an answer cut off mid-stream is reported rather than
quietly retried, since whether the provider did the work and charged for it cannot be known
from here. The connection is verified end to end against a model
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
Apply is still one Undo even when it moved a fader as well. A proposal is measured
against the revision the question was asked at rather than the one the answer arrived
at, so an answer that comes back after the person has edited the same notes is
`STALE_REVISION` instead of quietly overwriting them. The scope is rebuilt from what
was attached and the scope fields a reply writes for itself are stripped before
anything reads them; an empty allowed list means nothing rather than everything, a note
change with nothing attached is refused, and two attachments from different patterns no
longer merge their ids under whichever came last. An added note's defaults are decided
once, where the change is read, so the number shown, the number checked and the number
written are the same one; a verb the service does not know is refused rather than read
as "change", and pitch and velocity have to arrive as numbers — `"abc"` used to become
0 and `60.7` used to become 60. Whatever proposed it — the echo bridge's
mechanical suggestion or a model's block — goes through the same scope, keeps and revision
checks in the app. A real model has now written one: the local model wrote the block
itself, the app checked it, and Apply moved exactly the four notes it named, kept the
rhythm, and touched nothing outside the attachment.

A proposal can also be heard before it is taken. `--ui-script`'s `{"preview": [proposal,
from, to]}` renders that stretch twice — once as it is, once as the proposal would make
it — over the same range and through the same mix path, and writes `preview-before.wav`,
`preview-after.wav` and `preview-status.json` beside the project. Nothing reaches the
live song: the proposal is still unapplied afterwards and the revision has not moved,
because what was rendered was a copy. The status file carries each file's fingerprint
(FNV-1a over the bytes — it tells two renders apart, it does not seal them; the release
artefacts still use SHA-256), the revision it came from and the beats it covers, so a
preview left over from before an edit is visibly not what a fresh render would produce.
A proposal that also moves mixer parameters is previewed for its notes only and says so,
because a comparison that quietly left half the change out would be worse than none. And
the file says `heard: false` and why: making a file is not listening to it, and whether
the change sounds better is not something this app or a model with no ears can report.
`python tools/test_preview.py` checks everything around that judgement and prints the
judgement itself under `FOR A PERSON`. Both halves are now derived the same way — only
the second one used to re-derive its clips from the adjusted copy, so "as it is" was
rendered from whatever the live edit happened to be holding — and the check compares the
notes the engine would play rather than the file bytes: renders on this machine are not
bit-deterministic, two renders of the same music do not always hash the same, so "the
files differ" was passing on noise while the preview rendered the old notes twice. The
proof a preview offers is at the level of those notes, not of bytes. There is no button
for this yet.

What is known about a project is kept in three kinds that never merge. A condition is a
person's rule ("keep the drums as they are"). A guess is the assistant's reading ("this
sounds like D minor"), which carries who said it and which revision it was read from. A
request is what is being asked right now and expires with the answer. A guess never
becomes a rule on its own: only a person promotes one, and the guess stays where it was,
because what was read and what was agreed are different facts. The prompt labels all
three, since a model shown its own earlier guess unlabelled reads it as something it was
told. None of it is music — writing a note moves no revision and enters no undo history,
because somebody undoing an edit is undoing an edit, not forgetting a decision. Notes
live in `notes.json` beside the project and belong to a project id, so a copied folder
inherits nothing. `--ui-script`'s `{"project_note": ["condition"|"guess"|"request"|
"accept"|"remove", text or id]}` is the only way in so far; `chat-inspector.json` reports
what is there.

Two limits remain. The hosted
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

It also supports transpose, clear-notes, place, make-unique, gain, parameter, stop, undo, redo and quit. `transpose` and `clear-notes` take a pattern id and therefore change every placement of that pattern. Transport/history commands use `control.json` and request-correlated `control-status.json` acknowledgements. `quit` is acknowledged **before** it is carried out and every other command after: asking the app to quit may not leave an afterwards, and a caller told nothing waits out its timeout and concludes the app died — which was making anything that closes this app by script report a hang that had not happened.

## Documentation

- [Windows 실행 및 외부 AI 협업 가이드](docs/windows-guide.ko.md)
- [작업 단위와 검증 기록](docs/worklog.ko.md)
- [AI 도구 계약 스키마](docs/ai-tool-contract.schema.json)
- [배포 방식·코드 서명·업데이트 채널 결정](docs/release.ko.md)
- [릴리스 후보](docs/release-candidate.ko.md)
- [이 기계의 VST3 호환성 표](docs/compatibility-2026-09-10.ko.md)
- [수용 검사 기록](docs/acceptance-2026-09-10.ko.md)
- [변경 기록](docs/CHANGELOG.md)

The earlier DemoRunner remains available in `examples/DemoRunner`; CoCompose is now the editor entry point. Windows Release built with MSVC 19.44.35223, and all 28 real-app integration checks passed, the packaged ZIP installs, updates and uninstalls without touching the user's projects (docs/release.ko.md), and every VST3 on the development machine was checked against the real app (docs/compatibility-2026-09-10.ko.md). Run `python tools/test_live_sync.py` with other CoCompose instances closed to repeat them in a new test folder. `python tools/test_ai_chat.py` covers the chat — attachments, a conversation that survives a restart, and proposals being refused, applied and undone — and `python tools/test_tool_contract.py` checks the app's real answers against `docs/ai-tool-contract.schema.json` (it needs the `jsonschema` package). `python tools/test_chat_reliability.py` covers the connection lying about itself — a restart that must not re-ask, an interrupted answer that must not be resent, a dead bridge that must not look alive, a question that must come back, an earlier attachment that must survive in the history, and two regions with identical labels that must read differently — and needs neither a key nor a network, three of the four using a provider spy and the fourth reading what the app writes. `python tools/test_daw_interaction.py` drives the real mouse handlers with real `MouseEvent`s for the ruler gestures, the stop-twice return and right-click opening a menu instead of deleting, and also reads `shortcuts.json` to refuse two commands on one key and checks that lane height clamps and moves no revision; it drives real right-button events into the channel header, an effect slot and an audio clip too, holding each to the same rule — opening a menu is not an edit — because calling the function that opens a menu says the menu exists and nothing about whether right-clicking reaches it; where a menu appears and how it reads is left as a person's job. One thing it cannot produce on this machine is a script-owned keyboard focus, so "Space, Return and Home while typing do not run DAW commands" is printed under `NOT CHECKED HERE` rather than counted as a pass — a headless window never gets OS keyboard focus, so type in the chat box and press them to see it. `python tools/test_proposal_boundaries.py` holds the lines a proposal must not cross: a late reply that cannot overwrite what the person changed meanwhile, a reply that cannot name its own scope, a reply with nothing attached that changes nothing, an empty allowed list read as none rather than all, an added note that has to fit the pattern it is added to, an unknown verb and a non-number refused, and a reply for another project ignored. The fourth review's own reproduction tool, `tools/review_proposal_repro.py`, is kept as well and now answers `STALE_REVISION`, `OUT_OF_SCOPE` and a 0.5-beat note in a 0.5-beat pattern where it used to reproduce the bugs. `python tools/test_preview.py` covers the A/B — two real files, different audio, each saying which revision and which beats it came from, and a song that did not move — with the listening itself printed under `FOR A PERSON`, and `python tools/test_project_notes.py` covers the notes: a guess that is not a rule, a promotion that keeps both, a decision that survives a restart while a finished request does not, and a copied project that inherits nothing. A timeout in these checks now says how long it waited out of how long it was allowed, what it was waiting for, and whether it was swallowing an exception the whole time; three timing assumptions in the checks themselves and two app bugs have been fixed. The second of those was the intermittent one: a control request sent the moment a new session answered was marked already-seen while the app was still starting, which cost roughly one live-sync run in three and arrived as a hang that had not happened. What separates a leftover request from a live one is which session it names, so that is what decides now. The harness also stops lying about being blocked — the wait for a free single instance used to give up silently and every caller ignored it, so another copy of the app holding the lock surfaced as a failing check somewhere unrelated; it now raises and names the process in the way, `Session.open` retries a launch the single-instance mutex refused (the mutex can outlive the process that held it), and a failed launch leaves no app behind. Those suites were last run together on one binary, sha256 `347ba03c794f209f04ae8cbb8c1807df6cfc4ecd47d99ff20727eec32f5c70fd`, run sequentially with nothing else holding the app and the same hash before and after: tool contract, proposal boundaries, project notes, chat, chat reliability, connection, DAW interaction and preview all report `FAILURES: none` (261 individual ok lines) and live-sync passes its 28. CI runs only when someone asks it to (`gh workflow run cocompose-windows.yml --ref ai-editor`); a push does not trigger it. Details are in the work log. Third-party VST3 compatibility is recorded per plugin for the development machine; listening to the output is a person's judgement and is not claimed. Graph changes may briefly interrupt playback before it resumes on the next UI tick; live sync does not guarantee gapless audio.

Keep upstream license notices intact. Tracktion Engine and JUCE have separate licenses; see the upstream README and JUCE license files.
