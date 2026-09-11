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
        # Windows can briefly deny the replace while the app has the file open.
        for attempt in range(40):
            try:
                os.replace(temporary, path)
                break
            except PermissionError:
                if attempt == 39:
                    raise
                time.sleep(0.025)
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


class Conflict(RuntimeError):
    """The app moved on before the edit landed; read state.json and try again."""


def apply_change(project, change, attempts=5):
    """Reads the live state, lets `change` modify it, and submits the result. A
    conflict means someone else edited first, so the whole thing is read and redone
    rather than forced over the top.

    `change` is called with the fresh state each attempt and may return a value to
    pass back to the caller.
    """
    for attempt in range(attempts):
        state, _ = current(project)
        carried = change(state)

        try:
            return submit(project, state), carried
        except Conflict:
            if attempt == attempts - 1:
                raise
            time.sleep(0.2)

    raise Conflict("Gave up after %d attempts" % attempts)


def submit(project, state):
    folder = Path(project).parent
    before, status = current(project)
    if state["revision"] != before["revision"]:
        raise Conflict("Stale revision; read state.json and reapply the edit")
    session = status["session_id"]
    request_id = str(uuid.uuid4())
    state["request_id"] = request_id
    atomic_write(project, state)

    def acknowledged():
        after = read(folder / "sync-status.json")
        if after["session_id"] != session:
            raise RuntimeError("Session changed while editing")
        if after.get("request_id") == request_id and after.get("error"):
            error = after["error"]
            if "Revision conflict" in error or "Session changed" in error:
                raise Conflict(error)
            raise RuntimeError(error)
        if after.get("request_id") == request_id and after["revision"] > state["revision"]:
            return read(folder / "state.json")
        return None

    try:
        return wait_for(acknowledged)
    except TimeoutError as exc:
        error = read(folder / "sync-status.json").get("error")
        if error and ("Revision conflict" in error or "Session changed" in error):
            raise Conflict(error) from exc
        raise RuntimeError(error or str(exc)) from exc


def control(project, action, attempts=5):
    """Transport and history commands carry the revision too, so a command sent while
    the app was mid-change is refused. Read again and resend rather than force it."""
    folder = Path(project).parent

    for attempt in range(attempts):
        state, status = current(project)
        request_id = str(uuid.uuid4())
        atomic_write(folder / "control.json", {"id": request_id, "action": action,
                     "session_id": status["session_id"], "revision": state["revision"]})

        def acknowledged():
            response = read(folder / "control-status.json")
            if response.get("id") != request_id:
                return None
            if response["error"]:
                if "conflict" in response["error"].lower():
                    raise Conflict(response["error"])
                raise RuntimeError(response["error"])
            return response

        try:
            return wait_for(acknowledged)
        except Conflict:
            if attempt == attempts - 1:
                raise
            time.sleep(0.2)

    raise Conflict("Gave up sending %s after %d attempts" % (action, attempts))


def tool(project, name, arguments=None, contract_version=1, request_id=None, timeout=30):
    """Calls one of the app's tools and returns its answer.

    This is the same service the chat panel inside the app talks to, reached through a
    request file the way control() reaches the transport. Nothing is interpreted here:
    whatever the app answers is what comes back, errors included, so a script and the
    chat cannot end up with different ideas about what happened.
    """
    folder = Path(project).parent
    request = {"contract_version": contract_version,
               "request_id": request_id or str(uuid.uuid4()),
               "tool": name,
               "arguments": arguments or {}}

    response = folder / "tool-response.json"
    atomic_write(folder / "tool-request.json", request)

    # The answer is the one carrying this request id. Asking the same id twice is
    # answered from what the app already decided, so the second call returns at once
    # with the same answer rather than doing the work again - which is the behaviour
    # the contract asks for, not a shortcut taken here.
    def answered():
        current = read(response)
        return current if current.get("request_id") == request["request_id"] else None

    return wait_for(answered, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path,
                        default=Path.home() / "Documents/CoCompose/project.json")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("inspect")
    call = commands.add_parser("tool", help="call one of the app's tools directly")
    call.add_argument("name")
    call.add_argument("--arg", action="append", default=[], metavar="KEY=VALUE",
                      help="tool argument; repeat for more. Numbers and JSON are parsed.")
    call.add_argument("--contract-version", type=int, default=1)
    call.add_argument("--request-id", default=None,
                      help="reuse one to check that a repeated request is answered once")
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

    # A tool call goes straight to the app's own service and is answered by it, so
    # nothing here reads or rewrites the project first.
    if args.command == "tool":
        arguments = {}
        for pair in args.arg:
            key, _, raw = pair.partition("=")
            try:
                arguments[key] = json.loads(raw)
            except json.JSONDecodeError:
                arguments[key] = raw

        answer = tool(args.project, args.name, arguments,
                      contract_version=args.contract_version, request_id=args.request_id)
        print(json.dumps(answer, ensure_ascii=False, indent=2))
        raise SystemExit(0 if answer.get("status") == "ok" else 1)

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
