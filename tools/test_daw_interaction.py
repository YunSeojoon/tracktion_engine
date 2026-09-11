"""D0 from the interaction spec: the ruler, and what a click on it means.

The ruler used to do one thing - set a zero-length loop at wherever you clicked - which
meant there was no way to say "play from here" with the mouse, and no way to mark a
stretch of time that was not the loop. Three intentions had one gesture, and it was the
least useful of the three.

These drive the real handlers: MouseEvents built with the right modifiers, passed to
mouseDown, mouseDrag and mouseUp exactly as a window would. What they cannot do is prove
Windows delivers the click - that part stays a person's job and is recorded as such.

    python tools/test_daw_interaction.py
"""
import argparse
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import read, tool, wait_for
from test_plugin_compatibility import Session, prepare_song

ROOT = Path(__file__).resolve().parents[1]


class Report:
    def __init__(self):
        self.failures = []
        self.unchecked = []

    def expect(self, name, condition, detail=''):
        print(('  ok   ' if condition else '  FAIL ') + name + (('  ' + str(detail)) if detail else ''))
        if not condition:
            self.failures.append(name)

    def cannot_check(self, name, why):
        """Something this environment will not let a script produce.

        Recorded apart from both passes and failures, because it is neither: writing it
        off as a pass would be a false report, and as a failure would be blaming the
        code for the room it is running in."""
        print('  --   ' + name + '  (not checked here: ' + why + ')')
        self.unchecked.append(name + " - " + why)


def transport(folder):
    return read(folder / "sync-status.json")


def position_beats(folder):
    return transport(folder).get("position_beats")


def loop_beats(folder):
    status = transport(folder)
    return (status.get("loop_start_beat"), status.get("loop_end_beat"))


def check_the_ruler_moves_the_playhead(exe, folder, report):
    """D0: a left click on the ruler seeks there, and does not start or stop anything."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        session.settled()
        before_revision = read(folder / "sync-status.json")["revision"]

        session.run([{"ruler": ["click", 8.0]}])
        time.sleep(0.5)
        report.expect("clicking the ruler moves the playhead there",
                      abs((position_beats(folder) or -1) - 8.0) < 0.25, position_beats(folder))
        report.expect("and does not start playing", transport(folder)["playing"] is False)
        report.expect("and is not an edit",
                      read(folder / "sync-status.json")["revision"] == before_revision,
                      read(folder / "sync-status.json")["revision"])

        # A second click somewhere else, to be sure it is following the click rather
        # than having landed near eight by luck.
        session.run([{"ruler": ["click", 20.0]}])
        time.sleep(0.5)
        report.expect("and again, somewhere else",
                      abs((position_beats(folder) or -1) - 20.0) < 0.25, position_beats(folder))

        # Dragging shows where it would land and commits once, at the end.
        session.run([{"ruler": ["drag", 4.0, 12.0]}])
        time.sleep(0.5)
        report.expect("dragging ends where it was released",
                      abs((position_beats(folder) or -1) - 12.0) < 0.25, position_beats(folder))
        report.expect("and dragging is not an edit either",
                      read(folder / "sync-status.json")["revision"] == before_revision)
    finally:
        session.close()


def check_a_time_range_is_not_the_loop(exe, folder, report):
    """D0: shift-dragging the ruler marks a stretch of time. It is not the loop, and it
    is what the assistant means by "this part" when nothing else is picked out."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        session.settled()
        before_revision = read(folder / "sync-status.json")["revision"]

        loop_before = loop_beats(folder)

        session.run([{"ruler": ["shift-drag", 8.0, 24.0]}])
        time.sleep(0.5)

        report.expect("marking a time range does not move the loop",
                      loop_beats(folder) == loop_before,
                      (loop_before, loop_beats(folder)))
        report.expect("and does not change the music",
                      read(folder / "sync-status.json")["revision"] == before_revision)

        # With nothing else picked out, that stretch is what a question is about.
        session.run([{"attach": "region"}])
        time.sleep(0.5)
        card = read(folder / "chat-inspector.json")["attachments"][-1]
        report.expect("the marked stretch is what gets attached",
                      abs(card["start_beat"] - 8.0) < 0.25 and abs(card["end_beat"] - 24.0) < 0.25,
                      (card["start_beat"], card["end_beat"]))
    finally:
        session.close()


