"""Checks the plugins actually installed on this machine against the real app.

Scans for VST3s, then for each one: puts it on a channel or an insert, reads its
parameters back from the engine, saves and reopens the project to see whether it
comes back, automates one parameter, and renders. Writes a table of what happened.

This needs a machine with plugins and an audio device. Where either is missing the
run reports that rather than passing.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time
import uuid
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import Conflict, apply_change, atomic_write, control, current, read, submit, wait_for


def start(exe, folder, script, extra=()):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    return subprocess.Popen([str(exe), "--project", str(folder / "project.json"), "--headless",
                             "--ui-script", str(script), *extra], startupinfo=startup)


def quit_app(process, folder):
    if process is None or process.poll() is not None:
        return
    try:
        control(folder / "project.json", "quit")
        process.wait(timeout=45)
    except Exception:
        process.kill()
        process.wait(timeout=20)


def running_apps():
    """The CoCompose processes alive right now, by pid."""
    try:
        listed = subprocess.run(["tasklist", "/FI", "IMAGENAME eq CoCompose.exe", "/NH"],
                                capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.SubprocessError):
        return []

    return [line.split()[1] for line in listed.splitlines()
            if line.lower().startswith("cocompose.exe")]


def wait_for_no_running_app(timeout=30):
    """Waits for the last app to finish leaving before starting the next one.

    Quitting is asked for and then happens; the process lingers for a moment after the
    check that asked has moved on. Starting into that moment is what produced timeouts
    that looked like hangs.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not running_apps():
            return True
        time.sleep(0.25)

    return False


class Session:
    """One run of the app, driven by the UI script, that can be restarted."""

    def __init__(self, exe, folder):
        self.exe = exe
        self.folder = folder
        self.script = folder / "ui-script.json"
        self.round = 0
        self.process = None

    def open(self, extra=()):
        # The app allows one instance. A second one hands its command line to the first
        # and exits at once, so it never writes a state.json of its own and the wait
        # below times out saying "no acknowledgement" - which reads as a hang and is
        # actually a collision with a process that has not finished leaving yet.
        #
        # That is not hypothetical: it is where the intermittent timeouts in this
        # project came from. A check whose app took an extra moment to exit made the
        # next check fail at startup, in a different file, for no visible reason.
        wait_for_no_running_app()

        atomic_write(self.script, [])
        self.round = 0
        self.process = start(self.exe, self.folder, self.script, extra)

        try:
            wait_for(lambda: read(self.folder / "state.json"), timeout=90)
        except TimeoutError:
            if self.process.poll() is not None:
                raise TimeoutError(
                    "CoCompose exited immediately - another instance was already "
                    "running and took the command line") from None
            raise

        return self

    def close(self):
        quit_app(self.process, self.folder)
        self.process = None

    def run(self, actions, timeout=180):
        self.round += 1
        atomic_write(self.script, [{"comment": self.round}] + list(actions))
        wait_for(lambda: (read(self.folder / "ui-script-status.json").get("round") == self.round
                          and read(self.folder / "ui-script-status.json").get("finished")), timeout=timeout)
        status = read(self.folder / "ui-script-status.json")
        if status["error"]:
            raise RuntimeError(status["error"])
        return self.settled()

    def settled(self, quiet=0.4, timeout=30):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            status = read(self.folder / "sync-status.json")
            state = read(self.folder / "state.json")
            if state["revision"] == status["revision"] and state["revision"] == last:
                return state
            last = status["revision"]
            time.sleep(quiet)
        raise TimeoutError("the project never settled")


def read_wav(path):
    with wave.open(str(path), "rb") as stream:
        frames = stream.getnframes()
        rate = stream.getframerate()
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
    rms = math.sqrt(sum((s / scale) ** 2 for s in samples) / max(1, len(samples)))
    return {"seconds": frames / rate, "peak": peak, "rms": rms}


def scan(session):
    session.run([{"scan": True}], timeout=60)
    wait_for(lambda: read(session.folder / "plugin-scan.json").get("running") is False, timeout=1800)
    return read(session.folder / "plugin-scan.json")


def prepare_song(session):
    """Something for the plugins to make a noise with: notes on a channel, placed."""
    state = session.run([{"command": "New pattern"}, {"select_pattern": 1}, {"select_channel": 0}]
                        + [{"step": [0, step]} for step in (0, 4, 8, 12)])
    pattern = state["patterns"][1]

    def stretch(live):
        next(p for p in live["patterns"] if p["id"] == pattern["id"])["length"] = 16.0

    apply_change(session.folder / "project.json", stretch)
    session.run([{"select_pattern": 1}, {"select_lane": 0}, {"place": [0, 0.0]}])
    return pattern["id"]


