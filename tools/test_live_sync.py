"""Integration check against the real Windows app; no mock engine or GUI."""
import argparse
import copy
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import time
import struct
import threading
import uuid
import wave
from xml.etree import ElementTree

from cocompose import (Conflict, apply_change, atomic_write, control, current, read,
                       submit, wait_for)

LEGACY_SESSION = Path(__file__).resolve().parents[1] / "tests/cocompose/legacy-schema1.tracktionedit"


def equivalent(a, b):
    if isinstance(a, float) and isinstance(b, (float, int)):
        return math.isclose(a, b, rel_tol=0, abs_tol=1e-8)
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(equivalent(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(equivalent(x, y) for x, y in zip(a, b))
    return a == b


def only_pattern(state):
    return state["patterns"][0]


def notes_of(pattern, channel):
    return next(s for s in pattern["sequences"] if s["channel"] == channel)["notes"]


def engine_clips(state, pattern_id=None):
    """The MIDI clips the engine will actually play, optionally for one pattern only."""
    placements = {c["id"]: c for c in state["playlist"]["clips"]}
    found = []
    for track in state["engine"]["tracks"]:
        for clip in track["clips"]:
            placement = placements.get(clip["clip"])
            if placement is None:
                continue
            if pattern_id is None or placement["pattern"] == pattern_id:
                found.append(clip)
    return found


def settled(folder, quiet=0.4, timeout=20):
    """state.json is written a tick after an action runs, and rendering the result can
    take another tick, so wait until the published revision has stopped moving."""
    deadline = time.monotonic() + timeout
    last = None

    while time.monotonic() < deadline:
        status = read(folder / "sync-status.json")
        state = read(folder / "state.json")

        if state["revision"] == status["revision"] and state["revision"] == last:
            return state

        last = status["revision"]
        time.sleep(quiet)

    raise TimeoutError("CoCompose kept changing; state never settled")


def report_identity(exe):
    """What was actually run: which binary, from which commit. The hash is what lets a
    release step tell this report apart from one made against a different build."""
    exe = Path(exe)
    digest = hashlib.sha256(exe.read_bytes()).hexdigest().upper() if exe.exists() else ""
    commit = ""
    try:
        commit = subprocess.run(["git", "-C", str(Path(__file__).resolve().parents[1]),
                                 "rev-parse", "HEAD"],
                                capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return digest, commit


def write_report(exe, folder, checks, failure):
    digest, commit = report_identity(exe)
    report = {
        "schema": 1,
        "result": "passed" if failure is None else "failed",
        "checks": len(checks),
        "passed": checks,
        "failure": "" if failure is None else "%s: %s" % (type(failure).__name__, failure),
        "executable": str(exe),
        "executable_sha256": digest,
        "commit": commit,
        "finished_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "folder": str(folder),
    }
    atomic_write(folder / "test-report.json", report)
    print(json.dumps(report, indent=2))
    return report


def run(exe, folder):
    folder.mkdir(parents=True, exist_ok=False)
    project = folder / "project.json"
    process = None
    checks = []

    def launch(play=False, project_file=None, cwd=None):
        command = [str(exe), "--project", str(project_file or project), "--headless", "--screenshots"]
        if play:
            command.append("--play")
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        return subprocess.Popen(command, startupinfo=startup, cwd=cwd)

    def live_state():
        return read(folder / "state.json")

    def status():
        return read(folder / "sync-status.json")

    def rejected(data, fragment):
        before = live_state()
        atomic_write(project, data)
        wait_for(lambda: fragment in status().get("error", ""))
        assert live_state() == before, "Rejected edit changed live state"

    try:
        process = launch(play=True)
        wait_for(lambda: status().get("playing"), timeout=30)
        first = live_state()
        identity = (status()["session_id"], status()["edit_instance"])
        assert first["schema"] == 2
        assert len(first["channels"]) == 1
        assert len(first["patterns"]) == 1
        assert len(first["playlist"]["clips"]) == 1
        assert len(first["mixer"]["inserts"]) == 1
        assert len(notes_of(only_pattern(first), first["channels"][0]["id"])) == 32
        assert len(engine_clips(first)) == 1, "Playlist placement produced no engine clip"
        assert len(engine_clips(first)[0]["notes"]) == 32
        wait_for(lambda: (folder / "ui.png").exists())
        shutil.copyfile(folder / "ui.png", folder / "before.png")
        checks.append("Windows startup and real Tracktion playback")

        channel_id = first["channels"][0]["id"]
        data = copy.deepcopy(first)
        data["bpm"] = 137
        data["channels"][0]["name"] = "Live sync verified"
        notes_of(only_pattern(data), channel_id)[0]["pitch"] = 71
        notes_of(only_pattern(data), channel_id)[0]["velocity"] = 103
        changed = submit(project, data)
        assert changed["bpm"] == 137
        assert notes_of(only_pattern(changed), channel_id)[0]["pitch"] == 71
        assert notes_of(only_pattern(changed), channel_id)[0]["velocity"] == 103
        played = engine_clips(changed)[0]["notes"]
        assert any(n["pitch"] == 71 and n["velocity"] == 103 for n in played), "Engine did not get the edit"
        assert (status()["session_id"], status()["edit_instance"]) == identity
        assert status()["playing"], "Live edit stopped playback"
        assert changed["channels"][0]["parameters"][0]["plugin_id"] == first["channels"][0]["parameters"][0]["plugin_id"]
        before_hash = hashlib.sha256((folder / "before.png").read_bytes()).digest()
        wait_for(lambda: "Live sync verified" in read(folder / "ui-state.json")["labels"])
        wait_for(lambda: hashlib.sha256((folder / "ui.png").read_bytes()).digest() != before_hash)
        shutil.copyfile(folder / "ui.png", folder / "after.png")
        checks.append("Tempo, channel name, pitch and velocity update without replacing Edit; UI changes")

        invalid = live_state()
        notes_of(only_pattern(invalid), channel_id)[0]["velocity"] = 999
        rejected(invalid, "velocity out of range")
        dangling = live_state()
        dangling["playlist"]["clips"][0]["pattern"] = "no-such-pattern"
        rejected(dangling, "unknown pattern")
        checks.append("Invalid MIDI and dangling pattern references rejected before mutation")

        before_partial = live_state()
        project.write_text('{"schema":', encoding="utf-8")
        wait_for(lambda: "Invalid JSON" in status().get("error", ""))
        assert live_state() == before_partial

        # The same rule for control requests. A half-written one must not be counted as
        # seen, or the request that follows it is never answered and whoever sent it
        # waits for a reply that is not coming.
        (folder / "control.json").write_text('{"id": "half', encoding="utf-8")
        time.sleep(1.0)
        control(project, "undo")
        control(project, "redo")
        checks.append("Partial JSON preserves previous live state, and a half-written "
                      "control request is not swallowed")

        stale = live_state()
        stale["revision"] -= 1
        rejected(stale, "Revision conflict")
        wrong_session = live_state()
        wrong_session["session_id"] = "previous-session"
        rejected(wrong_session, "Session changed")
        checks.append("Stale revision and wrong session rejected")

        restored = submit(project, live_state())
        notes_before = copy.deepcopy(notes_of(only_pattern(restored), channel_id))
        data = live_state()
        notes_of(only_pattern(data), channel_id).clear()
        cleared = submit(project, data)
        assert not notes_of(only_pattern(cleared), channel_id)
        assert not engine_clips(cleared), "Emptied pattern left a clip on the engine"
        control(project, "undo")
        assert notes_of(only_pattern(live_state()), channel_id) == notes_before
        assert len(engine_clips(live_state())[0]["notes"]) == 32, "Undo did not restore the engine clip"
        control(project, "redo")
        assert not notes_of(only_pattern(live_state()), channel_id)
        control(project, "undo")
        checks.append("Empty pattern clears MIDI and its clips; one Undo restores; Redo reapplies")

        data = live_state()
        param = next(p for p in data["channels"][0]["parameters"] if p["id"] == "filterFreq")
        old_value = param["value"]
        param["value"] = 0.31
        updated = submit(project, data)
        actual = next(p for p in updated["channels"][0]["parameters"] if p["id"] == "filterFreq")
        assert abs(actual["value"] - 0.31) < 0.01
        control(project, "undo")
        actual = next(p for p in live_state()["channels"][0]["parameters"] if p["id"] == "filterFreq")
        assert abs(actual["value"] - old_value) < 0.01
        checks.append("Synth parameter write and Undo verified against engine readback")

        data = live_state()
        data["mixer"]["inserts"].append({"id": "insert-2", "index": 2, "name": "Insert 2",
                                         "gain_db": 0.0, "pan": 0.0, "mute": False})
        data["channels"].insert(0, {"id": "second", "name": "New channel", "gain_db": -18, "pan": 0.0,
                                    "mute": False, "solo": False, "insert": 2, "parameters": []})
        result = submit(project, data)
        assert [c["id"] for c in result["channels"]] == ["second", channel_id], [c["id"] for c in result["channels"]]
        wanted = [c["id"] for c in result["channels"]]
        assert [t["channel"] for t in result["engine"]["tracks"] if t["channel"] in wanted] == wanted, \
            "Channel order did not reach the engine tracks"
        data = live_state()
        data["channels"] = [c for c in data["channels"] if c["id"] != "second"]
        submit(project, data)
        assert (status()["session_id"], status()["edit_instance"]) == identity
        checks.append("Channel insertion, reordering and removal preserve live Edit")

        # M1: one pattern, two placements. Editing the pattern must move both.
        data = live_state()
        shared = only_pattern(data)
        lane_id = data["playlist"]["lanes"][0]["id"]
        data["playlist"]["clips"].append({"id": "placement-2", "lane": lane_id,
                                          "pattern": shared["id"], "start": 32.0,
                                          "length": shared["length"]})
        two = submit(project, data)
        assert len(engine_clips(two, shared["id"])) == 2, "Second placement produced no engine clip"
        assert sorted(round(c["start"], 6) for c in engine_clips(two, shared["id"])) == [0.0, 32.0]

        data = live_state()
        for note in notes_of(only_pattern(data), channel_id):
            note["pitch"] += 5
        both = submit(project, data)
        pitches = [sorted(n["pitch"] for n in clip["notes"]) for clip in engine_clips(both, shared["id"])]
        assert len(pitches) == 2 and pitches[0] == pitches[1], "Placements drifted apart"
        assert min(pitches[0]) == 53, "Pattern edit did not reach both placements"
        control(project, "undo")
        after_undo = live_state()
        pitches = [sorted(n["pitch"] for n in clip["notes"]) for clip in engine_clips(after_undo, shared["id"])]
        assert len(pitches) == 2 and pitches[0] == pitches[1] and min(pitches[0]) == 48, \
            "One Undo did not restore both placements"
        checks.append("Two placements of one pattern edit and undo together")

        # M1: Make unique detaches a placement, so editing it leaves the other alone.
        data = live_state()
        shared = only_pattern(data)
        detached = copy.deepcopy(shared)
        detached["id"] = "unique-pattern"
        detached["name"] = "Unique"
        for sequence in detached["sequences"]:
            for index, note in enumerate(sequence["notes"]):
                note["id"] = "unique-note-%d" % index
                note["pitch"] += 12
        data["patterns"].append(detached)
        next(c for c in data["playlist"]["clips"] if c["id"] == "placement-2")["pattern"] = "unique-pattern"
        unique = submit(project, data)
        assert len(unique["patterns"]) == 2
        original_pitches = sorted(n["pitch"] for n in engine_clips(unique, shared["id"])[0]["notes"])
        copy_pitches = sorted(n["pitch"] for n in engine_clips(unique, "unique-pattern")[0]["notes"])
        assert min(original_pitches) == 48, "Detaching a placement changed the shared pattern"
        assert min(copy_pitches) == 60, "The detached placement did not get its own notes"
        data = live_state()
        data["playlist"]["clips"] = [c for c in data["playlist"]["clips"] if c["id"] != "placement-2"]
        data["patterns"] = [p for p in data["patterns"] if p["id"] != "unique-pattern"]
        pruned = submit(project, data)
        assert len(engine_clips(pruned)) == 1
        checks.append("Duplicating a pattern makes one placement independent")

        control(project, "stop")
        assert not status()["playing"]

        # Simulate a blocked output path only inside this fresh test directory.
        data = live_state()
        data["bpm"] = 141
        state_path = folder / "state.json"
        state_path.rename(folder / "state-before-io-test.json")
        state_path.mkdir()
        try:
            atomic_write(project, data)
            wait_for(lambda: status()["status"] == "applied_unpersisted")
        finally:
            state_path.rmdir()
        wait_for(lambda: status()["status"] == "synced" and live_state()["bpm"] == 141)
        checks.append("Persistence failure distinguished from rejection; state saved automatically after recovery")

        final = live_state()
        control(project, "quit")
        assert process.wait(timeout=15) == 0
        process = launch()
        wait_for(lambda: status()["session_id"] != identity[0], timeout=30)
        reopened = live_state()
        for section in ("channels", "patterns", "playlist", "mixer"):
            assert equivalent(reopened[section], final[section]), \
                "Saved %s changed beyond floating-point roundoff" % section
        # The engine tracks are derived from the saved project and have to match.
        # The device beside them is a fact about the machine, not part of the project:
        # the MIDI inputs are still being enumerated moments after a launch, so
        # comparing them would be comparing how fast Windows listed the hardware.
        assert equivalent(reopened["engine"]["tracks"], final["engine"]["tracks"]), \
            "Saved engine changed beyond floating-point roundoff"
        assert reopened["bpm"] == final["bpm"]
        checks.append("Graceful exit and native session save/reload preserve music and plugin state")
        control(project, "quit")
        assert process.wait(timeout=15) == 0

        checks.append(check_legacy_session(launch, folder))
        checks.append(check_legacy_json_input(launch, folder))
        checks.append(check_workspace_layout(launch, folder))
        checks.append(check_pattern_built_in_the_ui(exe, folder))
        checks.append(check_arrangement_built_in_the_ui(exe, folder))
        checks.append(check_audio_clips_and_assets(exe, folder))
        checks.append(check_mixer_routing_and_effects(exe, folder))
        checks.append(check_automation_and_render(exe, folder))
        checks.append(check_external_agent_session(exe, folder))
        checks.append(check_every_menu_command(exe, folder))
        checks.append(check_survives_the_rough_edges(exe, folder))
        checks.append(check_render_output_is_never_lost(exe, folder))
        checks.append(check_render_boundaries(exe, folder))
        checks.append(check_recording_and_recovery(exe, folder))
        checks.append(check_the_last_editing_gaps(exe, folder))
        checks.append(check_a_song_made_only_on_screen(exe, folder))

        write_report(exe, folder, checks, None)
    except BaseException as failure:
        # A failed run leaves a report too, saying so. Otherwise a release script has
        # nothing to reject and the only evidence of the failure is a console log.
        write_report(exe, folder, checks, failure)
        raise
    finally:
        if process is not None and process.poll() is None:
            try:
                control(project, "quit")
                process.wait(timeout=10)
            except (OSError, RuntimeError, TimeoutError, subprocess.TimeoutExpired):
                process.terminate()
                process.wait(timeout=10)


def with_session(launch, folder, name, prepare):
    """Runs the app once in a fresh sub-folder prepared by the caller."""
    sub = folder / name
    sub.mkdir()
    prepare(sub)
    process = launch(project_file=sub / "project.json")
    try:
        wait_for(lambda: read(sub / "state.json"), timeout=30)
        time.sleep(0.5)
        state = read(sub / "state.json")
        status = read(sub / "sync-status.json")
        assert not status["error"], status["error"]
        return state
    finally:
        atomic_write(sub / "control.json", {"id": "quit", "action": "quit",
                                            "session_id": read(sub / "sync-status.json")["session_id"],
                                            "revision": read(sub / "state.json")["revision"]})
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=10)


def check_legacy_session(launch, folder):
    """A session saved by the pre-pattern build must convert without losing the music."""
    assert LEGACY_SESSION.exists(), "Missing legacy session fixture: %s" % LEGACY_SESSION

    def prepare(sub):
        shutil.copyfile(LEGACY_SESSION, sub / "session.tracktionedit")

    state = with_session(launch, folder, "legacy-session", prepare)
    assert state["schema"] == 2
    assert [c["id"] for c in state["channels"]] == ["synth-1"], state["channels"]
    assert [p["id"] for p in state["patterns"]] == ["phrase-1"], state["patterns"]
    assert len(state["playlist"]["lanes"]) == 1
    placement = state["playlist"]["clips"][0]
    assert placement["pattern"] == "phrase-1"
    assert round(placement["start"], 6) == 0.0
    assert round(placement["length"], 6) == 32.0
    assert state["channels"][0]["insert"] == state["mixer"]["inserts"][0]["index"]
    notes = notes_of(state["patterns"][0], "synth-1")
    assert len(notes) == 32, len(notes)
    assert [n["id"] for n in notes[:3]] == ["note-0", "note-1", "note-2"], "Note ids were not kept"
    assert sorted(n["pitch"] for n in notes)[0] == 48
    clips = engine_clips(state)
    assert len(clips) == 1 and len(clips[0]["notes"]) == 32, "Converted arrangement lost its engine clip"
    return "Pre-pattern session converts to channels, patterns and playlist with ids and notes intact"


def check_legacy_json_input(launch, folder):
    """A schema 1 project.json written by an older script must still apply."""
    legacy = {
        "schema": 1, "revision": 0, "session_id": "", "request_id": "", "bpm": 96.0,
        "tracks": [{"id": "legacy-track", "name": "Legacy", "gain_db": -9.0,
                    "mute": False, "solo": False, "parameters": [],
                    "clips": [{"id": "legacy-clip", "name": "Legacy clip", "start": 0.0, "length": 8.0,
                               "notes": [{"id": "legacy-note", "pitch": 55, "velocity": 90,
                                          "start": 0.0, "length": 1.0}]}]}],
    }

    def prepare(sub):
        atomic_write(sub / "project.json", legacy)

    state = with_session(launch, folder, "legacy-json", prepare)
    assert state["schema"] == 2
    assert state["bpm"] == 96.0
    assert [c["id"] for c in state["channels"]] == ["legacy-track"]
    assert [p["id"] for p in state["patterns"]] == ["legacy-clip"]
    notes = notes_of(state["patterns"][0], "legacy-track")
    assert [n["id"] for n in notes] == ["legacy-note"]
    assert notes[0]["pitch"] == 55
    assert len(engine_clips(state)) == 1
    return "Schema 1 project.json input upgrades to the pattern model"


def check_workspace_layout(launch, folder):
    """Panel sizes, visibility and selection live in the session, so a reopened project
    comes back with the same work surface."""
    session = folder / "workspace/session.tracktionedit"

    state = with_session(launch, folder, "workspace", lambda _: None)
    layout = ElementTree.parse(session).getroot().find("COCOMPOSELAYOUT")
    assert layout is not None, "The session has no saved workspace layout"

    sizes = [float(v) for v in layout.get("sizes", "").split()]
    # One number per shared split. The chat panel added a fifth when it became a column.
    assert len(sizes) == 5 and all(size > 1.0 for size in sizes), layout.get("sizes")
    assert layout.get("visible") == "11111", layout.get("visible")
    assert layout.get("selectedChannel") == state["channels"][0]["id"]
    assert layout.get("selectedPattern") == state["patterns"][0]["id"]
    assert layout.get("selectedLane") == state["playlist"]["lanes"][0]["id"]

    # Hide the Mixer and the Playlist and narrow the browser, the way the View menu and
    # the resizer bars do, then confirm the next run honours it instead of resetting.
    tree = ElementTree.parse(session)
    edited = tree.getroot().find("COCOMPOSELAYOUT")
    edited.set("visible", "11010")
    edited.set("sizes", "160 " + " ".join(str(size) for size in sizes[1:]))
    tree.write(session, encoding="UTF-8", xml_declaration=True)

    with_session(launch, folder, "workspace-reopened", lambda target:
                 shutil.copyfile(session, target / "session.tracktionedit"))
    reopened = ElementTree.parse(folder / "workspace-reopened/session.tracktionedit").getroot()
    restored = reopened.find("COCOMPOSELAYOUT")
    assert restored.get("visible") == "11010", restored.get("visible")
    restored_sizes = [float(v) for v in restored.get("sizes", "").split()]
    assert abs(restored_sizes[0] - 160.0) < 1.0, restored_sizes
    return "Panel sizes, visibility and selection are saved in the session and restored"


def check_pattern_built_in_the_ui(exe, folder):
    """Builds a drum, bass and melody pattern using only the work surface: menu
    commands, the Channel Rack step grid and the piano roll. Then plays it, saves it,
    and confirms an outside note edit reaches the open editor."""
    sub = folder / "ui-built"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"

    drum_steps = [0, 4, 8, 12, 2, 10]
    bass_steps = [0, 6, 8, 14]
    melody = [(72, 0.0, 0.5, 100), (76, 0.5, 0.5, 96), (79, 1.0, 1.0, 104),
              (76, 2.0, 0.5, 92), (72, 2.5, 1.5, 88)]

    actions = [{"command": "New pattern"}, {"select_pattern": 1}]
    # Channel 0 is the seeded synth; add the bass and the drums beside it.
    actions += [{"command": "Add channel"}, {"command": "Add channel"}]
    actions += [{"select_channel": 0}] + [{"step": [0, step]} for step in drum_steps]
    actions += [{"select_channel": 1}] + [{"step": [1, step]} for step in bass_steps]
    actions += [{"select_channel": 2}] + [{"note": list(note)} for note in melody]
    actions += [{"select_lane": 0}, {"command": "Place pattern"},
                {"command": "Play / Stop"}, {"command": "Save now"}]
    atomic_write(script, actions)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "ui-script-status.json").get("finished"), timeout=90)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status["error"]
        assert status["done"] == status["total"], status

        state = read(sub / "state.json")
        assert len(state["channels"]) == 3, state["channels"]
        built = next(p for p in state["patterns"] if len(p["sequences"]) == 3)
        by_channel = {s["channel"]: s["notes"] for s in built["sequences"]}
        ids = [c["id"] for c in state["channels"]]

        assert len(by_channel[ids[0]]) == len(drum_steps), by_channel[ids[0]]
        assert sorted(round(n["start"] / 0.25) for n in by_channel[ids[0]]) == sorted(drum_steps)
        assert len(by_channel[ids[1]]) == len(bass_steps)
        assert sorted(round(n["start"] / 0.25) for n in by_channel[ids[1]]) == sorted(bass_steps)
        assert sorted((n["pitch"], n["start"]) for n in by_channel[ids[2]]) == \
            sorted((p, s) for p, s, _, _ in melody)

        played = engine_clips(state, built["id"])
        assert len(played) == 3, "The pattern the UI built did not reach three engine clips"
        assert sum(len(clip["notes"]) for clip in played) == len(drum_steps) + len(bass_steps) + len(melody)
        assert read(sub / "sync-status.json")["playing"], "Play from the transport did not start"

        # An outside note edit has to show up in the editor that is already open.
        outside = read(sub / "state.json")
        target = next(p for p in outside["patterns"] if p["id"] == built["id"])
        notes = next(s for s in target["sequences"] if s["channel"] == ids[2])["notes"]
        for note in notes:
            note["pitch"] += 3
        updated = submit(project, outside)
        shown = next(s for s in next(p for p in updated["patterns"] if p["id"] == built["id"])["sequences"]
                     if s["channel"] == ids[2])["notes"]
        assert sorted(n["pitch"] for n in shown) == sorted(p + 3 for p, _, _, _ in melody)
        assert sorted(n["pitch"] for n in engine_clips(updated, built["id"])[2]["notes"]) == \
            sorted(p + 3 for p, _, _, _ in melody)
        labels = read(sub / "ui-state.json")["labels"]
        for channel in state["channels"]:
            assert channel["name"] in labels, (channel["name"], labels)

        first_session = read(sub / "sync-status.json")["session_id"]
        control(project, "quit")
        assert process.wait(timeout=20) == 0

        # And it all has to come back from the saved session.
        process = subprocess.Popen([str(exe), "--project", str(project), "--headless"], startupinfo=startup)
        wait_for(lambda: read(sub / "sync-status.json")["session_id"] != first_session, timeout=40)
        reopened = read(sub / "state.json")
        assert len(reopened["channels"]) == 3
        restored = next(p for p in reopened["patterns"] if p["id"] == built["id"])
        assert len(restored["sequences"]) == 3
        assert len(engine_clips(reopened, built["id"])) == 3
        control(project, "quit")
        assert process.wait(timeout=20) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return "Drum, bass and melody channels built through the rack and piano roll play, save and reload"


