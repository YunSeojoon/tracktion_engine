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

from cocompose import apply_change, atomic_write, control, read, tool, wait_for
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

            # Two halves to hear, and a way to hear them. A comparison that leaves two
            # files and no button finishes in Explorer.
            hearable = wait_for(lambda: (panel(folder).get("listening")
                                         if (panel(folder).get("listening") or {}).get("offered")
                                         else None),
                                timeout=30, what="the panel to offer both halves")
            report.expect("both halves can be played from the panel",
                          hearable["a"] == "Play A" and hearable["b"] == "Play B", hearable)

            # Playing must not happen on top of the music the proposal is about.
            session.run([{"command": "Play / Stop"}])
            time.sleep(0.8)
            session.run([{"press": "play_a"}])
            time.sleep(0.8)

            now = panel(folder).get("listening") or {}
            report.expect("the button says which half is playing",
                          now.get("a") == "Playing A" and now.get("b") == "Play B", now)
            report.expect("and the song was stopped rather than played over",
                          read(folder / "sync-status.json")["playing"] is False,
                          read(folder / "sync-status.json")["playing"])

            session.run([{"press": "play_a"}])
            time.sleep(0.5)
            stopped = panel(folder).get("listening") or {}
            report.expect("pressing it again stops it",
                          stopped.get("a") == "Play A", stopped)

            shelf = wait_for(lambda: (panel(folder).get("candidates_on_screen")
                                      if len(panel(folder).get("candidates_on_screen") or []) >= 2
                                      else None),
                             timeout=20, what="both suggestions to reach the list")
            report.expect("earlier suggestions are on screen, numbered", len(shelf) == 2, shelf)

            # And one can be thrown away. It removes a record of an offer and no music,
            # which is the whole reason it is safe to put next to the list.
            before = len(read(folder / "state.json")["patterns"][0].get("sequences", []))
            session.run([{"press": "forget"}])
            fewer = wait_for(lambda: (panel(folder).get("candidates_on_screen")
                                      if len(panel(folder).get("candidates_on_screen") or []) == 1
                                      else None),
                             timeout=20, what="the suggestion to leave the list")
            report.expect("one can be forgotten", len(fewer) == 1, fewer)
            report.expect("and forgetting it deleted no music",
                          len(read(folder / "state.json")["patterns"][0].get("sequences", []))
                          == before)
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


def check_another_project_can_be_opened_without_leaving_the_app(exe, folder, report):
    """W0 step 1: choosing a project.

    Without --project the app opens one fixed path in Documents and there was no way to
    work on anything else: no Open in the File menu, only Save a copy. That stops the
    milestone's scenario at its first word, and everything after it is untested ground
    for somebody who cannot get there.

    The dialog is a person's to click. What this drives is everything after it, which is
    the part that can go wrong in an interesting way: the old work surface comes down
    with its engine and its hold on the old folder, and a new one goes up - and nothing
    of the old project comes with it, because the conversation, the shelf and the notes
    belong to the project they were made in."""
    first = folder / "first"
    second = folder / "second"
    first.mkdir(parents=True, exist_ok=True)
    second.mkdir(parents=True, exist_ok=True)

    session = Session(exe, first).open()

    try:
        prepare_song(session)
        session.settled()

        # Something in the first project that must not follow us.
        session.run([{"project_note": ["condition", "the first project decided this"]}])
        time.sleep(0.5)
        report.expect("the first project has something in it",
                      len(read(first / "state.json")["patterns"]) >= 1
                      and any("first project" in n["text"]
                              for n in (panel(first).get("notes") or {}).get("notes", [])),
                      (len(read(first / "state.json")["patterns"]),
                       [n["text"] for n in (panel(first).get("notes") or {}).get("notes", [])]))

        # Fired, not awaited. Session.run waits for the round to be marked finished in
        # the folder it was sent to, and the thing that marks it is the editor that is
        # about to be taken down - so waiting here is waiting for a write nobody is left
        # to make. What the switch happened is read from the new folder instead.
        atomic_write(first / "ui-script.json",
                     [{"comment": "switch"}, {"open_project": str(second)}])

        # The new project writes its own files. Waiting for state.json to appear there is
        # the first thing that can be true only if the switch actually happened.
        wait_for(lambda: (second / "state.json").exists(), timeout=60,
                 what="the new project to write its state")
        wait_for(lambda: read(second / "sync-status.json").get("session_id"), timeout=30,
                 what="the new project to say who is holding it")

        report.expect("the app is working on the new folder now",
                      (second / "project.json").exists() and (second / "state.json").exists())

        fresh = read(second / "state.json")
        report.expect("and it is a new project, not the old one copied",
                      not any("first project" in n.get("text", "")
                              for n in (panel(second).get("notes") or {}).get("notes", [])),
                      [n.get("text") for n in (panel(second).get("notes") or {}).get("notes", [])])
        report.expect("its conversation is its own",
                      not read(second / "conversation.json").get("messages")
                      if (second / "conversation.json").exists() else True)

        # Live sync has to work on the new one - that is the thing the app is for.
        before = read(second / "sync-status.json")["revision"]

        def rename(live):
            live["channels"][0]["name"] = "Renamed in the second project"

        apply_change(second / "project.json", rename)
        time.sleep(1.0)
        report.expect("live sync is running on the new project",
                      read(second / "state.json")["channels"][0]["name"]
                      == "Renamed in the second project",
                      read(second / "state.json")["channels"][0]["name"])
        report.expect("and its revision moved",
                      read(second / "sync-status.json")["revision"] != before)

        # The old folder is nobody's now: the app must not still be writing to it.
        was = read(first / "sync-status.json")["revision"]
        time.sleep(1.5)
        report.expect("the old project was let go, not held open",
                      read(first / "sync-status.json")["revision"] == was,
                      (was, read(first / "sync-status.json")["revision"]))

        report.for_a_person("that the folder chooser is easy to find and says what it wants",
                            "a file dialog is modal; what it looks like and how it reads "
                            "is seen, not measured")
    finally:
        # The session object still points at the first folder, and the app is on the
        # second. Quitting has to be asked of the project the app is actually holding.
        try:
            control(second / "project.json", "quit")
            if session.process is not None:
                session.process.wait(timeout=20)
            session.process = None
        except Exception:
            session.close()