def check_instrument(session, plugin, pattern_id):
    """Puts the instrument on the channel and sees whether it survives a reopen."""
    project = session.folder / "project.json"
    notes = []

    def choose(live):
        live["channels"][0]["instrument"] = plugin["identifier"]

    state, _ = apply_change(project, choose)
    time.sleep(1.5)
    state = session.settled()

    loaded = state["engine"]["tracks"][0]["plugins"]
    on_track = any(p["type"] == "vst" or p["name"] == plugin["name"] for p in loaded)
    parameters = state["channels"][0]["parameters"]
    notes.append("%d parameters" % len(parameters))

    if not on_track:
        return {"loaded": False, "notes": "; ".join(notes + ["did not appear on the track"])}

    # The plugin's own editor: a plugin that loads but cannot show its window is not
    # usable, so it is asked for one and the engine is asked whether it came up.
    window_open = False
    try:
        session.run([{"select_channel": 0}, {"open_plugin": 0}])
        time.sleep(1.5)
        window_open = any(p["window_open"] for p in session.settled()["engine"]["tracks"][0]["plugins"])
        session.run([{"close_plugins": True}])
    except Exception as error:
        notes.append("window failed: %s" % error)

    automated = False
    if parameters:
        target = parameters[0]
        try:
            session.run([{"select_channel": 0},
                         {"automate": [0, target["plugin_id"], target["id"]]},
                         {"curve_click": [0, 0.0, 0.2]},
                         {"curve_click": [0, 8.0, 0.8]}])
            curve = session.settled()["automation"]["curves"]
            automated = bool(curve) and curve[0]["engine_points"] == 2
            if not automated:
                notes.append("automation: %s" % json.dumps(
                    [{"p": c["parameter"], "pts": len(c["points"]), "eng": c["engine_points"]} for c in curve]))
        except Exception as error:
            notes.append("automation failed: %s" % error)

    return {"loaded": True, "parameters": len(parameters), "window": window_open,
            "automated": automated, "notes": "; ".join(notes)}


def check_effect(session, plugin):
    """Puts the effect on the insert the first channel plays through."""
    project = session.folder / "project.json"
    effect_id = uuid.uuid4().hex

    def add(live):
        live["mixer"]["inserts"][0]["effects"].append(
            {"id": effect_id, "type": plugin["identifier"], "bypass": False, "wet": 1.0,
             "parameters": []})

    apply_change(project, add)
    time.sleep(1.5)
    state = session.settled()

    effect = next((e for e in state["mixer"]["inserts"][0]["effects"] if e["id"] == effect_id), None)
    if effect is None:
        return {"loaded": False, "notes": "the insert did not keep the effect"}

    on_track = any(p["effect"] == effect_id
                   for track in state["engine"]["tracks"] for p in track["plugins"])
    parameters = effect["parameters"]
    notes = ["%d parameters" % len(parameters)]

    if not on_track:
        return {"loaded": False, "parameters": len(parameters),
                "notes": "; ".join(notes + ["did not appear on the insert track"])}

    # Bypassing has to reach the engine, which is what tells us the plugin is really
    # in the chain rather than merely constructed.
    def bypass(live):
        next(e for e in live["mixer"]["inserts"][0]["effects"] if e["id"] == effect_id)["bypass"] = True

    apply_change(project, bypass)
    time.sleep(1.0)
    after = session.settled()
    enabled = [p["enabled"] for track in after["engine"]["tracks"]
               for p in track["plugins"] if p["effect"] == effect_id]
    notes.append("bypass reaches the engine" if enabled == [False] else "bypass did not reach the engine")

    def unbypass(live):
        next(e for e in live["mixer"]["inserts"][0]["effects"] if e["id"] == effect_id)["bypass"] = False

    apply_change(project, unbypass)
    return {"loaded": True, "parameters": len(parameters), "bypass": enabled == [False],
            "notes": "; ".join(notes)}


def restart_and_verify(session, plugin):
    """Reopens the project and reports whether the plugin and its settings came back."""
    session.close()
    session.open()
    state = session.settled()

    loaded = state["engine"]["tracks"][0]["plugins"]
    back = any(p["name"] == plugin["name"] for p in loaded)
    parameters = len(state["channels"][0]["parameters"])
    curves = state["automation"]["curves"]
    automation_back = bool(curves) and curves[0]["engine_points"] == 2
    return {"restored": back, "parameters": parameters, "automation_restored": automation_back}


