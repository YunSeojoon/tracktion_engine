"""W3: the things the app can do, reachable from the panel a person is looking at.

Preview, the shelf of earlier suggestions and the notes were all built and all reached
only by the diagnostic script. So "press Preview" on the acceptance sheet was not a
step somebody had skipped - it was a step with no button, and a blank next to it meant
something different from every other blank on the page.

What a script can check here is not whether the pointer lands on the button; it is
whether there is a button to land on, whether it is offered at the right moment, and
whether pressing it does what its label says. Where it appears on screen and how it
reads stays a person's job and is recorded as one.

    python tools/test_panel_reach.py
"""
import argparse
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import atomic_write, read, tool, wait_for
from test_plugin_compatibility import Session, prepare_song
from cocompose_bridge import Liveness

ROOT = Path(__file__).resolve().parents[1]


class Report:
    def __init__(self):
        self.failures = []
        self.unchecked = []

    def expect(self, name, condition, detail=''):
        print(('  ok   ' if condition else '  FAIL ') + name + (('  ' + str(detail)) if detail else ''))
        if not condition:
            self.failures.append(name)

    def for_a_person(self, name, why):
        print('  --   ' + name + '  (' + why + ')')
        self.unchecked.append(name + " - " + why)


def panel(folder):
    return read(folder / "chat-inspector.json")


def buttons(folder):
    return panel(folder).get("buttons", {})


def ask_and_answer(session, folder, change, previous):
    session.run([{"attach": "notes"}])
    session.run([{"chat": "ask: something"}])

    request = wait_for(lambda: (read(folder / "chat-request.json")
                                if read(folder / "chat-request.json").get("request_id") != previous
                                else None),
                       timeout=20, what="a question other than " + str(previous))

    atomic_write(folder / "chat-reply.json",
                 {"request_id": request["request_id"], "status": "ok",
                  "text": "a synthetic reply", "provider": "fixture", "model": "fixture",
                  "change": change})

    wait_for(lambda: next((m for m in read(folder / "conversation.json")["messages"]
                           if m.get("request_id") == request["request_id"]
                           and m.get("from") == "assistant"
                           and (m.get("proposal_id") or m.get("proposal_problem"))), None),
             timeout=20, what="the app to consider the reply")

    return request["request_id"]


def check_a_suggestion_can_be_heard_without_a_script(exe, folder, report):
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        pattern_id = prepare_song(session)
        state = session.settled()
        channel_id = state["channels"][0]["id"]
        notes = tool(project, "inspect_pattern",
                     {"pattern": pattern_id, "channel": channel_id})["result"]["parts"][0]["notes"]
        subject = notes[0]

        report.expect("with nothing offered there is nothing to apply or hear",
                      buttons(folder).get("apply") is False
                      and buttons(folder).get("preview") is False,
                      buttons(folder))

        with Liveness(folder / "chat-bridge.json", "fixture bridge",
                      provider="fixture", model="fixture"):
            wait_for(lambda: panel(folder).get("bridge_connected"), timeout=20,
                     what="the app to see the fixture bridge")

            asked = ask_and_answer(session, folder,
                                   {"description": "AI: up a tone",
                                    "notes": [{"what": "change", "id": subject["id"],
                                               "pitch": min(127, subject["pitch"] + 2)}]}, None)

            offered = wait_for(lambda: (buttons(folder)
                                        if buttons(folder).get("preview") else None),
                               timeout=20, what="the panel to offer a comparison")
            report.expect("a suggestion offers both taking it and hearing it first",
                          offered.get("apply") and offered.get("preview"), offered)

            # Press it. No range is asked for: the proposal knows what it touches.
            revision = read(folder / "sync-status.json")["revision"]
            (folder / "preview-status.json").unlink(missing_ok=True)
            session.run([{"press": "preview"}])

            state_line = wait_for(lambda: (panel(folder).get("preview_state")
                                           if panel(folder).get("preview_state") else None),
                                  timeout=30, what="the panel to say what it is doing")
            report.expect("the panel says what is happening, in words",
                          "render" in state_line.lower(), state_line)

            status = wait_for(lambda: (read(folder / "preview-status.json")
                                       if (folder / "preview-status.json").exists()
                                       and read(folder / "preview-status.json").get("running") is False
                                       else None),
                              timeout=240, what="the comparison to finish")
            report.expect("pressing it rendered both halves",
                          status["before"]["exists"] and status["after"]["exists"],
                          (status["before"]["exists"], status["after"]["exists"]))
            report.expect("over a stretch the app chose from what the proposal touches",
                          status["end_beat"] > status["start_beat"],
                          (status["start_beat"], status["end_beat"]))
            report.expect("and hearing it changed no music",
                          read(folder / "sync-status.json")["revision"] == revision)

            done = wait_for(lambda: (panel(folder).get("preview_state")
                                     if "your part" in (panel(folder).get("preview_state") or "")
                                     else None),
                            timeout=30, what="the panel to say it is done")
            report.expect("and says plainly that listening is the person's part",
                          "your part" in done, done[:80])

            # A second suggestion, so there is a shelf to go back to.
            ask_and_answer(session, folder,
                           {"description": "AI: up a fifth",
                            "notes": [{"what": "change", "id": subject["id"],
                                       "pitch": min(127, subject["pitch"] + 7)}]}, asked)

            shelf = wait_for(lambda: (panel(folder).get("candidates_on_screen")
                                      if len(panel(folder).get("candidates_on_screen") or []) >= 2
                                      else None),
                             timeout=20, what="both suggestions to reach the list")
            report.expect("earlier suggestions are on screen, numbered", len(shelf) == 2, shelf)
            report.expect("and each says what it was",
                          all(("up a tone" in s or "up a fifth" in s) for s in shelf), shelf)

        report.for_a_person("that the buttons are where a hand expects them and read well",
                            "where a control sits and how it reads is seen, not measured")
    finally:
        session.close()


