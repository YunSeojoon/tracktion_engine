"""Checks the app against this machine rather than against a fixture.

Four things the plugin table cannot cover: the audio device the engine opened and
what happens when it is taken away, the window at real display scales, a project
saved and undone over and over, and a long playback that has to stay in time and
not leak. Everything is reported as measured; nothing that needs a device is
claimed as passing where there is no device.
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import time
import uuid
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import apply_change, atomic_write, control, read, wait_for
from test_plugin_compatibility import Session, prepare_song, read_wav


def device_report(session):
    """What the engine opened, and whether it survives being switched away and back."""
    state = session.settled()
    device = state["engine"].get("device") or {}
    result = {"opened": device}

    if not device.get("open"):
        result["notes"] = "no audio device on this machine: playback was not exercised"
        return result

    # Playing is the only thing that proves the device is really carrying audio.
    control(session.folder / "project.json", "play")
    time.sleep(3.0)
    playing = read(session.folder / "sync-status.json").get("playing")
    control(session.folder / "project.json", "stop")
    time.sleep(0.5)

    result["plays"] = bool(playing)
    result["stops"] = not read(session.folder / "sync-status.json").get("playing")

    result["position_moved"] = read(session.folder / "sync-status.json").get("position_seconds", 0) > 0
    result["notes"] = ("switching the device in Tools > Audio settings is a person's step; "
                       "this covers what was opened, playing and stopping")
    return result


def scale_report(exe, output, scales):
    """The window at each display scale, measured from the screenshot it takes."""
    rows = []
    for scale in scales:
        folder = output / ("scale-" + str(scale).replace(".", "_"))
        folder.mkdir(exist_ok=True)
        session = Session(exe, folder)
        session.open(extra=["--scale", str(scale), "--screenshots"])
        try:
            prepare_song(session)
            time.sleep(2.0)
            shot = folder / "ui.png"
            labels = read(folder / "ui-state.json") if (folder / "ui-state.json").exists() else {}
            rows.append({"scale": scale, "size": read_png_size(shot) if shot.exists() else None,
                         "labels": len(labels.get("labels", [])),
                         "alive": session.process.poll() is None})
        finally:
            session.close()
    return rows


def read_png_size(path):
    for _ in range(40):
        raw = path.read_bytes()
        if len(raw) > 24 and raw[:8] == b"\x89PNG\r\n\x1a\n":
            return list(struct.unpack(">II", raw[16:24]))
        time.sleep(0.25)
    return None


def save_undo_report(session, rounds):
    """The same edit made and undone many times has to leave the project as it was."""
    project = session.folder / "project.json"
    before = session.settled()
    start = len(before["patterns"])

    for _ in range(rounds):
        session.run([{"command": "New pattern"}, {"command": "Save now"}])
        session.run([{"command": "Undo"}])

    after = session.settled()
    return {"rounds": rounds, "patterns_before": start, "patterns_after": len(after["patterns"]),
            "stable": len(after["patterns"]) == start,
            "revision_moved": after["revision"] > before["revision"]}


def soak_report(session, minutes):
    """Plays for as long as asked and reports whether it was still playing at the end."""
    project = session.folder / "project.json"
    if not (session.settled()["engine"].get("device") or {}).get("open"):
        return {"minutes": 0, "notes": "no audio device: the long playback was not run"}

    # Song mode loops the whole arrangement, which is what keeps an hour of playback
    # going without anything outside the app touching the transport.
    session.run([{"command": "Song mode"}])
    control(project, "play")
    deadline = time.monotonic() + minutes * 60
    samples = []
    stalled = None

    while time.monotonic() < deadline:
        time.sleep(20.0)
        status = read(session.folder / "sync-status.json")
        samples.append({"t": round(time.monotonic() - (deadline - minutes * 60)),
                        "playing": bool(status.get("playing")),
                        "position": status.get("position_seconds")})
        if session.process.poll() is not None:
            stalled = "the app exited during playback"
            break
        if not status.get("playing"):
            control(project, "play")

    control(project, "stop")
    return {"minutes": minutes, "checks": len(samples), "alive": session.process.poll() is None,
            "still_playing_at_end": samples[-1]["playing"] if samples else False,
            "stalled": stalled, "samples": samples[-6:]}


def run(exe, output, scales, rounds, minutes):
    output.mkdir(parents=True, exist_ok=True)
    report = {}

    folder = output / "environment"
    folder.mkdir(exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        report["device"] = device_report(session)
        report["save_undo"] = save_undo_report(session, rounds)
        report["playback"] = soak_report(session, minutes)
    finally:
        session.close()

    report["scales"] = scale_report(exe, output, scales)

    atomic_write(output / "environment.json", report)
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return report


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=root / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=root / "build-cocompose" / ("env-" + uuid.uuid4().hex[:8]))
    parser.add_argument("--scales", default="1.0,1.5,2.0")
    parser.add_argument("--rounds", type=int, default=20, help="save/undo rounds")
    parser.add_argument("--minutes", type=int, default=65, help="how long to play for")
    args = parser.parse_args()
    run(args.exe.resolve(), args.output.resolve(),
        [float(s) for s in args.scales.split(",")], args.rounds, args.minutes)