def check_what_is_known_about_the_project_is_on_screen(exe, folder, report):
    """W0: the three kinds of note, where the person can see them.

    A condition is a rule they set. A guess is the assistant's reading, and is marked as
    one, because a guess that reads like a rule is how an assistant ends up defending a
    key nobody chose. A todo is work not done. All three went to the model and none was
    on screen, so the one participant who could not see what had been agreed was the one
    who had agreed to it."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        clip = state["playlist"]["clips"][0]

        empty = wait_for(lambda: (panel(folder).get("decisions_on_screen")
                                  if panel(folder).get("decisions_on_screen") else None),
                         timeout=20, what="the panel to say what is known")
        report.expect("with nothing decided, it says so rather than showing a blank",
                      "nothing decided" in empty.lower(), empty[:60])

        session.run([{"project_note": ["condition", "keep the drums as they are"]},
                     {"project_note": ["guess", "this sounds like D minor"]},
                     {"project_note": ["todo_on", "rewrite this fill", clip["id"]]}])

        shown = wait_for(lambda: (panel(folder).get("decisions_on_screen")
                                  if "drums" in (panel(folder).get("decisions_on_screen") or "")
                                  else None),
                         timeout=20, what="the notes to reach the screen")

        report.expect("what the person decided is marked as decided",
                      "DECIDED  keep the drums as they are" in shown, shown)
        report.expect("what the assistant guessed is marked as a guess",
                      "guessed  this sounds like D minor" in shown, shown)
        report.expect("and says which music the guess was read from",
                      "read at revision" in shown, shown)
        report.expect("work not done is shown as work not done",
                      "to do    rewrite this fill" in shown, shown)
        report.expect("and a note tied to a clip says it follows one",
                      "follows a clip" in shown, shown)

        # The clip goes. The note has to say it lost its place, on screen, not only in
        # the file the model reads.
        def remove(live):
            live["playlist"]["clips"] = [p for p in live["playlist"]["clips"]
                                         if p["id"] != clip["id"]]

        apply_change(folder / "project.json", remove)
        orphaned = wait_for(lambda: (panel(folder).get("decisions_on_screen")
                                     if "has gone" in (panel(folder).get("decisions_on_screen") or "")
                                     else None),
                            timeout=30, what="the panel to notice the clip went")
        report.expect("a note whose clip was deleted says so where a person can see it",
                      "the clip this was about has gone" in orphaned, orphaned)

        # How far a suggestion may go, chosen on screen.
        report.expect("the strength starts where the project left it",
                      panel(folder).get("strength_on_screen") == "Nudge it",
                      panel(folder).get("strength_on_screen"))

        session.run([{"project_note": ["strength", "fresh"]}])
        moved = wait_for(lambda: (panel(folder).get("strength_on_screen")
                                  if panel(folder).get("strength_on_screen") != "Nudge it"
                                  else None),
                         timeout=20, what="the strength to change on screen")
        report.expect("and follows the project when it changes",
                      moved == "Something new", moved)

        report.for_a_person("whether the three kinds read as three different things",
                            "what a label communicates is read, not measured")
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
    print("another project can be opened without leaving the app")
    check_another_project_can_be_opened_without_leaving_the_app(exe, output / "open", report)
    print()
    print("what is known about the project is on screen")
    check_what_is_known_about_the_project_is_on_screen(exe, output / "notes", report)
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