def engine_clip_for(state, clip_id):
    for track in state["engine"]["tracks"]:
        for clip in track["clips"]:
            if clip["clip"] == clip_id:
                return clip
    return None


def check_arrangement_built_in_the_ui(exe, folder):
    """Builds a thirty-two bar arrangement in the playlist grid: place, move, split and
    undo, all in one running session, and confirms the sound, the screen and state.json
    agree. Then an outside placement lands while it is still playing."""
    sub = folder / "arrangement"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    stage = {"round": 0}

    def run(actions):
        stage["round"] += 1
        # The app reloads the script when its contents change, so repeating the same
        # actions needs a marker to tell the rounds apart.
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=90)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status
        return settled(sub)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless", "--play",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "sync-status.json").get("playing"), timeout=40)
        session = read(sub / "sync-status.json")["session_id"]

        # A four-bar pattern of eighth notes, so a split has something to cut through.
        state = run([{"command": "New pattern"}, {"select_pattern": 1}, {"select_channel": 0}]
                    + [{"step": [0, step]} for step in range(0, 16, 2)])
        built = state["patterns"][1]["id"]

        data = read(sub / "state.json")
        next(p for p in data["patterns"] if p["id"] == built)["length"] = 16.0
        assert next(p for p in submit(project, data)["patterns"] if p["id"] == built)["length"] == 16.0

        # Eight placements of a four-bar pattern make thirty-two bars.
        state = run([{"select_pattern": 1}, {"select_lane": 0}]
                    + [{"place": [0, bar * 16.0]} for bar in range(8)])
        placed = [c for c in state["playlist"]["clips"] if c["pattern"] == built]
        assert len(placed) == 8, placed
        assert max(c["start"] + c["length"] for c in placed) == 128.0, placed
        assert len(engine_clips(state, built)) == 8, "The arrangement did not reach the engine"
        assert read(sub / "sync-status.json")["playing"], "The arrangement stopped playing"

        # Move the last clip a bar later.
        state = run([{"pick_clip": [0, 112.0]}, {"move_clip": [4.0, 0]}])
        moved = next(c for c in state["playlist"]["clips"] if abs(c["start"] - 116.0) < 1e-6)
        assert abs(engine_clip_for(state, moved["id"])["start"] - 116.0) < 1e-6, \
            "The engine clip did not follow the move"

        # Split it in half; the second half continues further into the pattern.
        state = run([{"pick_clip": [0, 116.0]}, {"split_clip": 124.0}])
        halves = sorted((c for c in state["playlist"]["clips"]
                         if abs(c["start"] - 116.0) < 1e-6 or abs(c["start"] - 124.0) < 1e-6),
                        key=lambda c: c["start"])
        assert len(halves) == 2, halves
        assert abs(halves[0]["length"] - 8.0) < 1e-6 and abs(halves[1]["length"] - 8.0) < 1e-6, halves
        assert abs(halves[1]["offset"] - 8.0) < 1e-6, "The second half does not continue the pattern"
        first, second = (engine_clip_for(state, half["id"]) for half in halves)
        assert first is not None and second is not None, "A half lost its engine clip"
        assert len(first["notes"]) + len(second["notes"]) == 8, (first["notes"], second["notes"])
        assert all(n["start"] < 8.0 for n in second["notes"]), second["notes"]

        # One undo puts the clip back together, on screen and in the engine.
        control(project, "undo")
        time.sleep(0.5)
        state = read(sub / "state.json")
        rejoined = [c for c in state["playlist"]["clips"] if abs(c["start"] - 116.0) < 1e-6]
        assert len(rejoined) == 1 and abs(rejoined[0]["length"] - 16.0) < 1e-6, rejoined
        assert len(engine_clip_for(state, rejoined[0]["id"])["notes"]) == 8, "Undo left the engine split"

        # Duplicating a clip repeats it directly after itself. Bar 13 is clear of the
        # eight-bar example placement the empty project starts with.
        state = run([{"pick_clip": [0, 48.0]}, {"command": "Duplicate clip"}])
        assert len([c for c in state["playlist"]["clips"] if c["pattern"] == built]) == 9,             [c["start"] for c in state["playlist"]["clips"] if c["pattern"] == built]

        # An outside change to the arrangement lands while it is still playing.
        assert read(sub / "sync-status.json")["playing"]
        outside = read(sub / "state.json")
        outside["playlist"]["clips"].append({"id": "outside-clip", "lane": outside["playlist"]["lanes"][0]["id"],
                                             "pattern": built, "start": 140.0, "length": 16.0, "offset": 0.0})
        updated = submit(project, outside)
        assert engine_clip_for(updated, "outside-clip") is not None, "Outside placement never played"
        assert read(sub / "sync-status.json")["session_id"] == session, "The project was reopened"
        assert read(sub / "sync-status.json")["playing"], "The outside change stopped playback"

        control(project, "quit")
        assert process.wait(timeout=20) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return "A thirty-two bar arrangement placed, moved, split and undone in the playlist grid"