def check_typing_does_not_play_the_song(exe, folder, report):
    """D0: Space in a text box is a space.

    Someone writing a question to the assistant is not asking for playback, and a
    transport key that fires anyway makes the app feel like it is fighting them."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        session.settled()

        session.run([{"chat": "draft: does space start the song"}, {"chat": "focus"}])
        time.sleep(0.4)
        report.expect("there is something typed to protect",
                      "space" in (read(folder / "chat-inspector.json").get("draft") or "").lower(),
                      read(folder / "chat-inspector.json").get("draft"))
        # The guard reads the same focus JUCE uses to route key presses, so it is right
        # in use - but a window that the OS will not make active cannot hold the
        # keyboard, and under automation here it never does. So the state the guard
        # exists for cannot be produced from a script on this machine.
        if read(folder / "chat-inspector.json").get("typing") is not True:
            report.cannot_check(
                "space while typing does not start playback",
                "this machine will not give the app window the keyboard from a script;"
                " type in the chat box and press space to see it")
            return

        session.run([{"command": "Play / Stop"}])
        time.sleep(0.6)
        report.expect("space while typing does not start playback",
                      transport(folder)["playing"] is False, transport(folder)["playing"])
        report.expect("and the words are still there",
                      "space" in (read(folder / "chat-inspector.json").get("draft") or "").lower())
    finally:
        session.close()


def check_stop_twice_goes_back(exe, folder, report):
    """D0: stop when already stopped means "take me back to where I started"."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        session.settled()

        session.run([{"ruler": ["click", 16.0]}])
        time.sleep(0.4)
        session.run([{"command": "Play / Stop"}])
        time.sleep(1.2)
        report.expect("it is playing", transport(folder)["playing"] is True)

        session.run([{"command": "Stop"}])
        time.sleep(0.6)
        report.expect("stopping stops", transport(folder)["playing"] is False)
        moved_to = position_beats(folder)

        session.run([{"command": "Stop"}])
        time.sleep(0.6)
        report.expect("stopping again goes back to where play began",
                      abs((position_beats(folder) or -1) - 16.0) < 0.5,
                      (moved_to, position_beats(folder)))
        report.expect("and did not start playing to get there",
                      transport(folder)["playing"] is False)
    finally:
        session.close()


def check_right_click_asks_instead_of_deleting(exe, folder, report):
    """D1: right-clicking a clip used to delete it on the spot.

    That is the one gesture a person cannot take back before it happens - a mis-aimed
    click and the clip is gone - and it is also the gesture every other DAW uses to ask
    "what can I do with this". Now it asks."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()
        clips_before = len(state["playlist"]["clips"])
        report.expect("there is a clip to right-click", clips_before > 0, clips_before)

        revision = read(folder / "sync-status.json")["revision"]
        start = state["playlist"]["clips"][0]["start"]

        session.run([{"ruler": ["right-click", start + 0.5, start + 0.5, 0]}])
        time.sleep(0.6)
        session.run([{"dismiss_menus": True}])
        time.sleep(0.3)

        after = session.settled()
        report.expect("right-clicking a clip does not delete it",
                      len(after["playlist"]["clips"]) == clips_before,
                      (clips_before, len(after["playlist"]["clips"])))
        report.expect("and opening a menu is not an edit",
                      read(folder / "sync-status.json")["revision"] == revision,
                      read(folder / "sync-status.json")["revision"])

        # Right-clicking empty space is also not a place-a-clip-here gesture.
        session.run([{"ruler": ["right-click", start + 200.0, start + 200.0, 0]}])
        time.sleep(0.6)
        session.run([{"dismiss_menus": True}])
        time.sleep(0.3)
        report.expect("right-clicking empty space places nothing",
                      len(session.settled()["playlist"]["clips"]) == clips_before)

        # Deleting still works, through the key that means delete.
        # Delete is a key on the grid, not a menu command - the grid is what has a
        # selection to delete.
        session.run([{"pick_clip": [0, start + 0.5]}, {"grid_key": "delete"}])
        time.sleep(0.6)
        remaining = len(session.settled()["playlist"]["clips"])
        report.expect("Delete still deletes", remaining == clips_before - 1,
                      (clips_before, remaining))
    finally:
        session.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("the ruler moves the playhead")
    check_the_ruler_moves_the_playhead(exe, output / "seek", report)
    print()
    print("a time range is its own thing")
    check_a_time_range_is_not_the_loop(exe, output / "range", report)
    print()
    print("typing is not transport")
    check_typing_does_not_play_the_song(exe, output / "typing", report)
    print()
    print("stop, then stop again")
    check_stop_twice_goes_back(exe, output / "stop", report)
    print()
    print("right-click asks rather than destroys")
    check_right_click_asks_instead_of_deleting(exe, output / "menus", report)

    print()
    if report.unchecked:
        print("NOT CHECKED HERE:")
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
                        default=ROOT / "build-cocompose" / ("daw-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