def check_a_stale_suggestion_says_so_on_screen(exe, folder, report):
    """A candidate worked out against music that has moved cannot simply be applied, and
    the list says which music it read rather than letting somebody pick one and meet a
    refusal they were given no warning about."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        pattern_id = prepare_song(session)
        state = session.settled()
        channel_id = state["channels"][0]["id"]
        notes = tool(project, "inspect_pattern",
                     {"pattern": pattern_id, "channel": channel_id})["result"]["parts"][0]["notes"]

        with Liveness(folder / "chat-bridge.json", "fixture bridge",
                      provider="fixture", model="fixture"):
            wait_for(lambda: panel(folder).get("bridge_connected"), timeout=20,
                     what="the app to see the fixture bridge")
            ask_and_answer(session, folder,
                           {"description": "AI: up a tone",
                            "notes": [{"what": "change", "id": notes[0]["id"],
                                       "pitch": min(127, notes[0]["pitch"] + 2)}]}, None)

        fresh = wait_for(lambda: (panel(folder).get("candidates_on_screen")
                                  if panel(folder).get("candidates_on_screen") else None),
                         timeout=20, what="the suggestion to reach the list")
        report.expect("while the music has not moved, nothing is marked stale",
                      not any("revision" in s for s in fresh), fresh)

        # The person edits. The suggestion is now about music that is no longer there.
        session.run([{"command": "Transpose pattern up"}])
        time.sleep(1.0)
        session.settled()

        stale = wait_for(lambda: (panel(folder).get("candidates_on_screen")
                                  if any("revision" in s
                                         for s in (panel(folder).get("candidates_on_screen") or []))
                                  else None),
                         timeout=20, what="the list to notice the music moved")
        report.expect("once the music moves, the list says which music it read",
                      any("revision" in s for s in stale), stale)
    finally:
        session.close()


def check_the_panel_says_what_it_is_connected_to(exe, folder, report):
    """W3: the connection, the model and what to do about it, on screen.

    The line used to read "connected: <name>" or "no bridge running". Neither said which
    model was answering, what it could do, or - when nothing answered - what would make
    it answer. A status line that states a problem and not its remedy leaves somebody
    looking at the app with nowhere to go."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        session.settled()

        nothing = wait_for(lambda: (panel(folder).get("connection")
                                    if panel(folder).get("connection") else None),
                           timeout=20, what="the panel to say what it is connected to")
        report.expect("with nothing listening, the line says so",
                      "no ai connected" in nothing.lower(), nothing)
        report.expect("and says what would make it answer",
                      "connect ai" in nothing.lower() or "bridge" in nothing.lower(), nothing)

        with Liveness(folder / "chat-bridge.json", "fixture bridge",
                      provider="ollama", model="llama3.1:8b",
                      capabilities={"suggests_changes": True, "hears_audio": False,
                                    "runs_locally": True, "is_a_model": True}):
            connected = wait_for(lambda: (panel(folder).get("connection")
                                          if "connected:" in (panel(folder).get("connection") or "").lower()
                                          else None),
                                 timeout=20, what="the panel to notice the bridge")

            report.expect("it names the model, not just the bridge",
                          "llama3.1:8b" in connected, connected)
            report.expect("and which provider it came through",
                          "ollama" in connected, connected)
            report.expect("and that it cannot hear audio, which it cannot",
                          "cannot hear audio" in connected, connected)
            report.expect("and that it is on this machine",
                          "on this machine" in connected, connected)

        # It goes away again.
        gone = wait_for(lambda: (panel(folder).get("connection")
                                 if "connected:" not in (panel(folder).get("connection") or "").lower()
                                 else None),
                        timeout=30, what="the panel to notice the bridge left")
        # Not "it stopped" and not "it is starting": the file says ready:false in both
        # cases and the app cannot tell them apart, so it says the part that is true
        # either way rather than guessing and being confidently wrong half the time.
        report.expect("when nothing is answering, the line says so and what to do",
                      "not answering" in gone.lower() or "no ai is answering" in gone.lower(),
                      gone)
        report.expect("and does not claim to know whether it is starting or stopped",
                      "starting up" not in gone.lower(), gone)
    finally:
        session.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("a suggestion can be heard without a script")
    check_a_suggestion_can_be_heard_without_a_script(exe, output / "hear", report)
    print()
    print("the panel says what it is connected to")
    check_the_panel_says_what_it_is_connected_to(exe, output / "connection", report)
    print()
    print("a stale suggestion says so on screen")
    check_a_stale_suggestion_says_so_on_screen(exe, output / "stale", report)

    print()
    if report.unchecked:
        print("FOR A PERSON:")
        for item in report.unchecked:
            print("  - " + item)
    print("FAILURES:", report.failures if report.failures else "none")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path,
                        default=ROOT / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build-cocompose" / ("panel-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