def write_wav(path, seconds=2.0, frequency=220.0, rate=44100):
    """A short tone, so a check does not need a sample library."""
    frames = int(seconds * rate)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(b"".join(
            struct.pack("<h", int(20000 * math.sin(2 * math.pi * frequency * i / rate)))
            for i in range(frames)))
    return path


def audio_in_engine(state, clip_id):
    for track in state["engine"]["tracks"]:
        for clip in track.get("audio", []):
            if clip["clip"] == clip_id:
                return clip
    return None


def check_audio_clips_and_assets(exe, folder):
    """Drops a WAV onto a lane, shapes it, collects the project's samples, and reopens
    the folder somewhere else to confirm it still plays."""
    sub = folder / "audio"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    outside = folder / "outside-samples"
    outside.mkdir()
    sample = write_wav(outside / "tone.wav")
    stage = {"round": 0}

    def run(actions):
        stage["round"] += 1
        # The app reloads the script when its contents change, so repeating the same
        # actions needs a marker to tell the rounds apart.
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=90)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status
        return settled(sub)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless", "--play",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "sync-status.json").get("playing"), timeout=40)

        state = run([{"select_lane": 0}, {"audio": [0, str(sample), 8.0]}])
        assert len(state["playlist"]["audio"]) == 1, state["playlist"]["audio"]
        clip = state["playlist"]["audio"][0]
        assert clip["file"] == str(sample), clip
        assert not clip["missing"]
        assert abs(clip["start"] - 8.0) < 1e-6, clip
        played = audio_in_engine(state, clip["id"])
        assert played is not None, "The dropped WAV never reached the engine"
        assert abs(played["start"] - 8.0) < 1e-6, played

        # Trim it, move its start into the file, set gain, fades and speed.
        state = run([{"shape": ["length", 2.0]}, {"shape": ["offset", 1.0]}, {"shape": ["gain", -4.5]},
                     {"shape": ["fade_in", 0.25]}, {"shape": ["fade_out", 0.5]}, {"shape": ["speed", 1.5]}])
        clip = state["playlist"]["audio"][0]
        assert abs(clip["length"] - 2.0) < 1e-6 and abs(clip["offset"] - 1.0) < 1e-6, clip
        played = audio_in_engine(state, clip["id"])
        assert abs(played["length"] - 2.0) < 1e-6, played
        assert abs(played["gain_db"] + 4.5) < 0.01, played
        assert abs(played["fade_in"] - 0.25) < 0.01 and abs(played["fade_out"] - 0.5) < 0.01, played
        assert abs(played["speed"] - 1.5) < 0.01, played
        assert played["offset_seconds"] > 0.4, played

        # A file that is not audio is refused before anything changes.
        junk = outside / "notes.txt"
        junk.write_text("this is not a wave file", encoding="utf-8")
        before = read(sub / "state.json")
        bad = read(sub / "state.json")
        bad["playlist"]["audio"][0]["file"] = str(junk)
        atomic_write(project, bad)
        wait_for(lambda: "Not an audio file" in read(sub / "sync-status.json").get("error", ""))
        assert read(sub / "state.json") == before, "A refused file changed the project"
        assert audio_in_engine(read(sub / "state.json"), clip["id"]) is not None, "The good clip stopped playing"

        # A file that is gone is reported, not silently dropped.
        moved = outside / "tone-moved.wav"
        sample.rename(moved)
        wait_for(lambda: read(sub / "state.json")["playlist"]["audio"][0]["missing"], timeout=20)
        moved.rename(sample)
        wait_for(lambda: not read(sub / "state.json")["playlist"]["audio"][0]["missing"], timeout=20)

        # Collecting copies it into the project folder and repoints the clip.
        state = run([{"command": "Collect samples"}])
        collected = state["playlist"]["audio"][0]
        assert Path(collected["file"]).parent == sub / "samples", collected
        assert Path(collected["file"]).exists()
        assert audio_in_engine(state, collected["id"]) is not None, "The collected sample stopped playing"

        control(project, "quit")
        assert process.wait(timeout=20) == 0
        process = None

        # The folder is self-contained: move it and it still plays.
        elsewhere = folder / "moved-project"
        shutil.copytree(sub, elsewhere)
        shutil.rmtree(outside)
        for stale in ("state.json", "sync-status.json", "ui-script.json", "ui-script-status.json"):
            (elsewhere / stale).unlink(missing_ok=True)

        # A copied folder keeps absolute paths, so point the clip at its own copy first.
        moved_project = elsewhere / "project.json"
        document = json.loads(moved_project.read_text(encoding="utf-8-sig"))
        for entry in document["playlist"]["audio"]:
            entry["file"] = str(elsewhere / "samples" / Path(entry["file"]).name)
        moved_project.write_text(json.dumps(document), encoding="utf-8")

        process = subprocess.Popen([str(exe), "--project", str(moved_project), "--headless"],
                                   startupinfo=startup)
        wait_for(lambda: read(elsewhere / "state.json"), timeout=40)
        time.sleep(0.7)
        reopened = read(elsewhere / "state.json")
        assert not read(elsewhere / "sync-status.json")["error"], read(elsewhere / "sync-status.json")
        assert len(reopened["playlist"]["audio"]) == 1, reopened["playlist"]["audio"]
        restored = reopened["playlist"]["audio"][0]
        assert not restored["missing"], restored
        assert audio_in_engine(reopened, restored["id"]) is not None, "The moved project lost its audio"
        control(moved_project, "quit")
        assert process.wait(timeout=20) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return "A dropped WAV plays, trims, fades and stretches, and survives collecting and moving the folder"


def engine_track(state, channel_id):
    return next((t for t in state["engine"]["tracks"] if t["channel"] == channel_id), None)


