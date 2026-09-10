"""Edit a running CoCompose session using only Python's standard library."""
import argparse
import json
import os
from pathlib import Path
import tempfile
import time
import uuid


def read(path):
    # Windows can briefly deny an open while the app atomically replaces a file.
    for attempt in range(20):
        try:
            return json.loads(Path(path).read_text(encoding="utf-8-sig"))
        except (PermissionError, FileNotFoundError):
            if attempt == 19:
                raise
            time.sleep(0.025)


def atomic_write(path, value):
    path = Path(path)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                     suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.flush()
        os.fsync(stream.fileno())
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def wait_for(check, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            result = check()
            if result:
                return result
        except (FileNotFoundError, json.JSONDecodeError, PermissionError):
            pass
        time.sleep(0.1)
    raise TimeoutError("No acknowledgement from CoCompose; check sync-status.json")


def current(project):
    folder = Path(project).parent
    status = read(folder / "sync-status.json")
    if abs(time.time() * 1000 - status["updated_at_ms"]) > 5000:
        raise RuntimeError("CoCompose is not responding. Start the app with this project.")
    state = read(folder / "state.json")
    return state, status


def submit(project, state):
    folder = Path(project).parent
    before, status = current(project)
    if state["revision"] != before["revision"]:
        raise RuntimeError("Stale revision; read state.json and reapply the edit")
    session = status["session_id"]
    request_id = str(uuid.uuid4())
    state["request_id"] = request_id
    atomic_write(project, state)

    def acknowledged():
        after = read(folder / "sync-status.json")
        if after["session_id"] != session:
            raise RuntimeError("Session changed while editing")
        if after.get("request_id") == request_id and after.get("error"):
            raise RuntimeError(after["error"])
        if after.get("request_id") == request_id and after["revision"] > state["revision"]:
            return read(folder / "state.json")
        return None

    try:
        return wait_for(acknowledged)
    except TimeoutError as exc:
        error = read(folder / "sync-status.json").get("error")
        raise RuntimeError(error or str(exc)) from exc


def control(project, action):
    state, status = current(project)
    folder = Path(project).parent
    request_id = str(uuid.uuid4())
    atomic_write(folder / "control.json", {"id": request_id, "action": action,
                 "session_id": status["session_id"], "revision": state["revision"]})

    def acknowledged():
        response = read(folder / "control-status.json")
        if response.get("id") != request_id:
            return None
        if response["error"]:
            raise RuntimeError(response["error"])
        return response

    return wait_for(acknowledged)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path,
                        default=Path.home() / "Documents/CoCompose/project.json")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("inspect")
    tempo = commands.add_parser("tempo")
    tempo.add_argument("bpm", type=float)
    transpose = commands.add_parser("transpose")
    transpose.add_argument("pattern_id")
    transpose.add_argument("semitones", type=int)
    clear = commands.add_parser("clear-notes")
    clear.add_argument("pattern_id")
    place = commands.add_parser("place", help="add a playlist placement of a pattern")
    place.add_argument("lane_id")
    place.add_argument("pattern_id")
    place.add_argument("start", type=float, help="start position in beats")
    unique = commands.add_parser("make-unique", help="detach one placement from its pattern")
    unique.add_argument("clip_id")
    gain = commands.add_parser("gain")
    gain.add_argument("channel_id")
    gain.add_argument("db", type=float)
    parameter = commands.add_parser("parameter")
    parameter.add_argument("channel_id")
    parameter.add_argument("plugin_id")
    parameter.add_argument("parameter_id")
    parameter.add_argument("value", type=float, help="normalised value from 0 to 1")
    for action in ("play", "stop", "undo", "redo", "quit"):
        commands.add_parser(action)
    args = parser.parse_args()
    if args.command in ("play", "stop", "undo", "redo", "quit"):
        result = control(args.project, args.command)
    else:
        state, _ = current(args.project)
        if args.command == "inspect":
            result = state
        else:
            if args.command == "tempo":
                state["bpm"] = args.bpm
            elif args.command in ("transpose", "clear-notes"):
                # A pattern is shared, so this changes every placement of it.
                pattern = next(p for p in state["patterns"] if p["id"] == args.pattern_id)
                for sequence in pattern["sequences"]:
                    if args.command == "clear-notes":
                        sequence["notes"] = []
                    else:
                        for note in sequence["notes"]:
                            note["pitch"] += args.semitones
            elif args.command == "place":
                pattern = next(p for p in state["patterns"] if p["id"] == args.pattern_id)
                next(l for l in state["playlist"]["lanes"] if l["id"] == args.lane_id)
                state["playlist"]["clips"].append(
                    {"id": str(uuid.uuid4()), "lane": args.lane_id, "pattern": args.pattern_id,
                     "start": args.start, "length": pattern["length"]})
            elif args.command == "make-unique":
                clip = next(c for c in state["playlist"]["clips"] if c["id"] == args.clip_id)
                shared = next(p for p in state["patterns"] if p["id"] == clip["pattern"])
                copy = json.loads(json.dumps(shared))
                copy["id"] = str(uuid.uuid4())
                copy["name"] = shared["name"] + " (unique)"
                for sequence in copy["sequences"]:
                    for note in sequence["notes"]:
                        note["id"] = str(uuid.uuid4())
                state["patterns"].append(copy)
                clip["pattern"] = copy["id"]
            elif args.command == "gain":
                next(c for c in state["channels"] if c["id"] == args.channel_id)["gain_db"] = args.db
            elif args.command == "parameter":
                channel = next(c for c in state["channels"] if c["id"] == args.channel_id)
                param = next(p for p in channel["parameters"]
                             if p["plugin_id"] == args.plugin_id and p["id"] == args.parameter_id)
                param["value"] = args.value
            result = submit(args.project, state)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, OSError, StopIteration, TimeoutError) as error:
        raise SystemExit(str(error) or "Requested channel, pattern, clip, or parameter ID was not found")