def restart_and_verify_effect(session):
    """Reopens the project and reports whether the effect came back on the insert."""
    session.close()
    session.open()
    state = session.settled()
    effect = state["mixer"]["inserts"][0]["effects"]
    on_track = any(p["effect"] == effect[0]["id"]
                   for track in state["engine"]["tracks"] for p in track["plugins"]) if effect else False
    return {"restored": on_track,
            "parameters_restored": len(effect[0]["parameters"]) if effect else 0}


def render(session):
    folder = session.folder
    (folder / "render-status.json").unlink(missing_ok=True)
    session.run([{"export": "mix"}])
    wait_for(lambda: read(folder / "render-status.json").get("running") is False, timeout=600)
    result = read(folder / "render-status.json")
    if not result["files"]:
        return {"rendered": False, "notes": result["message"]}

    measured = read_wav(folder / "mix.wav")
    return {"rendered": True, "peak": round(measured["peak"], 4),
            "rms": round(measured["rms"], 5), "seconds": round(measured["seconds"], 2)}


def run(exe, output, limit):
    output.mkdir(parents=True, exist_ok=True)
    scan_folder = output / "scan"
    scan_folder.mkdir(exist_ok=True)

    session = Session(exe, scan_folder).open()
    try:
        catalogue = scan(session)
    finally:
        session.close()

    instruments = [p for p in catalogue["found"] if p["instrument"]]
    effects = [p for p in catalogue["found"] if not p["instrument"]]
    if limit:
        instruments = instruments[:limit]
        effects = effects[:limit]

    report = {"scanned_files": catalogue["scanned"], "plugins_found": len(catalogue["found"]),
              "device": None, "rows": [], "effect_rows": []}

    for plugin in effects:
        folder = output / ("effect-" + "".join(c for c in plugin["name"] if c.isalnum())[:24])
        folder.mkdir(exist_ok=True)
        row = {"name": plugin["name"], "format": plugin["format"],
               "manufacturer": plugin["manufacturer"], "version": plugin["version"]}
        print("  testing effect", plugin["name"], flush=True)

        session = Session(exe, folder)
        try:
            session.open()
            prepare_song(session)
            # What the channel sounds like before the effect, so a silent render after
            # it can be told apart from a song that was silent anyway.
            row["dry_peak"] = render(session).get("peak")
            row.update(check_effect(session, plugin))
            if row.get("loaded"):
                row.update(render(session))
                row.update(restart_and_verify_effect(session))
        except Exception as error:
            row["error"] = "%s: %s" % (type(error).__name__, error)
        finally:
            session.close()

        report["effect_rows"].append(row)
        print("    ", json.dumps({k: v for k, v in row.items() if k != "name"}), flush=True)

    for plugin in instruments:
        folder = output / ("plugin-" + "".join(c for c in plugin["name"] if c.isalnum())[:24])
        folder.mkdir(exist_ok=True)
        row = {"name": plugin["name"], "format": plugin["format"],
               "manufacturer": plugin["manufacturer"], "version": plugin["version"]}
        print("  testing", plugin["name"], flush=True)

        session = Session(exe, folder)
        try:
            session.open()
            pattern_id = prepare_song(session)
            row.update(check_instrument(session, plugin, pattern_id))

            if row.get("loaded"):
                row.update(render(session))
                row.update(restart_and_verify(session, plugin))
            report["device"] = report["device"] or session.settled()["engine"].get("device")
        except Exception as error:
            row["error"] = "%s: %s" % (type(error).__name__, error)
        finally:
            session.close()

        report["rows"].append(row)
        print("    ", json.dumps({k: v for k, v in row.items() if k != "name"}), flush=True)

    atomic_write(output / "compatibility.json", report)
    print(json.dumps({"scanned_files": report["scanned_files"],
                      "plugins_found": report["plugins_found"],
                      "instruments_tested": len(report["rows"]),
                      "effects_tested": len(report["effect_rows"]),
                      "device": report["device"]}, indent=2))
    return report


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=root / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=root / "build-cocompose" / ("compat-" + uuid.uuid4().hex[:8]))
    parser.add_argument("--limit", type=int, default=0, help="only test this many instruments")
    args = parser.parse_args()
    run(args.exe.resolve(), args.output.resolve(), args.limit)