def check_mixer_routing_and_effects(exe, folder):
    """Three instruments into their own inserts, the drums into a bus, the bus sending
    to a reverb insert, everything reaching the master. Then the six effects, a
    refused feedback loop, and a reopen."""
    sub = folder / "mixer"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    stage = {"round": 0}

    def run(actions):
        stage["round"] += 1
        # The app reloads the script when its contents change, so repeating the same
        # actions needs a marker to tell the rounds apart.
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=90)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status
        return settled(sub)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless", "--play",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "sync-status.json").get("playing"), timeout=40)
        session = read(sub / "sync-status.json")["session_id"]

        state = run([{"command": "Add channel"}, {"command": "Add channel"}])
        assert len(state["channels"]) == 3
        assert len(state["mixer"]["inserts"]) == 3

        # Every channel already plays through its own insert, which reaches the master.
        for channel in state["channels"]:
            slot = channel["insert"]
            insert = next(i for i in state["mixer"]["inserts"] if i["index"] == slot)
            track = engine_track(state, channel["id"])
            assert track["output"] == "insert:" + insert["id"], (channel["name"], track["output"])
            assert engine_track(state, "insert:" + insert["id"])["output"] == "master"

        # A drum bus and a reverb bus, with the drums routed into the bus and sending
        # to the reverb. The insert the drums used becomes the bus.
        data = read(sub / "state.json")
        drums, bass, lead = data["channels"]
        inserts = data["mixer"]["inserts"]
        data["mixer"]["inserts"] += [
            {"id": "bus", "index": 4, "name": "Drum bus", "gain_db": -2.0, "pan": 0.0, "mute": False,
             "output": "master",
             "effects": [{"id": "fx-eq", "type": "eq", "bypass": False, "wet": 1.0, "parameters": []},
                         {"id": "fx-sat", "type": "saturation", "bypass": False, "wet": 1.0, "parameters": []},
                         {"id": "fx-lim", "type": "limiter", "bypass": False, "wet": 1.0, "parameters": []}],
             "sends": [{"id": "send-verb", "target": "verb", "level": -8.0}]},
            {"id": "verb", "index": 5, "name": "Reverb bus", "gain_db": -4.0, "pan": 0.0, "mute": False,
             "output": "master",
             "effects": [{"id": "fx-delay", "type": "delay", "bypass": False, "wet": 0.4, "parameters": []},
                         {"id": "fx-chorus", "type": "chorus", "bypass": False, "wet": 0.5, "parameters": []},
                         {"id": "fx-reverb", "type": "reverb", "bypass": False, "wet": 0.6, "parameters": []}],
             "sends": []}]
        # The drum insert now feeds the bus instead of the master.
        next(i for i in data["mixer"]["inserts"] if i["index"] == drums["insert"])["output"] = "bus"
        state = submit(project, data)

        bus = next(i for i in state["mixer"]["inserts"] if i["id"] == "bus")
        verb = next(i for i in state["mixer"]["inserts"] if i["id"] == "verb")
        assert [e["type"] for e in bus["effects"]] == ["eq", "saturation", "limiter"], bus["effects"]
        assert [e["type"] for e in verb["effects"]] == ["delay", "chorus", "reverb"], verb["effects"]

        drum_insert = next(i for i in state["mixer"]["inserts"] if i["index"] == drums["insert"])
        assert engine_track(state, "insert:" + drum_insert["id"])["output"] == "insert:bus", \
            "The drums do not reach the bus"
        assert engine_track(state, "insert:bus")["output"] == "master"

        # The six effects are real plugins on the bus tracks, in order.
        bus_plugins = [p for p in engine_track(state, "insert:bus")["plugins"] if p["effect"]]
        assert [p["type"] for p in bus_plugins][:3] == ["4bandEq", "coComposeSaturation", "compressor"], bus_plugins
        verb_plugins = [p for p in engine_track(state, "insert:verb")["plugins"] if p["effect"]]
        assert [p["type"] for p in verb_plugins][:3] == ["delay", "chorus", "reverb"], verb_plugins
        assert any(p["type"] == "auxsend" for p in engine_track(state, "insert:bus")["plugins"]), \
            "The send never reached the engine"
        assert any(p["type"] == "auxreturn" for p in engine_track(state, "insert:verb")["plugins"]), \
            "The reverb bus has no return"

        # An effect parameter is written and read back from the engine, then undone.
        data = read(sub / "state.json")
        target = next(e for e in next(i for i in data["mixer"]["inserts"] if i["id"] == "bus")["effects"]
                      if e["type"] == "saturation")
        drive = next(p for p in target["parameters"] if p["id"] == "drive")
        before = drive["value"]
        drive["value"] = 0.62
        state = submit(project, data)
        actual = next(p for p in next(e for e in next(i for i in state["mixer"]["inserts"] if i["id"] == "bus")["effects"]
                                      if e["type"] == "saturation")["parameters"] if p["id"] == "drive")
        assert abs(actual["value"] - 0.62) < 0.01, actual
        control(project, "undo")
        time.sleep(0.4)
        actual = next(p for p in next(e for e in next(i for i in read(sub / "state.json")["mixer"]["inserts"]
                                                      if i["id"] == "bus")["effects"]
                                      if e["type"] == "saturation")["parameters"] if p["id"] == "drive")
        assert abs(actual["value"] - before) < 0.01, (actual, before)

        # Bypassing an effect disables the plugin without removing it.
        data = read(sub / "state.json")
        next(e for e in next(i for i in data["mixer"]["inserts"] if i["id"] == "bus")["effects"]
             if e["type"] == "limiter")["bypass"] = True
        state = submit(project, data)
        limiter = next(p for p in engine_track(state, "insert:bus")["plugins"] if p["type"] == "compressor")
        assert not limiter["enabled"], limiter

        # A routing that would feed back on itself is refused, and nothing changes.
        before_state = read(sub / "state.json")
        loop = read(sub / "state.json")
        next(i for i in loop["mixer"]["inserts"] if i["id"] == "verb")["output"] = "verb"
        atomic_write(project, loop)
        wait_for(lambda: "cannot be routed to itself" in read(sub / "sync-status.json").get("error", ""))
        assert read(sub / "state.json") == before_state, "A refused routing changed the project"

        # Reordering the chain keeps the plugins and their settings.
        data = read(sub / "state.json")
        effects = next(i for i in data["mixer"]["inserts"] if i["id"] == "bus")["effects"]
        effects.insert(0, effects.pop())
        state = submit(project, data)
        assert [e["type"] for e in next(i for i in state["mixer"]["inserts"] if i["id"] == "bus")["effects"]] \
            == ["limiter", "eq", "saturation"]
        bus_plugins = [p for p in engine_track(state, "insert:bus")["plugins"] if p["effect"]]
        assert [p["type"] for p in bus_plugins][:3] == ["compressor", "4bandEq", "coComposeSaturation"], bus_plugins

        assert read(sub / "sync-status.json")["session_id"] == session, "The project was reopened"
        assert read(sub / "sync-status.json")["playing"], "The mixer work stopped playback"

        control(project, "quit")
        assert process.wait(timeout=20) == 0

        # The routing, the chains and the sends all come back from the saved session.
        process = subprocess.Popen([str(exe), "--project", str(project), "--headless"], startupinfo=startup)
        wait_for(lambda: read(sub / "sync-status.json")["session_id"] != session, timeout=40)
        time.sleep(0.6)
        reopened = read(sub / "state.json")
        bus = next(i for i in reopened["mixer"]["inserts"] if i["id"] == "bus")
        assert [e["type"] for e in bus["effects"]] == ["limiter", "eq", "saturation"], bus["effects"]
        assert [s["target"] for s in bus["sends"]] == ["verb"], bus["sends"]
        assert engine_track(reopened, "insert:" + drum_insert["id"])["output"] == "insert:bus"
        assert any(p["type"] == "coComposeSaturation"
                   for p in engine_track(reopened, "insert:bus")["plugins"]), "The saturation was not restored"
        control(project, "quit")
        assert process.wait(timeout=20) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return "Three instruments through their inserts, a drum bus with a reverb send, and the six effects"


