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

from cocompose import atomic_write, control, current, read, submit, wait_for


def equivalent(a, b):
    if isinstance(a, float) and isinstance(b, (float, int)):
        return math.isclose(a, b, rel_tol=0, abs_tol=1e-8)
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(equivalent(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(equivalent(x, y) for x, y in zip(a, b))
    return a == b


def run(exe, folder):
    folder.mkdir(parents=True, exist_ok=False)
    project = folder / "project.json"
    process = None
    checks = []

    def launch(play=False):
        command = [str(exe), "--project", str(project), "--headless", "--screenshots"]
        if play:
            command.append("--play")
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        return subprocess.Popen(command, startupinfo=startup)

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
        assert len(first["tracks"]) == 1
        assert len(first["tracks"][0]["clips"][0]["notes"]) == 32
        wait_for(lambda: (folder / "ui.png").exists())
        shutil.copyfile(folder / "ui.png", folder / "before.png")
        checks.append("Windows startup and real Tracktion playback")

        data = copy.deepcopy(first)
        data["bpm"] = 137
        data["tracks"][0]["name"] = "Live sync verified"
        data["tracks"][0]["clips"][0]["notes"][0]["pitch"] = 71
        data["tracks"][0]["clips"][0]["notes"][0]["velocity"] = 103
        changed = submit(project, data)
        assert changed["bpm"] == 137
        assert changed["tracks"][0]["clips"][0]["notes"][0]["pitch"] == 71
        assert changed["tracks"][0]["clips"][0]["notes"][0]["velocity"] == 103
        assert (status()["session_id"], status()["edit_instance"]) == identity
        assert status()["playing"], "Live edit stopped playback"
        assert changed["tracks"][0]["parameters"][0]["plugin_id"] == first["tracks"][0]["parameters"][0]["plugin_id"]
        before_hash = hashlib.sha256((folder / "before.png").read_bytes()).digest()
        wait_for(lambda: "Live sync verified" in read(folder / "ui-state.json")["labels"])
        wait_for(lambda: hashlib.sha256((folder / "ui.png").read_bytes()).digest() != before_hash)
        shutil.copyfile(folder / "ui.png", folder / "after.png")
        checks.append("Tempo, track name, pitch and velocity update without replacing Edit; UI changes")

        invalid = live_state()
        invalid["tracks"][0]["clips"][0]["notes"][0]["velocity"] = 999
        rejected(invalid, "velocity out of range")
        checks.append("Invalid MIDI rejected before mutation")

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
        notes_before = copy.deepcopy(restored["tracks"][0]["clips"][0]["notes"])
        data = live_state()
        data["tracks"][0]["clips"][0]["notes"] = []
        assert not submit(project, data)["tracks"][0]["clips"][0]["notes"]
        control(project, "undo")
        assert live_state()["tracks"][0]["clips"][0]["notes"] == notes_before
        control(project, "redo")
        assert not live_state()["tracks"][0]["clips"][0]["notes"]
        control(project, "undo")
        checks.append("Empty notes clears MIDI; one Undo restores; Redo reapplies")

        data = live_state()
        param = next(p for p in data["tracks"][0]["parameters"] if p["id"] == "filterFreq")
        old_value = param["value"]
        param["value"] = 0.31
        updated = submit(project, data)
        actual = next(p for p in updated["tracks"][0]["parameters"] if p["id"] == "filterFreq")
        assert abs(actual["value"] - 0.31) < 0.01
        control(project, "undo")
        actual = next(p for p in live_state()["tracks"][0]["parameters"] if p["id"] == "filterFreq")
        assert abs(actual["value"] - old_value) < 0.01
        checks.append("Synth parameter write and Undo verified against engine readback")

        data = live_state()
        data["tracks"].insert(0, {"id": "second", "name": "New track", "gain_db": -18,
                                  "mute": False, "solo": False, "clips": [], "parameters": []})
        result = submit(project, data)
        assert [t["id"] for t in result["tracks"]] == ["second", "synth-1"]
        data = live_state()
        data["tracks"] = [t for t in data["tracks"] if t["id"] != "second"]
        submit(project, data)
        assert (status()["session_id"], status()["edit_instance"]) == identity
        checks.append("Track insertion, reordering and removal preserve live Edit")

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
        assert equivalent(reopened["tracks"], final["tracks"]), "Saved music changed beyond floating-point roundoff"
        assert reopened["bpm"] == final["bpm"]
        checks.append("Graceful exit and native session save/reload preserve music and plugin state")
        control(project, "quit")
        assert process.wait(timeout=15) == 0
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


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=root / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=root / "build-cocompose" / ("live-test-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    run(args.exe.resolve(), args.output.resolve())
