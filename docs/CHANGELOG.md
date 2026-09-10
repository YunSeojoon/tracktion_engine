# CoCompose Changelog

Dates are the day the work landed on the `ai-editor` branch. Each release records
the commit it was built from; `BUILD-INFO.json` inside the ZIP carries the exact
one for the copy you have.

## 0.1.0 — 2026-09-11 (release candidate 2)

Designated from commit 9110648. What was run against that exact binary, where, and
what is still unverified is in `docs/release-candidate.ko.md`. Since the first draft
of these notes the second review's P0 to P4 landed: a render never replaces a good
file before its replacement exists, the release seal reads the report it is given
instead of trusting it, uninstalling says whether it worked, automation curves have
a shape that the engine plays, a knob can be learnt, and the surface has one palette.

## 0.1.0 — 2026-09-10

The first packaged build. A Windows DAW on Tracktion Engine whose open project an
outside tool edits live through JSON files, with no restart used as a sync step.

### The work surface

- Four areas: Browser, Channel Rack with a step grid and piano roll, Playlist
  arrangement grid, and a Mixer with a real signal path.
- Channels choose an instrument: 4OSC, the sampler, or any scanned VST3.
- Patterns are placed on playlist lanes; a placement can be moved, split,
  duplicated and made independent of the pattern it came from.
- Audio clips can be dropped on a lane and trimmed, faded and stretched.
- Mixer inserts carry an effect chain — six built in plus any scanned VST3 effect —
  and can send to another insert without moving the dry signal off the channel.
- Automation curves are edited on screen, under the arrangement, and the engine
  plays them.
- Instrument presets save and recall a plugin's own settings per channel.
- Undo and Redo say what they will undo.

### Live sync

- `project.json` in, `state.json` out, with `sync-status.json` reporting what each
  request changed and why one was refused.
- Requests carry a revision, so an edit made against a stale read is refused rather
  than silently overwriting the app.
- `control.json` drives the transport, saving, undo and quit from outside.
- The Edit is opened once at startup and never replaced.

### Recording and output

- Recording into an armed channel; a take becomes a pattern placed in the
  arrangement.
- Mix and per-channel stem rendering to WAV, over a snapshot of the project so it
  stays editable while a render runs.
- Automatic backups that roll, a recovery path when a session is lost, and a report
  of samples the project can no longer find.

### Known limits

- One project open at a time. Opening a second brings the first to the front and says
  which project it is already on.
- Only VST3 is scanned, from the default locations.
- Presets only load onto the same kind of instrument, and say what they are for when
  they do not fit.
- The build is not code-signed and there is no auto-update. See `docs/release.ko.md`.
- Listening to the output, recording from a real keyboard, and switching devices
  mid-playback are a person's steps and are not claimed. See
  `docs/acceptance-2026-09-10.ko.md`.

### Verified

- 28 integration checks against the real packaged executable, in CI on every push,
  and the ZIP is only published when they pass. The seal ties each report to the
  binary it actually ran against.
- The 23 VST3 plugins installed on the development machine, plus its audio device,
  three display scales, repeated save/undo and 65 minutes of continuous playback.
  See `docs/compatibility-2026-09-10.ko.md`.
- Listening to the output is a person's judgement and is not claimed here.