def read_wav(path):
    """Peak, rms and length of a rendered file, using only the standard library."""
    with wave.open(str(path), "rb") as stream:
        frames = stream.getnframes()
        rate = stream.getframerate()
        channels = stream.getnchannels()
        width = stream.getsampwidth()
        raw = stream.readframes(frames)

    if width == 2:
        samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
        scale = 32768.0
    elif width == 3:
        samples = [int.from_bytes(raw[i:i + 3], "little", signed=True) for i in range(0, len(raw), 3)]
        scale = 8388608.0
    else:
        samples = struct.unpack("<%di" % (len(raw) // 4), raw)
        scale = 2147483648.0

    peak = max((abs(s) for s in samples), default=0) / scale
    total = math.sqrt(sum((s / scale) ** 2 for s in samples) / max(1, len(samples)))
    return {"seconds": frames / rate, "peak": peak, "rms": total, "channels": channels}


def check_automation_and_render(exe, folder):
    """Automates a synth filter and a channel fader, keeps a take on an armed channel,
    then renders the arrangement and its stems and checks the files are real audio."""
    sub = folder / "automation"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    stage = {"round": 0}

    def run(actions):
        stage["round"] += 1
        # The app reloads the script when its contents change, so repeating the same
        # actions needs a marker to tell the rounds apart.
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=180)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status
        return settled(sub)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless", "--play",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "sync-status.json").get("playing"), timeout=40)
        session = read(sub / "sync-status.json")["session_id"]
        state = read(sub / "state.json")
        channel = state["channels"][0]
        filter_param = next(p for p in channel["parameters"] if p["id"] == "filterFreq")

        # A filter sweep across the first eight bars, written through the model.
        state = run([{"automation": [0, filter_param["plugin_id"], "filterFreq",
                                     0.0, 0.15, 8.0, 0.9, 16.0, 0.3, 31.0, 0.75]}])
        curve = state["automation"]["curves"][0]
        assert curve["parameter"] == "filterFreq", curve
        assert len(curve["points"]) == 4, curve["points"]
        assert curve["engine_points"] == 4, "The curve never reached the engine"
        assert [round(p["time"], 3) for p in curve["points"]] == [0.0, 8.0, 16.0, 31.0]

        # Editing it from outside lands in the same curve and the same engine parameter.
        data = read(sub / "state.json")
        data["automation"]["curves"][0]["points"].append(
            {"id": "extra-point", "time": 24.0, "value": 0.05, "curve": 0.0})
        state = submit(project, data)
        curve = state["automation"]["curves"][0]
        assert len(curve["points"]) == 5 and curve["engine_points"] == 5, curve

        # One undo takes the point away again, from the model and the engine.
        control(project, "undo")
        time.sleep(0.5)
        curve = read(sub / "state.json")["automation"]["curves"][0]
        assert len(curve["points"]) == 4 and curve["engine_points"] == 4, curve

        # A curve naming a parameter that is not there is refused before anything moves.
        before = read(sub / "state.json")
        bad = read(sub / "state.json")
        bad["automation"]["curves"][0]["parameter"] = "notAParameter"
        atomic_write(project, bad)
        wait_for(lambda: "parameter that does not exist" in read(sub / "sync-status.json").get("error", ""))
        assert read(sub / "state.json") == before, "A refused curve changed the project"

        # Arming a channel, then a take landing on it. No MIDI keyboard is plugged in
        # here, so the take is put on the track the way a recording leaves one and the
        # fold-back has to claim it: notes into a new pattern, placed where it was played.
        state = run([{"arm": [0, True]}])
        assert state["channels"][0]["arm"], state["channels"][0]

        patterns_before = {p["id"] for p in state["patterns"]}
        take_pitches = [60, 63, 67, 70]
        state = run([{"take": [0, 16.0, 4.0] + take_pitches}, {"keep_takes": True}])

        kept = [p for p in state["patterns"] if p["id"] not in patterns_before]
        assert len(kept) == 1, [p["name"] for p in state["patterns"]]
        take = kept[0]
        assert round(take["length"], 3) == 4.0, take
        notes = notes_of(take, state["channels"][0]["id"])
        assert sorted(n["pitch"] for n in notes) == sorted(take_pitches), notes

        placement = next(c for c in state["playlist"]["clips"] if c["pattern"] == take["id"])
        assert round(placement["start"], 3) == 16.0, placement
        played = next(c for c in engine_clips(state, take["id"]))
        assert len(played["notes"]) == len(take_pitches), played
        assert round(played["start"], 3) == 16.0, played

        # One undo takes the take back out of the arrangement.
        control(project, "undo")
        time.sleep(0.5)
        assert not [c for c in settled(sub)["playlist"]["clips"] if c["pattern"] == take["id"]]

        state = run([{"arm": [0, False]}])
        assert not state["channels"][0]["arm"]

        # Render the arrangement, then the stems. A render runs on its own thread and
        # reports through render-status.json.
        def render(what):
            (sub / "render-status.json").unlink(missing_ok=True)
            run([{"export": what}])
            wait_for(lambda: read(sub / "render-status.json").get("running") is False, timeout=180)
            result = read(sub / "render-status.json")
            assert result["files"], result
            return result

        render("mix")
        mix = sub / "mix.wav"
        assert mix.exists(), "No mix was written"
        rendered = read_wav(mix)
        assert rendered["seconds"] > 10.0, rendered
        assert rendered["peak"] > 0.001, ("The render is silent", rendered)
        assert rendered["peak"] <= 1.0, ("The render clips", rendered)

        render("stems")
        stems = sorted((sub / "stems").glob("*.wav"))
        assert len(stems) == len(read(sub / "state.json")["channels"]), stems
        stem = read_wav(stems[0])
        assert stem["peak"] > 0.001 and stem["peak"] <= 1.0, stem

        # The automation is audible, and it beats the stored value: a curve on the
        # channel's own volume pulls the render down even though the model still says
        # the channel is at its usual level.
        volume_plugin = next(p for p in channel["parameters"] if p["id"] == "volume")
        data = read(sub / "state.json")
        data["automation"]["curves"].append(
            {"id": "fade", "source": channel["id"], "plugin_id": volume_plugin["plugin_id"],
             "parameter": "volume",
             "points": [{"id": "f0", "time": 0.0, "value": 0.05, "curve": 0.0},
                        {"id": "f1", "time": 32.0, "value": 0.05, "curve": 0.0}]})
        faded_state = submit(project, data)
        fade = next(c for c in faded_state["automation"]["curves"] if c["id"] == "fade")
        assert fade["engine_points"] == 2, fade
        assert faded_state["channels"][0]["gain_db"] == channel["gain_db"], \
            "The stored channel level should not have moved"

        render("mix")
        faded = read_wav(sub / "mix.wav")
        assert faded["rms"] < rendered["rms"] * 0.7, (faded, rendered)

        # Taking the curve away hands the parameter back to the stored value.
        data = read(sub / "state.json")
        data["automation"]["curves"] = [c for c in data["automation"]["curves"] if c["id"] != "fade"]
        submit(project, data)
        render("mix")
        restored_level = read_wav(sub / "mix.wav")
        assert restored_level["rms"] > faded["rms"] * 1.4, (restored_level, faded)

        assert read(sub / "sync-status.json")["session_id"] == session, "The project was reopened"

        control(project, "quit")
        assert process.wait(timeout=30) == 0

        # The curve comes back with the session.
        process = subprocess.Popen([str(exe), "--project", str(project), "--headless"], startupinfo=startup)
        wait_for(lambda: read(sub / "sync-status.json")["session_id"] != session, timeout=40)
        time.sleep(0.8)
        reopened = read(sub / "state.json")
        assert len(reopened["automation"]["curves"]) == 1, reopened["automation"]
        assert reopened["automation"]["curves"][0]["engine_points"] == 4, reopened["automation"]
        control(project, "quit")
        assert process.wait(timeout=20) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return "Automation is heard and beats the stored value, a take is kept, and mix and stems render as real audio"


def check_external_agent_session(exe, folder):
    """An outside agent works on a playing thirty-two bar song: it varies the drums,
    writes a bass line, rearranges a section and adjusts the mix. Each request is one
    undo, each reports what it changed, and a request that races a change made in the
    app is refused and succeeds after re-reading."""
    sub = folder / "agent"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    stage = {"round": 0}

    def run(actions):
        stage["round"] += 1
        # The app reloads the script when its contents change, so repeating the same
        # actions needs a marker to tell the rounds apart.
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=120)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status
        return settled(sub)

    def change():
        return read(sub / "sync-status.json")["change"]

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless", "--play",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "sync-status.json").get("playing"), timeout=40)
        session = read(sub / "sync-status.json")["session_id"]

        # A thirty-two bar song built in the app, so the agent works on real material.
        state = run([{"command": "New pattern"}, {"select_pattern": 1}, {"select_channel": 0}]
                    + [{"step": [0, step]} for step in (0, 4, 8, 12)])
        drums = state["patterns"][1]["id"]
        drum_channel = state["channels"][0]["id"]

        data = read(sub / "state.json")
        next(p for p in data["patterns"] if p["id"] == drums)["length"] = 16.0
        submit(project, data)

        state = run([{"select_pattern": 1}, {"select_lane": 0}]
                    + [{"place": [0, bar * 16.0]} for bar in range(8)])
        assert len(engine_clips(state, drums)) == 8
        assert read(sub / "sync-status.json")["playing"]

        # 1. Vary the drums: the same pattern, played busier.
        def busier(live):
            pattern = next(p for p in live["patterns"] if p["id"] == drums)
            notes = pattern["sequences"][0]["notes"]
            for index, beat in enumerate((2.0, 6.0, 10.0, 14.0)):
                notes.append({"id": "extra-%d" % index, "pitch": notes[0]["pitch"],
                              "velocity": 90, "start": beat, "length": 0.25})

        after, _ = apply_change(project, busier)
        assert len(notes_of(next(p for p in after["patterns"] if p["id"] == drums), drum_channel)) == 8
        assert drums in change()["patterns"]["changed"], change()
        assert all(len(clip["notes"]) == 8 for clip in engine_clips(after, drums)), \
            "The variation did not reach every placement"

        # 2. Write a bass line: a new channel, a new pattern and its placements.
        def bass(live):
            live["mixer"]["inserts"].append({"id": "bass-insert", "index": 9, "name": "Bass",
                                             "gain_db": -6.0, "pan": 0.0, "mute": False,
                                             "output": "master", "effects": [], "sends": []})
            live["channels"].append({"id": "bass", "name": "Bass", "gain_db": -8.0, "pan": 0.0,
                                     "mute": False, "solo": False, "insert": 9, "instrument": "4osc",
                                     "sample": "", "step_pitch": 40, "step_length": 0.5,
                                     "arm": False, "parameters": []})
            live["patterns"].append({"id": "bass-pattern", "name": "Bass", "length": 16.0,
                                     "sequences": [{"channel": "bass", "notes": [
                                         {"id": "b%d" % i, "pitch": 40 + (i % 2) * 5, "velocity": 100,
                                          "start": i * 2.0, "length": 1.5} for i in range(8)]}]})
            live["playlist"]["lanes"].append({"id": "bass-lane", "name": "Bass", "mute": False})
            for bar in range(8):
                live["playlist"]["clips"].append({"id": "bass-clip-%d" % bar, "lane": "bass-lane",
                                                  "pattern": "bass-pattern", "start": bar * 16.0,
                                                  "length": 16.0, "offset": 0.0})

        after, _ = apply_change(project, bass)
        assert len(engine_clips(after, "bass-pattern")) == 8, "The bass never played"
        assert "bass" in change()["channels"]["added"], change()
        assert "bass-pattern" in change()["patterns"]["added"], change()
        assert len(change()["clips"]["added"]) == 8, change()

        # 3. Rearrange a section: drop the last four bars of drums and repeat bars 1-4.
        def rearrange(live):
            clips = [c for c in live["playlist"]["clips"] if c["pattern"] == drums]
            for clip in sorted(clips, key=lambda c: c["start"])[4:]:
                clip["start"] = clip["start"] - 64.0 + 128.0

        after, _ = apply_change(project, rearrange)
        moved = sorted(c["start"] for c in after["playlist"]["clips"] if c["pattern"] == drums)
        assert moved[-1] == 176.0, moved
        assert len(change()["clips"]["changed"]) == 4, change()

        # 4. Adjust the mix: quieter bass, and a limiter on its insert.
        def mix(live):
            insert = next(i for i in live["mixer"]["inserts"] if i["id"] == "bass-insert")
            insert["gain_db"] = -12.0
            insert["effects"].append({"id": "bass-limit", "type": "limiter", "bypass": False,
                                      "wet": 1.0, "parameters": []})

        after, _ = apply_change(project, mix)
        insert = next(i for i in after["mixer"]["inserts"] if i["id"] == "bass-insert")
        assert insert["gain_db"] == -12.0 and [e["type"] for e in insert["effects"]] == ["limiter"]
        assert "bass-insert" in change()["inserts"]["changed"], change()
        assert any(p["type"] == "compressor"
                   for p in engine_track(after, "insert:bass-insert")["plugins"]), "The limiter is not there"

        # Each of the four is one undo, in reverse order.
        control(project, "undo")
        time.sleep(0.4)
        assert not next(i for i in settled(sub)["mixer"]["inserts"] if i["id"] == "bass-insert")["effects"]

        control(project, "undo")
        time.sleep(0.4)
        assert sorted(c["start"] for c in settled(sub)["playlist"]["clips"] if c["pattern"] == drums)[-1] == 112.0

        control(project, "undo")
        time.sleep(0.4)
        assert not [c for c in settled(sub)["playlist"]["clips"] if c["pattern"] == "bass-pattern"]

        control(project, "undo")
        time.sleep(0.4)
        assert len(notes_of(next(p for p in settled(sub)["patterns"] if p["id"] == drums), drum_channel)) == 4

        # An edit that races a change made in the app is refused, then succeeds on a
        # re-read; the app is untouched by the refused one.
        stale = read(sub / "state.json")
        run([{"command": "Add channel"}])
        stale["bpm"] = 101.0
        try:
            submit(project, stale)
            raise AssertionError("A stale edit was accepted")
        except Conflict:
            pass

        assert read(sub / "state.json")["bpm"] != 101.0, "The refused edit changed the project"
        after, _ = apply_change(project, lambda live: live.update({"bpm": 101.0}))
        assert after["bpm"] == 101.0
        assert change()["bpm"], change()

        assert read(sub / "sync-status.json")["session_id"] == session, "The project was reopened"
        assert read(sub / "sync-status.json")["playing"], "The agent's work stopped playback"

        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return "An outside agent varies the drums, writes a bass, rearranges and mixes a playing song, one undo each"


def check_every_menu_command(exe, folder):
    """Runs every menu item the app offers, each with the selection it needs, so a
    command that quietly does nothing shows up as a failure rather than a dead button.

    "Open project folder" is the one command left out: it opens Explorer, which is not
    something a check should be doing dozens of times.
    """
    sub = folder / "commands"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"

    # A pattern with notes on channel 0, placed on lane 0, and that clip selected.
    setup = ([{"command": "Add channel"}, {"command": "New pattern"}, {"select_pattern": 1},
              {"select_channel": 0}] + [{"step": [0, step]} for step in (0, 4, 8, 12)]
             + [{"select_lane": 0}, {"command": "Place pattern"}, {"pick_clip": [0, 0.0]}])

    # Each group runs with the state its commands need, re-established as it goes.
    groups = [
        ["Play / Stop", "Song mode", "Metronome", "Song mode", "Metronome", "Play / Stop"],
        ["Make placement unique", "Split clip at playhead", "Duplicate clip"],
        [{"select_pattern": 1}, {"select_channel": 0},
         "Transpose pattern up", "Transpose pattern down"],
        ["Arm channel for recording", "Count in one bar", "Arm channel for recording",
         "Count in one bar"],
        ["Show Browser", "Show Browser", "Show Channel Rack", "Show Channel Rack",
         "Show Mixer", "Show Mixer", "Show Pattern picker", "Show Pattern picker",
         "Show Playlist", "Show Playlist", "Focus next panel", "Focus next panel"],
        ["Add channel", "New pattern", {"select_lane": 0}, "Place pattern"],
        ["Undo", "Redo", "Save now", "Collect samples"],
    ]

    actions = list(setup)
    for group in groups:
        actions += [entry if isinstance(entry, dict) else {"command": entry} for entry in group]

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "state.json"), timeout=40)
        atomic_write(script, actions)

        wait_for(lambda: read(sub / "ui-script-status.json").get("finished"), timeout=240)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status
        assert status["done"] == status["total"], status

        state = settled(sub)
        assert not read(sub / "sync-status.json")["error"], read(sub / "sync-status.json")
        assert len(state["channels"]) == 3, state["channels"]
        assert all(read(sub / "ui-script-status.json").get("finished") for _ in range(1))

        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return "Every menu command runs against a real project without an error"


