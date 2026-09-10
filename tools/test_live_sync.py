"""Integration check against the real Windows app; no mock engine or GUI."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import time
import uuid
from xml.etree import ElementTree

from cocompose import atomic_write, control, current, read, submit, wait_for

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
        checks.append("Partial JSON preserves previous live state")

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
        assert [c["id"] for c in result["channels"]] == ["second", channel_id]
        assert [t["channel"] for t in result["engine"]["tracks"]] == ["second", channel_id], \
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
        for section in ("channels", "patterns", "playlist", "mixer", "engine"):
            assert equivalent(reopened[section], final[section]), \
                "Saved %s changed beyond floating-point roundoff" % section
        assert reopened["bpm"] == final["bpm"]
        checks.append("Graceful exit and native session save/reload preserve music and plugin state")
        control(project, "quit")
        assert process.wait(timeout=15) == 0

        checks.append(check_legacy_session(launch, folder))
        checks.append(check_legacy_json_input(launch, folder))
        checks.append(check_workspace_layout(launch, folder))
        checks.append(check_pattern_built_in_the_ui(exe, folder))

        report = {"passed": checks, "folder": str(folder), "executable": str(exe)}
        atomic_write(folder / "test-report.json", report)
        print(json.dumps(report, indent=2))
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
    assert len(sizes) == 4 and all(size > 1.0 for size in sizes), layout.get("sizes")
    assert layout.get("visible") == "11111", layout.get("visible")
    assert layout.get("selectedChannel") == state["channels"][0]["id"]
    assert layout.get("selectedPattern") == state["patterns"][0]["id"]
    assert layout.get("selectedLane") == state["playlist"]["lanes"][0]["id"]

    # Hide the Mixer and the Playlist and narrow the browser, the way the View menu and
    # the resizer bars do, then confirm the next run honours it instead of resetting.
    tree = ElementTree.parse(session)
    edited = tree.getroot().find("COCOMPOSELAYOUT")
    edited.set("visible", "11010")
    edited.set("sizes", "160 %s %s %s" % tuple(str(size) for size in sizes[1:]))
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
        assert status["done"] == len(actions), status

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


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=root / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=root / "build-cocompose" / ("live-test-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    run(args.exe.resolve(), args.output.resolve())