def check_survives_the_rough_edges(exe, folder):
    """Display scaling, a plugin that is not installed, an unclean exit, and a long
    run with the transport going."""
    sub = folder / "rough"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0

    def launch(extra=()):
        """Starts the app and waits until it is the one answering, so a leftover
        sync-status.json from the previous run is never mistaken for this one."""
        previous = None
        if (sub / "sync-status.json").exists():
            previous = read(sub / "sync-status.json")["session_id"]

        started = subprocess.Popen([str(exe), "--project", str(project), "--headless", "--screenshots",
                                    "--ui-script", str(script), *extra], startupinfo=startup)
        wait_for(lambda: (sub / "sync-status.json").exists()
                         and read(sub / "sync-status.json")["session_id"] != previous, timeout=60)
        return started

    # A project with something in it, and a screenshot at normal scaling.
    process = launch()
    try:
        atomic_write(script, [{"comment": 1}, {"command": "Add channel"}, {"command": "New pattern"},
                              {"select_pattern": 1}, {"select_channel": 0},
                              {"step": [0, 0]}, {"step": [0, 8]},
                              {"select_lane": 0}, {"command": "Place pattern"}])
        wait_for(lambda: read(sub / "ui-script-status.json").get("finished"), timeout=120)
        assert not read(sub / "ui-script-status.json")["error"], read(sub / "ui-script-status.json")
        state = settled(sub)
        baseline = read_png_size(sub / "ui.png")
        baseline_labels = len(read(sub / "ui-state.json")["labels"])
        # Every later launch reads this file too, so leave nothing in it to replay.
        atomic_write(script, [])
        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    # Two window sizes people actually have. What matters is that the panels are all
    # still there and the window fits what it was asked for, not that it looks the same.
    for size in ("1280x720", "1920x1080"):
        (sub / "ui.png").unlink(missing_ok=True)
        process = launch(("--size", size))
        try:
            wait_for(lambda: (sub / "ui.png").exists(), timeout=60)
            time.sleep(2.0)
            shot = read_png_size(sub / "ui.png")
            asked = [int(part) for part in size.split("x")]
            assert shot[0] <= asked[0] and shot[1] <= asked[1], (size, shot)
            assert len(read(sub / "ui-state.json")["labels"]) == baseline_labels,                 (size, read(sub / "ui-state.json")["labels"])
            assert not read(sub / "sync-status.json")["error"], (size, read(sub / "sync-status.json"))
            control(project, "quit")
            assert process.wait(timeout=30) == 0
            process = None
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    # The same project at 150% and 200%: the surface has to lay out, not just start.
    # The window is measured in its own units, so scaling it up makes those units
    # cover more of the screen; what has to hold is that the window still fits the
    # display and still has its panels, not that the number grows.
    for scale in ("1.5", "2.0"):
        (sub / "ui.png").unlink(missing_ok=True)
        process = launch(("--scale", scale))
        try:
            wait_for(lambda: (sub / "ui.png").exists(), timeout=60)
            time.sleep(2.0)
            scaled = read_png_size(sub / "ui.png")
            # How small the window ends up depends on the screen it is on, so the test
            # is not a number: it fits the display, it is not degenerate, and it still
            # lays out everything it lays out at 100%.
            assert scaled[0] <= baseline[0] and scaled[1] <= baseline[1], (scale, scaled, baseline)
            assert scaled[0] >= 240 and scaled[1] >= 160, (scale, scaled)
            labels = read(sub / "ui-state.json")["labels"]
            assert any("Live sync" in str(label) for label in labels), (scale, labels)
            assert len(labels) == baseline_labels, (scale, len(labels), baseline_labels)
            assert not read(sub / "sync-status.json")["error"], (scale, read(sub / "sync-status.json"))
            control(project, "quit")
            assert process.wait(timeout=30) == 0
            process = None
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    # A channel naming a plugin that is not installed stays silent and keeps the
    # request, so scanning it later fixes the project instead of losing the channel.
    process = launch()
    try:
        channel_id = settled(sub)["channels"][1]["id"]

        def ask_for_a_missing_plugin(live):
            next(c for c in live["channels"] if c["id"] == channel_id)["instrument"] = "VST3-NotInstalled-0-0"

        after, _ = apply_change(project, ask_for_a_missing_plugin)
        assert next(c for c in after["channels"] if c["id"] == channel_id)["instrument"] \
            in ("VST3-NotInstalled-0-0", "4osc"), after["channels"]
        assert engine_track(after, channel_id) is not None, "The channel lost its track"
        assert not read(sub / "sync-status.json")["error"], read(sub / "sync-status.json")

        # A long run with the transport going, saving as it goes.
        control(project, "play")
        deadline = time.time() + 20
        while time.time() < deadline:
            time.sleep(2)
            assert read(sub / "sync-status.json")["playing"], "Playback stopped during the long run"
            assert not read(sub / "sync-status.json")["error"], read(sub / "sync-status.json")

        before_kill = settled(sub)
        session = read(sub / "sync-status.json")["session_id"]
    finally:
        # An unclean exit: no quit, no save on the way out.
        process.kill()
        process.wait(timeout=20)
        process = None

    # What was published before the kill has to come back.
    process = launch()
    try:
        time.sleep(1.0)
        recovered = settled(sub)
        assert not read(sub / "sync-status.json")["error"], read(sub / "sync-status.json")
        assert len(recovered["channels"]) == len(before_kill["channels"]), (recovered["channels"],
                                                                           before_kill["channels"])
        assert len(recovered["patterns"]) == len(before_kill["patterns"])
        assert len(recovered["playlist"]["clips"]) == len(before_kill["playlist"]["clips"])
        assert len(engine_clips(recovered)) == len(engine_clips(before_kill))
        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return ("The surface lays out at 1280x720 and 1920x1080 and at 150% and 200%, a missing "
            "plugin is survivable, and a killed app reopens its work")


PNG_MAGIC = bytes([137, 80, 78, 71, 13, 10, 26, 10])


def read_png_size(path):
    """The app replaces the file while it runs, so keep looking until a whole one is there."""
    for _ in range(40):
        try:
            head = Path(path).read_bytes()[:33]
            if len(head) >= 24 and head[:8] == PNG_MAGIC:
                return struct.unpack(">II", head[16:24])
        except (PermissionError, FileNotFoundError):
            pass
        time.sleep(0.1)

    raise RuntimeError("No readable PNG at %s" % path)



# Windows file handles, used to make a replace fail on purpose and to see whether a
# file was ever taken away. Nothing here changes the app; it only watches or holds.
_GENERIC_READ = 0x80000000
_FILE_LIST_DIRECTORY = 0x0001
_SHARE_READ_ONLY = 0x00000001
_SHARE_ALL = 0x00000007
_OPEN_EXISTING = 3
_BACKUP_SEMANTICS = 0x02000000
_NOTIFY_FILE_NAME = 0x00000001
_INVALID_HANDLE = ctypes.c_void_p(-1).value
_kernel32 = ctypes.windll.kernel32
_kernel32.CreateFileW.restype = wintypes.HANDLE


def hold_against_replacement(path):
    """An open handle that shares nothing, so no rename or delete can take its name."""
    handle = _kernel32.CreateFileW(str(path), _GENERIC_READ, _SHARE_READ_ONLY, None,
                                   _OPEN_EXISTING, 0, None)
    if handle == _INVALID_HANDLE:
        raise OSError("could not hold %s: %d" % (path, ctypes.GetLastError()))
    return handle


class FolderWatch:
    """Records name changes in a folder, so a file that briefly stops existing is seen
    even though polling would miss it."""

    def __init__(self, folder):
        self.folder = folder
        self.events = []
        self._stop = threading.Event()
        self._handle = None
        self._thread = None

    def __enter__(self):
        self._handle = _kernel32.CreateFileW(str(self.folder), _FILE_LIST_DIRECTORY, _SHARE_ALL,
                                             None, _OPEN_EXISTING, _BACKUP_SEMANTICS, None)
        if self._handle == _INVALID_HANDLE:
            raise OSError("could not watch %s: %d" % (self.folder, ctypes.GetLastError()))
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        time.sleep(0.5)
        return self

    def __exit__(self, *unused):
        self._stop.set()
        _kernel32.CancelIoEx(self._handle, None)
        _kernel32.CloseHandle(self._handle)
        self._thread.join(timeout=5)
        return False

    def removed(self, name):
        return [entry for entry in self.events if entry == ("removed", name)]

    def _run(self):
        buffer = ctypes.create_string_buffer(64 * 1024)
        written = wintypes.DWORD()
        actions = {1: "added", 2: "removed", 3: "modified", 4: "renamed_from", 5: "renamed_to"}
        while not self._stop.is_set():
            if not _kernel32.ReadDirectoryChangesW(self._handle, buffer, len(buffer), False,
                                                   _NOTIFY_FILE_NAME, ctypes.byref(written), None, None):
                return
            offset = 0
            while True:
                raw = buffer.raw
                step = int.from_bytes(raw[offset:offset + 4], "little")
                action = int.from_bytes(raw[offset + 4:offset + 8], "little")
                length = int.from_bytes(raw[offset + 8:offset + 12], "little")
                name = raw[offset + 12:offset + 12 + length].decode("utf-16-le")
                self.events.append((actions.get(action, action), name))
                if not step:
                    break
                offset += step


def check_render_output_is_never_lost(exe, folder):
    """Exporting must not lose work. Two channels that share a display name have to
    produce two files, a stem has to carry the sends its channel feeds, and a render
    that cannot write must leave the last good file alone."""
    sub = folder / "render-safety"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    stage = {"round": 0}

    def run(actions):
        stage["round"] += 1
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=180)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status
        return settled(sub)

    def render(what):
        (sub / "render-status.json").unlink(missing_ok=True)
        run([{"export": what}])
        wait_for(lambda: read(sub / "render-status.json").get("running") is False, timeout=240)
        return read(sub / "render-status.json")

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "state.json"), timeout=40)
        run([{"command": "Add channel"}, {"command": "New pattern"}, {"select_pattern": 1}])

        # Two channels playing different notes, sharing a display name, and a reverb
        # bus the first one only reaches through a send.
        def build(live):
            live["channels"][0]["name"] = "Lead"
            live["channels"][1]["name"] = "Lead"
            pattern = live["patterns"][1]
            pattern["length"] = 16.0
            pattern["sequences"] = [
                {"channel": live["channels"][0]["id"], "notes": [
                    {"id": "a%d" % i, "pitch": 60, "velocity": 110, "start": i * 2.0, "length": 1.0}
                    for i in range(8)]},
                {"channel": live["channels"][1]["id"], "notes": [
                    {"id": "b%d" % i, "pitch": 48, "velocity": 110, "start": i * 2.0 + 1.0, "length": 1.0}
                    for i in range(8)]}]
            live["playlist"]["clips"].append({"id": "place-1", "lane": live["playlist"]["lanes"][0]["id"],
                                              "pattern": pattern["id"], "start": 0.0, "length": 16.0,
                                              "offset": 0.0})
            live["mixer"]["inserts"].append({"id": "verb", "index": 7, "name": "Reverb",
                                             "gain_db": 0.0, "pan": 0.0, "mute": False,
                                             "output": "master",
                                             "effects": [{"id": "verb-fx", "type": "reverb",
                                                          "bypass": False, "wet": 1.0, "parameters": []}],
                                             "sends": []})
            dry = next(i for i in live["mixer"]["inserts"] if i["index"] == live["channels"][0]["insert"])
            dry["sends"] = [{"id": "to-verb", "target": "verb", "level": 0.0}]

        state, _ = apply_change(project, build)
        assert [c["name"] for c in state["channels"]] == ["Lead", "Lead"], state["channels"]
        assert len(engine_clips(state)) >= 2, "The two channels are not both playing"

        # R1: two channels with the same name must not land on one path.
        result = render("stems")
        assert len(result["files"]) == 2, result
        assert len(set(result["files"])) == 2, ("Two stems landed on one path", result)
        for path in result["files"]:
            assert Path(path).exists(), path
            assert read_wav(Path(path))["peak"] > 0.001, path

        # R3: the stem of the channel that feeds the reverb has to carry that send, so
        # taking the send away has to change its file.
        with_send = {path: hashlib.sha256(Path(path).read_bytes()).digest() for path in result["files"]}

        def drop_the_send(live):
            for insert in live["mixer"]["inserts"]:
                insert["sends"] = []

        apply_change(project, drop_the_send)
        result = render("stems")
        assert len(result["files"]) == 2, result
        without_send = {path: hashlib.sha256(Path(path).read_bytes()).digest() for path in result["files"]}
        assert any(with_send.get(path) != digest for path, digest in without_send.items()), \
            "No stem changed when the send was removed, so sends are not in the stems"

        # R2: a render that cannot write must leave the last good file alone.
        mix = render("mix")
        assert len(mix["files"]) == 1, mix
        assert read_wav(sub / "mix.wav")["peak"] > 0.001
        before = hashlib.sha256((sub / "mix.wav").read_bytes()).digest()

        keep = sub / "mix-known-good.wav"
        shutil.copyfile(sub / "mix.wav", keep)
        (sub / "mix.wav").unlink()
        (sub / "mix.wav").mkdir()
        try:
            failed = render("mix")
            assert not failed["files"], ("A blocked render reported success", failed)
        finally:
            (sub / "mix.wav").rmdir()
            shutil.copyfile(keep, sub / "mix.wav")

        assert hashlib.sha256((sub / "mix.wav").read_bytes()).digest() == before, \
            "The previous mix was lost"
        assert not failed["files"], failed
        assert "mix.wav" in failed["message"], failed

        # V2-R1. Two separate things: a render that finished but could not take the
        # destination's place, and the destination never being taken away at all.
        #
        # First, make the swap impossible by holding the destination open. The file that
        # is already there has to come through untouched, and the render that did finish
        # has to be kept rather than thrown away with the failure.
        run([{"select_channel": 0}, {"note": [64, 1.0, 2.0, 100]}])
        handle = hold_against_replacement(sub / "mix.wav")
        try:
            blocked = render("mix")
        finally:
            _kernel32.CloseHandle(handle)

        assert not blocked["files"], ("A blocked replace reported success", blocked)
        assert "replace" in blocked["message"], ("A failed replace is not told apart from a "
                                                 "failed render", blocked)
        assert hashlib.sha256((sub / "mix.wav").read_bytes()).digest() == before, \
            "A failed replace lost the previous mix"

        kept = list(sub.glob("mix-rendering-*.wav"))
        assert len(kept) == 1, ("The finished render was thrown away with the failure", kept)
        assert kept[0].name in blocked["message"], blocked
        assert read_wav(kept[0])["peak"] > 0.001, "The kept render is not audio"
        kept[0].unlink()

        # Second, a replace that works must never remove the destination on the way. A
        # delete followed by a move leaves a window where losing power costs the last
        # good render; the folder itself is asked whether that window existed.
        with FolderWatch(sub) as watching:
            replaced = render("mix")
        assert replaced["files"], replaced
        assert not watching.removed("mix.wav"), \
            ("The destination was deleted before it was replaced", watching.events)
        assert hashlib.sha256((sub / "mix.wav").read_bytes()).digest() != before, \
            "The replacement did not actually land"

        # Editing while a render runs must not disturb either of them. The render works
        # from the project as it was when it started, and it says which revision that was.
        (sub / "render-status.json").unlink(missing_ok=True)
        started_at = read(sub / "sync-status.json")["revision"]
        run([{"export": "mix"}])

        # Edit while it runs. The loop used to be `while still running`, which meant that
        # on a machine fast enough to finish this render before the first read it never
        # ran at all, and the check then failed for having edited nothing - blaming the
        # app for the machine being quick. An edit is always attempted now, and whether
        # it actually overlapped the render is reported rather than demanded.
        edits = 0
        overlapped = False

        for _ in range(12):
            running = read(sub / "render-status.json").get("running")

            def nudge(live):
                live["bpm"] = 120.0 + (edits % 5)

            try:
                apply_change(project, nudge)
                edits += 1
                overlapped = overlapped or running is True
            except (Conflict, RuntimeError):
                pass

            control(project, "undo")

            if running is False:
                break

        wait_for(lambda: read(sub / "render-status.json").get("running") is False, timeout=240)
        rendered = read(sub / "render-status.json")
        assert rendered["files"], rendered
        assert rendered["complete"], rendered
        assert rendered["revision"] == started_at, (rendered, started_at)
        assert edits > 0, "Nothing was edited at all"

        during = read_wav(sub / "mix.wav")
        assert during["peak"] > 0.001 and during["seconds"] > 1.0, during

        # The project itself is still healthy and still the same session.
        assert not read(sub / "sync-status.json")["error"], read(sub / "sync-status.json")
        after_stress = settled(sub)
        assert len(after_stress["channels"]) == 2, after_stress["channels"]
        assert len(engine_clips(after_stress)) >= 2, "The arrangement lost clips during the render"

        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return ("Exports keep their names apart, carry a channel's sends, survive a failed write, "
            "keep a finished render whose swap failed without ever removing the file it was "
            "replacing, and render the project as it was while it keeps being edited")


def check_render_boundaries(exe, folder):
    """The edges around rendering: closing the app while one runs, asking for another
    before the last result was collected, and rendering a sound that was only just
    changed. Each of these is a moment where a result can end up attached to the wrong
    request, or a half-finished file can end up where a finished one was."""
    sub = folder / "render-edges"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    stage = {"round": 0}
    atomic_write(script, [])

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0

    def launch():
        return subprocess.Popen([str(exe), "--project", str(project), "--headless",
                                 "--ui-script", str(script)], startupinfo=startup)

    def run(actions, timeout=180):
        stage["round"] += 1
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=timeout)
        assert not read(sub / "ui-script-status.json")["error"], read(sub / "ui-script-status.json")
        return settled(sub)

    def export_and_wait(timeout=600):
        (sub / "render-status.json").unlink(missing_ok=True)
        run([{"export": "mix"}])
        wait_for(lambda: read(sub / "render-status.json").get("running") is False, timeout=timeout)
        return read(sub / "render-status.json")

    def build_a_long_song():
        # Long enough that a render takes a while, so there is something to interrupt.
        # The pattern keeps whatever length it was made with; the placements follow it.
        # Rendering runs far faster than real time, so length alone will not keep one
        # going long enough to interrupt. Weight is what does: sixteen channels all
        # playing at once take long enough that a second and a half in is still the
        # middle of the render, and the file stays a manageable size.
        run([{"command": "Add channel"}] * 15, timeout=240)
        state = settled(sub)
        for channel in range(len(state["channels"])):
            run([{"select_channel": channel}] + [{"step": [channel, step]} for step in range(0, 16)])

        def stretch(live):
            live["patterns"][0]["length"] = 200.0

        apply_change(project, stretch)
        run([{"select_lane": 0}, {"place": [0, 0.0]}])

    process = launch()
    try:
        wait_for(lambda: read(sub / "state.json"), timeout=60)
        build_a_long_song()

        # A first render, so there is a good file to lose.
        first = export_and_wait()
        assert first["files"], first
        good = hashlib.sha256((sub / "mix.wav").read_bytes()).digest()
        assert read_wav(sub / "mix.wav")["seconds"] > 30.0, "The song is too short to interrupt"
        assert first["complete"], first

        # Closing the app during a render. The engine stops the render when the thread
        # is asked to exit, and reports the part-written file because it exists on disk.
        # What must not happen is that file taking the finished one's place.
        # Rendering runs so much faster than real time that no sleep is short enough to
        # land inside one. The quit is therefore the next action in the same script: it
        # runs a tick after the export starts, which is reliably the middle of it.
        (sub / "render-status.json").unlink(missing_ok=True)
        stage["round"] += 1
        atomic_write(script, [{"comment": stage["round"]}, {"export": "mix"}, {"command": "Exit"}])
        assert process.wait(timeout=90) == 0, "The app did not close while a render was running"
        process = None

        interrupted = read(sub / "render-status.json")
        assert not interrupted.get("complete"), ("The render finished before the quit reached it; "
                                                 "this check proved nothing", interrupted)

        assert hashlib.sha256((sub / "mix.wav").read_bytes()).digest() == good, \
            "Closing during a render replaced the finished file with a part-written one"
        assert not list(sub.glob("mix-rendering-*.wav")), \
            "A stopped render left its part-written file behind"

        # The project itself has to come back intact after that.
        atomic_write(script, [])
        stage["round"] = 0
        process = launch()
        wait_for(lambda: read(sub / "state.json"), timeout=60)
        reopened = settled(sub)
        assert not read(sub / "sync-status.json")["error"], read(sub / "sync-status.json")
        assert len(reopened["channels"]) == 16, reopened["channels"]
        assert len(reopened["playlist"]["clips"]) == 2, reopened["playlist"]["clips"]
        assert reopened["patterns"][0]["length"] == 200.0, reopened["patterns"][0]

        # Two exports in a row, the second asked for the moment the first finishes. Each
        # result has to name the revision its own request was made against.
        (sub / "render-status.json").unlink(missing_ok=True)
        before = read(sub / "sync-status.json")["revision"]
        run([{"export": "mix"}])
        wait_for(lambda: read(sub / "render-status.json").get("running") is False, timeout=600)
        one = read(sub / "render-status.json")
        assert one["files"], one
        assert one["revision"] == before, (one, before)

        # A second export asked for while the first is still going has to be refused
        # rather than take the running one's place. The refusal is what keeps the result
        # attached to the request that asked for it.
        (sub / "render-status.json").unlink(missing_ok=True)
        stage["round"] += 1
        atomic_write(script, [{"comment": stage["round"]}, {"export": "mix"}, {"export": "mix"}])
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("error")), timeout=120)
        assert "export" in read(sub / "ui-script-status.json")["error"], read(sub / "ui-script-status.json")

        wait_for(lambda: read(sub / "render-status.json").get("running") is False, timeout=600)
        overlapped = read(sub / "render-status.json")
        assert overlapped["files"] and overlapped["complete"], \
            ("The refused second export disturbed the first", overlapped)
        assert read_wav(sub / "mix.wav")["seconds"] > 30.0, "The overlapped render was cut short"

        atomic_write(script, [])
        run([{"select_channel": 0}, {"note": [67, 2.0, 2.0, 100]}])
        after = read(sub / "sync-status.json")["revision"]
        assert after != before, "The edit between renders did not change the revision"
        two = export_and_wait()
        assert two["files"], two
        assert two["revision"] == after, (two, after)
        assert hashlib.sha256((sub / "mix.wav").read_bytes()).digest() != good, \
            "The second render did not replace the first"

        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return ("Closing during a render keeps the last finished file and leaves nothing half "
            "written, a second export while one runs is refused rather than merged, and each "
            "render reports the revision its own request was made against")


def check_recording_and_recovery(exe, folder):
    """Stopping a recording from outside has to keep the take just as the app does, the
    project has to keep rolling backups, and a session that is gone has to come back
    from the newest one."""
    sub = folder / "recovery"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    stage = {"round": 0}

    def run(actions):
        stage["round"] += 1
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=180)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status
        return settled(sub)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "state.json"), timeout=40)

        # Recording is started from outside and stopped from outside. There is no MIDI
        # keyboard here, so a take is put on the armed track the way a recording leaves
        # one; stopping has to claim it whichever way the stop arrived.
        state = run([{"arm": [0, True]}])
        assert state["channels"][0]["arm"], state["channels"][0]
        patterns_before = {p["id"] for p in state["patterns"]}

        # The transport can only record where there is an audio device. Where there is
        # one, the whole path is driven from outside; where there is not, the same
        # fold-back is exercised directly and the transport part is reported as skipped.
        control(project, "record")
        transport_records = False
        try:
            wait_for(lambda: read(sub / "sync-status.json").get("recording"), timeout=15)
            transport_records = True
        except TimeoutError:
            pass

        run([{"take": [0, 8.0, 4.0, 62, 65, 69]}])

        if transport_records:
            control(project, "stop")
        else:
            run([{"keep_takes": True}])

        time.sleep(1.0)

        state = settled(sub)
        kept = [p for p in state["patterns"] if p["id"] not in patterns_before]
        assert len(kept) == 1, ("The take was lost", [p["name"] for p in state["patterns"]])
        take = kept[0]
        assert sorted(n["pitch"] for n in notes_of(take, state["channels"][0]["id"])) == [62, 65, 69]
        placement = next(c for c in state["playlist"]["clips"] if c["pattern"] == take["id"])
        assert round(placement["start"], 3) == 8.0, placement
        assert not read(sub / "sync-status.json")["recording"], "The transport is still recording"

        # Whichever way the first take arrived, the other way has to keep a take too.
        # On a machine with a device the branch above never runs keep_takes, which is
        # how a take that was folded in but never written out went unnoticed here.
        if transport_records:
            backups_before = read(sub / "sync-status.json")["backups"]
            run([{"arm": [0, True]}, {"take": [0, 20.0, 4.0, 55, 59]}, {"keep_takes": True}])
            time.sleep(1.0)
            state = settled(sub)
            second = [p for p in state["patterns"]
                      if p["id"] not in patterns_before and p["id"] != take["id"]]
            assert len(second) == 1, ("The second take was lost", [p["name"] for p in state["patterns"]])
            assert read(sub / "sync-status.json")["backups"] != backups_before,                 "Keeping a take left it only in memory"

        # Backups accumulate as the work changes, newest first, and are capped.
        for beat in (16.0, 24.0, 32.0):
            run([{"place": [0, beat]}])
            time.sleep(1.0)

        wait_for(lambda: read(sub / "sync-status.json")["backups"], timeout=180)
        backups = read(sub / "sync-status.json")["backups"]
        assert backups == sorted(backups, reverse=True), backups
        assert len(backups) <= 10, backups

        before_loss = settled(sub)
        session = read(sub / "sync-status.json")["session_id"]
        assert not read(sub / "sync-status.json")["recovered_from"], read(sub / "sync-status.json")
    finally:
        process.kill()
        process.wait(timeout=20)
        process = None

    # The session file is lost. The newest backup has to bring the work back.
    (sub / "session.tracktionedit").unlink()
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless"], startupinfo=startup)
    try:
        wait_for(lambda: (sub / "sync-status.json").exists()
                         and read(sub / "sync-status.json")["session_id"] != session, timeout=60)
        time.sleep(1.0)
        recovered = settled(sub)
        assert read(sub / "sync-status.json")["recovered_from"], read(sub / "sync-status.json")
        assert not read(sub / "sync-status.json")["error"], read(sub / "sync-status.json")
        assert len(recovered["channels"]) == len(before_loss["channels"]), recovered["channels"]
        assert any(p["id"] == take["id"] for p in recovered["patterns"]), "The take did not survive"
        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    # A sample that has gone missing is named, not silently dropped. A fresh process
    # counts its script rounds from zero, and would replay whatever is still in the
    # file, so it starts empty.
    sample = write_wav(sub / "tone.wav")
    atomic_write(script, [])
    stage["round"] = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless",
                                "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "state.json"), timeout=40)
        stage["round"] += 1
        atomic_write(script, [{"comment": stage["round"]}, {"select_lane": 0},
                              {"audio": [0, str(sample), 0.0]}])
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=120)
        assert not read(sub / "ui-script-status.json")["error"], read(sub / "ui-script-status.json")
        settled(sub)
        assert not read(sub / "sync-status.json")["missing_assets"], read(sub / "sync-status.json")

        sample.unlink()
        wait_for(lambda: "tone.wav" in read(sub / "sync-status.json")["missing_assets"], timeout=30)

        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return ("A take survives an outside stop, backups roll, a lost session recovers, and a missing "
            "sample is named" + ("" if transport_records
                                 else " (no audio device: the recording transport was not exercised)"))


def check_the_last_editing_gaps(exe, folder):
    """The three things the surface could not do on its own: bend an automation curve,
    map a knob to a parameter, and be told why a preset does not fit. Each is driven the
    way the mouse and the menu drive it, and each is checked against the engine rather
    than against the model that asked for it."""
    sub = folder / "editing-gaps"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    stage = {"round": 0}
    atomic_write(script, [])

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0

    def launch():
        return subprocess.Popen([str(exe), "--project", str(project), "--headless",
                                 "--screenshots", "--ui-script", str(script)], startupinfo=startup)

    def run(actions, timeout=180):
        stage["round"] += 1
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=timeout)
        assert not read(sub / "ui-script-status.json")["error"], read(sub / "ui-script-status.json")
        return settled(sub)

    def parameter_of(state, which):
        return next(p for p in state["channels"][0]["parameters"]
                    if p["plugin_name"] == "Volume & Pan Plugin" and p["id"] == which)

    process = launch()
    try:
        wait_for(lambda: read(sub / "state.json"), timeout=60)
        run([{"select_channel": 0}] + [{"step": [0, step]} for step in (0, 4, 8, 12)])
        state = run([{"select_lane": 0}, {"place": [0, 0.0]}])
        target = parameter_of(state, "volume")
        channel = state["channels"][0]["id"]

        # --- an automation curve with a shape, not just corners ----------------------
        run([{"automate": [0, target["plugin_id"], target["id"]]},
             {"curve_click": [0, 0.0, 0.1]}, {"curve_click": [0, 8.0, 0.9]}])
        straight = settled(sub)["automation"]["curves"][0]
        assert straight["parameter_found"] and straight["engine_points"] == 2, straight
        assert len(straight["engine_samples"]) == 9, straight

        # A segment with no shape reads as a straight line all the way across.
        middle = straight["engine_samples"][4]
        expected = (straight["engine_samples"][0] + straight["engine_samples"][-1]) / 2.0
        assert abs(middle - expected) < 0.01, ("A curve with no shape is not straight", straight)

        run([{"curve_bend": [0, 4.0, 0.8]}])
        bent = settled(sub)["automation"]["curves"][0]
        assert bent["points"][0]["curve"] > 0.3, ("The drag did not bend the segment", bent)
        assert bent["engine_samples"][0] == straight["engine_samples"][0], bent
        assert bent["engine_samples"][-1] == straight["engine_samples"][-1], bent
        assert abs(bent["engine_samples"][4] - middle) > 0.05, ("The shape never reached the engine",
                                                                straight["engine_samples"],
                                                                bent["engine_samples"])

        # Bending the other way has to go the other way.
        run([{"curve_bend": [0, 4.0, -1.6]}])
        other = settled(sub)["automation"]["curves"][0]
        assert other["points"][0]["curve"] < -0.3, other
        assert other["engine_samples"][4] > bent["engine_samples"][4], (bent["engine_samples"],
                                                                        other["engine_samples"])

        # One Undo puts the shape back where it was.
        control(project, "undo")
        time.sleep(0.5)
        undone = settled(sub)["automation"]["curves"][0]
        assert undone["points"][0]["curve"] > 0.3, ("Undo did not put the shape back", undone)

        # --- a knob that moves a parameter ------------------------------------------
        assert settled(sub)["automation"]["midi_mappings"] == [], "Something was mapped already"
        run([{"midi_learn": [channel, target["plugin_id"], target["id"]]}])
        time.sleep(0.8)
        run([{"midi_cc": [74, 1, 0.5]}])
        time.sleep(1.5)
        learned = settled(sub)["automation"]["midi_mappings"]
        assert len(learned) == 1, ("The controller was not learnt", learned)
        assert learned[0]["controller"] == 74 and learned[0]["channel"] == 1, learned

        before = parameter_of(settled(sub), "volume")["value"]
        run([{"midi_cc": [74, 1, 0.25]}])
        time.sleep(1.0)
        assert abs(parameter_of(settled(sub), "volume")["value"] - before) > 0.01, \
            "The mapped knob moved nothing"

        # A learn that is armed and then cancelled must leave nothing behind.
        pan = parameter_of(settled(sub), "pan")
        run([{"midi_learn": [channel, pan["plugin_id"], pan["id"]]}])
        time.sleep(0.8)
        run([{"midi_learn_cancel": True}])
        time.sleep(0.8)
        after_cancel = settled(sub)["automation"]["midi_mappings"]
        assert [m["parameter"] for m in after_cancel] == ["volume"], \
            ("A cancelled learn left a mapping behind", after_cancel)

        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None

        # The mapping is part of the project, so it has to come back with it.
        atomic_write(script, [])
        stage["round"] = 0
        process = launch()
        wait_for(lambda: read(sub / "state.json"), timeout=60)
        time.sleep(1.0)
        restored = settled(sub)["automation"]["midi_mappings"]
        assert len(restored) == 1 and restored[0]["controller"] == 74, \
            ("The mapping did not survive a reopen", restored)

        moved = parameter_of(settled(sub), "volume")["value"]
        run([{"midi_cc": [74, 1, 0.9]}])
        time.sleep(1.0)
        assert abs(parameter_of(settled(sub), "volume")["value"] - moved) > 0.01, \
            "The restored mapping moves nothing"

        # --- a preset that does not fit says so -------------------------------------
        run([{"command": "Save instrument preset"}])
        time.sleep(0.5)
        run([{"command": "Load latest instrument preset"}])
        time.sleep(0.5)
        assert read(sub / "sync-status.json")["message"] == "Loaded the latest instrument preset",             read(sub / "sync-status.json")["message"]

        # Now the same preset against a different instrument. Refusing is right; refusing
        # without saying why is what this is here to stop.
        def to_sampler(live):
            live["channels"][0]["instrument"] = "sampler"

        apply_change(project, to_sampler)
        time.sleep(2.0)
        assert settled(sub)["channels"][0]["instrument"] == "sampler", settled(sub)["channels"][0]
        run([{"command": "Load latest instrument preset"}])
        time.sleep(0.8)
        refusal = read(sub / "sync-status.json")["message"]
        assert "is for" in refusal and "Sampler" in refusal,             ("A preset for another instrument was not explained", refusal)

        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return ("A curve is bent on screen and the engine plays the shape, a knob is learnt and "
            "still moves its parameter after a reopen, and a preset for another instrument "
            "says what it is for")


def check_a_song_made_only_on_screen(exe, folder):
    """Everything from a sample to a finished WAV, using only what the work surface
    offers: no JSON written by hand. Then an outside edit lands in that same open
    project while it plays."""
    sub = folder / "on-screen"
    sub.mkdir()
    project = sub / "project.json"
    script = sub / "ui-script.json"
    sample = write_wav(sub / "loop.wav", seconds=1.5, frequency=180.0)
    stage = {"round": 0}

    def run(actions):
        stage["round"] += 1
        atomic_write(script, [{"comment": stage["round"]}] + list(actions))
        wait_for(lambda: (read(sub / "ui-script-status.json").get("round") == stage["round"]
                          and read(sub / "ui-script-status.json").get("finished")), timeout=240)
        status = read(sub / "ui-script-status.json")
        assert not status["error"], status
        return settled(sub)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--project", str(project), "--headless",
                                "--screenshots", "--ui-script", str(script)], startupinfo=startup)
    try:
        wait_for(lambda: read(sub / "state.json"), timeout=40)

        # A drum channel and a bass channel, both written with the step grid.
        state = run([{"command": "Add channel"}, {"command": "New pattern"}, {"select_pattern": 1},
                     {"select_channel": 0}] + [{"step": [0, step]} for step in (0, 4, 8, 12)]
                    + [{"select_channel": 1}] + [{"step": [1, step]} for step in (0, 6, 10)])
        written = state["patterns"][1]
        assert len(written["sequences"]) == 2, written

        # A melody in the piano roll on a third channel.
        state = run([{"command": "Add channel"}, {"select_channel": 2}]
                    + [{"note": note} for note in ([72, 0.0, 1.0, 100], [76, 1.0, 1.0, 96],
                                                   [79, 2.0, 2.0, 104])])
        assert len(state["patterns"][1]["sequences"]) == 3, state["patterns"][1]

        # Eight bars of it in the playlist, and a sample dropped on a lane.
        state = run([{"select_lane": 0}] + [{"place": [0, bar * 4.0]} for bar in range(8)]
                    + [{"audio": [0, str(sample), 32.0]}])
        assert len(engine_clips(state, written["id"])) >= 3, "The arrangement did not reach the engine"
        assert len(state["playlist"]["audio"]) == 1, state["playlist"]["audio"]

        # A curve drawn on the grid, with the points placed, moved and one removed —
        # all through the same code a click and a drag use.
        volume = next(p for p in state["channels"][0]["parameters"] if p["id"] == "volume")
        state = run([{"select_channel": 0},
                     {"automate": [0, volume["plugin_id"], "volume"]},
                     {"curve_click": [0, 0.0, 0.2]},
                     {"curve_click": [0, 16.0, 0.9]},
                     {"curve_click": [0, 24.0, 0.5]},
                     {"curve_drag": [0, 24.0, 0.35]},
                     {"curve_click": [0, 28.0, 0.7]},
                     {"curve_remove": [0, 28.0]}])

        assert len(state["automation"]["curves"]) == 1, state["automation"]
        curve = state["automation"]["curves"][0]
        assert curve["parameter"] == "volume", curve
        assert [round(p["time"], 3) for p in sorted(curve["points"], key=lambda p: p["time"])] \
            == [0.0, 16.0, 24.0], curve["points"]
        moved = next(p for p in curve["points"] if round(p["time"], 3) == 24.0)
        assert abs(moved["value"] - 0.35) < 0.06, moved
        assert curve["engine_points"] == 3, curve

        # One undo takes the last point back, and the engine follows.
        control(project, "undo")
        time.sleep(0.5)
        after_undo = settled(sub)["automation"]["curves"][0]
        assert len(after_undo["points"]) == 4, after_undo["points"]
        control(project, "redo")
        time.sleep(0.5)
        assert len(settled(sub)["automation"]["curves"][0]["points"]) == 3

        # A preset keeps the instrument's settings and puts them on another channel.
        state = run([{"select_channel": 0}, {"command": "Save instrument preset"}])

        # Play it, then render it, from the surface.
        run([{"command": "Play / Stop"}])
        wait_for(lambda: read(sub / "sync-status.json")["playing"], timeout=30)

        (sub / "render-status.json").unlink(missing_ok=True)
        run([{"export": "mix"}])
        wait_for(lambda: read(sub / "render-status.json").get("running") is False, timeout=240)
        assert read(sub / "render-status.json")["files"], read(sub / "render-status.json")
        mix = read_wav(sub / "mix.wav")
        assert mix["peak"] > 0.001 and mix["peak"] <= 1.0, mix

        # And an outside edit still lands in the open project while it plays.
        session = read(sub / "sync-status.json")["session_id"]

        def outside(live):
            live["patterns"][1]["sequences"][0]["notes"].append(
                {"id": "from-outside", "pitch": 40, "velocity": 100, "start": 2.0, "length": 0.5})

        after, _ = apply_change(project, outside)
        assert any(n["id"] == "from-outside"
                   for n in after["patterns"][1]["sequences"][0]["notes"]), after["patterns"][1]
        assert read(sub / "sync-status.json")["session_id"] == session, "The project was reopened"

        control(project, "quit")
        assert process.wait(timeout=30) == 0
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

    return "A song written, arranged, automated and rendered from the surface alone, still open to outside edits"


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=root / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=root / "build-cocompose" / ("live-test-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    run(args.exe.resolve(), args.output.resolve())
