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

from cocompose import apply_change, read, tool, wait_for
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
        # in use - but a window the OS will not make active cannot hold the keyboard,
        # and headless it never does. So the state the guard exists for cannot be
        # produced from a script on this machine.
        #
        # Nor can the script stand in for the key any more, and that is on purpose. The
        # guard now asks whether the command came from a key press, because a menu item
        # clicked while a name is selected in the arrangement is not somebody typing and
        # used to be declined as though it were. A command sent through the UI script is
        # not a key press either, so running it here would prove nothing about the key.
        if read(folder / "chat-inspector.json").get("typing") is not True:
            report.cannot_check(
                "Space, Return and Home while typing stay out of the music",
                "this machine will not give the app window the keyboard from a script;"
                " type in the chat box and press each of them to see it")
            return

        report.cannot_check(
            "Space, Return and Home while typing stay out of the music",
            "the app has the keyboard, but a UI-script command is not a key press and"
            " the guard is deliberately only about key presses")
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


def check_double_click_opens_the_pattern(exe, folder, report):
    """D2: double-clicking a clip opens what is in it.

    It used to split the clip. Splitting is something a person does on purpose, and
    there is a tool and a command for it; meanwhile there was no mouse gesture at all
    for "let me see the notes in this", which is the thing people try first."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()
        clips_before = len(state["playlist"]["clips"])
        start = state["playlist"]["clips"][0]["start"]
        revision = read(folder / "sync-status.json")["revision"]

        session.run([{"ruler": ["double-click", start + 1.0, start + 1.0, 0]}])
        time.sleep(1.0)

        report.expect("double-clicking a clip does not split it",
                      len(session.settled()["playlist"]["clips"]) == clips_before,
                      (clips_before, len(session.settled()["playlist"]["clips"])))
        report.expect("and entering the editor is not an edit",
                      read(folder / "sync-status.json")["revision"] == revision,
                      read(folder / "sync-status.json")["revision"])
        report.expect("the note editor is open",
                      read(folder / "chat-inspector.json").get("editor_open") is True,
                      read(folder / "chat-inspector.json").get("editor_open"))
        report.expect("it says which pattern and channel it is showing",
                      bool(read(folder / "chat-inspector.json").get("editor_heading")),
                      read(folder / "chat-inspector.json").get("editor_heading"))
    finally:
        session.close()


def check_the_editor_says_how_far_an_edit_reaches(exe, folder, report):
    """D2: a pattern placed twice is edited in both places at once.

    Someone who opened the editor by double-clicking one clip will think they are
    editing that clip. They are not, and the window has to say so before they touch
    anything rather than after."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()
        start = state["playlist"]["clips"][0]["start"]

        session.run([{"ruler": ["double-click", start + 1.0, start + 1.0, 0]}])
        time.sleep(1.0)
        heading = read(folder / "chat-inspector.json").get("editor_heading") or ""
        report.expect("one placement is described as one",
                      "one place" in heading, heading)

        # Place the same pattern again, somewhere else.
        session.run([{"select_lane": 0}, {"place": [0, 64.0]}])
        time.sleep(1.0)
        heading = read(folder / "chat-inspector.json").get("editor_heading") or ""
        report.expect("a second placement is announced before anything is edited",
                      "2 places" in heading and "all of them" in heading, heading)
    finally:
        session.close()


def check_select_does_not_draw(exe, folder, report):
    """D2: clicking empty grid used to write a note and start a selection box at the
    same time, so every attempt to drag out a selection left a note behind."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        session.settled()

        # Open the editor and find out what is in it.
        session.run([{"select_channel": 0}, {"note": [72, 0.0, 1.0, 100]}])
        time.sleep(0.8)
        here = tool(project, "get_selection")["result"]

        def note_count():
            part = tool(project, "inspect_pattern",
                        {"pattern": here["pattern"], "channel": here["channel"]})["result"]
            return sum(len(p["notes"]) for p in part["parts"])

        before = note_count()

        session.run([{"piano_tool": "select"}, {"piano_click": [90, 4.0]}])
        time.sleep(0.8)
        report.expect("clicking empty grid with Select writes no note",
                      note_count() == before, (before, note_count()))

        session.run([{"piano_tool": "draw"}, {"piano_click": [90, 4.0]}])
        time.sleep(0.8)
        report.expect("clicking empty grid with Draw writes one",
                      note_count() == before + 1, (before, note_count()))
    finally:
        session.close()


def check_select_does_not_place_clips(exe, folder, report):
    """D3: the arrangement behaved as though Draw were always on.

    Clicking empty space placed a clip, so dragging out a selection left a clip behind
    and the only way to select a region was to hold a modifier. Select is the default
    now, as it is in every arrangement view, and placing is something you choose."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()
        before = len(state["playlist"]["clips"])

        report.expect("Select is what the arrangement starts with",
                      read(folder / "chat-inspector.json").get("grid_tool") == "select",
                      read(folder / "chat-inspector.json").get("grid_tool"))

        session.run([{"ruler": ["click", 200.0, 200.0, 0]}])
        time.sleep(0.6)
        report.expect("clicking empty arrangement with Select places nothing",
                      len(session.settled()["playlist"]["clips"]) == before,
                      (before, len(session.settled()["playlist"]["clips"])))

        session.run([{"grid_tool": "draw"}, {"ruler": ["click", 200.0, 200.0, 0]}])
        time.sleep(0.6)
        report.expect("and with Draw places one",
                      len(session.settled()["playlist"]["clips"]) == before + 1,
                      (before, len(session.settled()["playlist"]["clips"])))

        session.run([{"grid_tool": "select"}])
        time.sleep(0.3)
        report.expect("the tool can be put back",
                      read(folder / "chat-inspector.json").get("grid_tool") == "select")
    finally:
        session.close()


def check_alt_puts_the_grid_away(exe, folder, report):
    """D3: Alt suspends snapping while it is held.

    Alt used to mean rubber-band select here, which the Select tool now does properly,
    so the modifier was free for what it means in most arrangements: put the grid away
    for a moment, I know where this goes."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()
        clip = state["playlist"]["clips"][0]
        start = clip["start"]

        # A whole number of beats, snapped, from a drag that asks for a fraction.
        session.run([{"pick_clip": [0, start + 0.5]},
                     {"ruler": ["drag", start + 0.5, start + 4.3, 0]}])
        time.sleep(0.8)
        snapped_to = session.settled()["playlist"]["clips"][0]["start"]
        report.expect("an ordinary drag lands on the grid",
                      abs(snapped_to - round(snapped_to)) < 1e-6, snapped_to)

        session.run([{"pick_clip": [0, snapped_to + 0.5]},
                     {"ruler": ["alt-drag", snapped_to + 0.5, snapped_to + 4.3, 0]}])
        time.sleep(0.8)
        free = session.settled()["playlist"]["clips"][0]["start"]
        report.expect("holding Alt lands between the lines",
                      abs(free - round(free)) > 1e-6, free)
    finally:
        session.close()


def check_erase_and_split_are_chosen_not_stumbled_into(exe, folder, report):
    """D3: the destructive gestures live behind tools you have to pick.

    Right-click no longer deletes, and the plain left button never did. Erase and Split
    act on the clip they are pointed at the moment they are clicked - which is safe
    precisely because choosing them is a deliberate act, and the cursor changes so a
    person can see which one they are holding."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()
        clips = len(state["playlist"]["clips"])
        start = state["playlist"]["clips"][0]["start"]
        report.expect("there is a clip to work on", clips > 0, clips)

        session.run([{"grid_tool": "split"}, {"ruler": ["click", start + 2.0, start + 2.0, 0]}])
        time.sleep(0.8)
        after_split = len(session.settled()["playlist"]["clips"])
        report.expect("the Split tool cuts what it is pointed at",
                      after_split == clips + 1, (clips, after_split))

        session.run([{"grid_tool": "erase"}, {"ruler": ["click", start + 2.5, start + 2.5, 0]}])
        time.sleep(0.8)
        after_erase = len(session.settled()["playlist"]["clips"])
        report.expect("the Erase tool removes what it is pointed at",
                      after_erase == after_split - 1, (after_split, after_erase))

        session.run([{"grid_tool": "select"}, {"ruler": ["click", start + 0.5, start + 0.5, 0]}])
        time.sleep(0.8)
        report.expect("and Select goes back to touching nothing",
                      len(session.settled()["playlist"]["clips"]) == after_erase,
                      len(session.settled()["playlist"]["clips"]))
        report.expect("the app agrees about which tool is held",
                      read(folder / "chat-inspector.json").get("grid_tool") == "select",
                      read(folder / "chat-inspector.json").get("grid_tool"))
    finally:
        session.close()


def check_no_two_commands_answer_to_one_key(exe, folder, report):
    """D4: the shortcut table, kept by the code rather than by a document.

    Two commands on one key is not a matter of taste - one of them silently never runs,
    and which one depends on registration order, so it will be the wrong one about half
    the time. The app writes out its own commands and their keys, so the table cannot
    drift from what is actually bound."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        session.settled()
        table = read(folder / "shortcuts.json")
        commands = table.get("commands", [])
        report.expect("the app writes down its own shortcuts", len(commands) > 10, len(commands))

        taken = {}
        clashes = []
        for command in commands:
            for key in command.get("keys", []):
                if key in taken:
                    clashes.append("%s and %s both answer to %s"
                                   % (taken[key], command["name"], key))
                taken[key] = command["name"]

        report.expect("no two commands answer to the same key", not clashes, clashes)

        # A shortcut nobody can discover is nearly as bad as none: the menu bar is where
        # the key is printed next to the name, so anything bound should be in a menu.
        report.expect("the commands that have keys are named",
                      all(command["name"] for command in commands if command.get("keys")))

        print("     shortcuts in use: " + ", ".join(sorted(taken)[:14]) + " ...")
    finally:
        session.close()


def check_lanes_can_be_made_taller_and_shorter(exe, folder, report):
    """D3: lane height. An arrangement with three lanes and one with thirty want
    different answers, and neither should have to be scrolled through at the other's
    size. It is how somebody is looking at the music, not part of the music."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        session.settled()
        revision = read(folder / "sync-status.json")["revision"]
        started = read(folder / "chat-inspector.json").get("lane_height")
        report.expect("lanes have a height to begin with", isinstance(started, int) and started > 0,
                      started)

        session.run([{"lane_height": 60}])
        time.sleep(0.5)
        report.expect("lanes can be made taller",
                      read(folder / "chat-inspector.json").get("lane_height") == 60,
                      read(folder / "chat-inspector.json").get("lane_height"))

        # Out of range is clamped rather than obeyed - a lane one pixel tall is not a
        # view of anything, and neither is one taller than the window.
        session.run([{"lane_height": 2}])
        time.sleep(0.5)
        clamped = read(folder / "chat-inspector.json").get("lane_height")
        report.expect("and not made uselessly small", clamped >= 16, clamped)

        report.expect("changing how it looks is not an edit",
                      read(folder / "sync-status.json")["revision"] == revision,
                      read(folder / "sync-status.json")["revision"])
    finally:
        session.close()


def check_an_effect_can_be_opened_from_the_chain(exe, folder, report):
    """D4: double-clicking a name in the mixer strip's effect chain opens that plugin.

    The chain was a label - correct and unreachable - so the only way to open an effect
    was to find it again in a menu. This drives the same call the double-click makes."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()
        insert_id = state["mixer"]["inserts"][0]["id"]

        def add_reverb(live):
            insert = next(i for i in live["mixer"]["inserts"] if i["id"] == insert_id)
            insert.setdefault("effects", []).append(
                {"id": "fx-verb", "type": "reverb", "bypass": False, "wet": 0.5, "parameters": []})

        apply_change(project, add_reverb)
        wait_for(lambda: any(e["id"] == "fx-verb" for e in
                             tool(project, "inspect_insert", {"insert": insert_id})
                                 ["result"]["effects"]), timeout=20)

        revision = read(folder / "sync-status.json")["revision"]
        session.run([{"open_effect": [insert_id, 0]}])
        time.sleep(1.0)

        report.expect("opening an effect from the chain is not an edit",
                      read(folder / "sync-status.json")["revision"] == revision,
                      read(folder / "sync-status.json")["revision"])

        # An index past the end is refused rather than opening whatever is nearby. The
        # script runner reports a refused action as a failed one, so that is the signal.
        refused = False
        try:
            session.run([{"open_effect": [insert_id, 9]}], timeout=20)
        except TimeoutError:
            refused = True

        report.expect("a chain position that does not exist opens nothing", refused)

        session.run([{"close_plugins": True}])
    finally:
        session.close()


def check_every_target_has_a_menu_and_opening_it_changes_nothing(exe, folder, report):
    """B1: the targets that had no menu, and the one whose menu was the wrong menu.

    A channel header, an effect in a mixer chain and an audio clip are all things a
    person points at and right-clicks, and until now the first two answered with
    nothing and the third answered with a pattern clip's menu - "Open in piano roll"
    on a recording. What every one of them has to have in common is that opening the
    menu is not itself an edit: a mis-aimed right-click must cost nothing.

    These drive real right-button MouseEvents into the components. What they cannot
    prove is that Windows delivers the click, which stays a person's job."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()
        insert_id = state["mixer"]["inserts"][0]["id"]

        # An effect to point at. Added through the project file rather than through the
        # menu being tested, so the check does not depend on the thing it is checking.
        def add_a_reverb(live):
            live["mixer"]["inserts"][0].setdefault("effects", []).append(
                {"id": "a-reverb-to-point-at", "type": "reverb", "bypass": False, "wet": 1.0})

        apply_change(project, add_a_reverb)
        time.sleep(0.8)
        state = session.settled()
        effects = state["mixer"]["inserts"][0].get("effects", [])
        report.expect("there is an effect in the chain to point at", len(effects) == 1, effects)

        # An audio clip to point at, on its own lane.
        sample = folder / "a-sound.wav"
        write_a_quiet_wav(sample)
        lanes = len(state["playlist"]["lanes"])
        session.run([{"audio": [lanes - 1, str(sample), 0.0]}])
        time.sleep(0.8)
        state = session.settled()
        # Audio placements are their own list beside the pattern clips, which is the
        # whole reason they need their own menu.
        audio_clips = state["playlist"].get("audio", [])
        report.expect("there is an audio clip to point at", len(audio_clips) >= 1,
                      len(audio_clips))

        clips_before = len(state["playlist"]["clips"])
        notes_before = len(state["patterns"][0].get("sequences", []))
        revision = read(folder / "sync-status.json")["revision"]

        def right_click(what, name):
            # The app refuses an action it cannot carry out, and Session.run turns that
            # refusal into an exception - so "it was answered" is the absence of one,
            # not a truthy return value. An earlier version of this checked that run()
            # returned something, which it always does.
            try:
                session.run([what])
                answered, why = True, ""
            except RuntimeError as refused:
                answered, why = False, str(refused)

            time.sleep(0.5)
            session.run([{"dismiss_menus": True}])
            time.sleep(0.3)
            report.expect("right-clicking " + name + " is answered", answered, why)
            report.expect("and opening " + name + "'s menu changed nothing",
                          read(folder / "sync-status.json")["revision"] == revision,
                          read(folder / "sync-status.json")["revision"])

        # The lane names down the left of the arrangement. The grid takes a beat and a
        # lane, and a beat of 0 lands inside the name column, which is the point.
        session.run([{"ruler": ["right-click", -1.0, -1.0, 0]}])
        time.sleep(0.5)
        session.run([{"dismiss_menus": True}])
        time.sleep(0.3)
        report.expect("and opening a lane header's menu changed nothing",
                      read(folder / "sync-status.json")["revision"] == revision,
                      read(folder / "sync-status.json")["revision"])
        report.expect("and it did not place a clip, the way the empty-grid menu would",
                      len(session.settled()["playlist"]["clips"]) == clips_before,
                      len(session.settled()["playlist"]["clips"]))

        right_click({"right_click": ["channel", 0]}, "a channel header")
        right_click({"right_click": ["effect", insert_id, 0]}, "an effect in the chain")
        right_click({"right_click": ["effect", insert_id, -1]}, "the empty part of a chain")

        if audio_clips:
            start = audio_clips[0]["start"]
            session.run([{"ruler": ["right-click", start + 0.25, start + 0.25, lanes - 1]}])
            time.sleep(0.5)
            session.run([{"dismiss_menus": True}])
            time.sleep(0.3)
            report.expect("and opening an audio clip's menu changed nothing",
                          read(folder / "sync-status.json")["revision"] == revision,
                          read(folder / "sync-status.json")["revision"])

        after = session.settled()
        report.expect("no clip went anywhere",
                      len(after["playlist"]["clips"]) == clips_before
                      and len(after["playlist"].get("audio", [])) == len(audio_clips),
                      (clips_before, len(after["playlist"]["clips"])))
        report.expect("and no pattern did either",
                      len(after["patterns"][0].get("sequences", [])) == notes_before)

        report.cannot_check("that the menus read right and land under the pointer",
                            "where a menu appears and how it reads are seen, not measured")
    finally:
        session.close()


def write_a_quiet_wav(path):
    """A second of silence at 44.1k, written by hand so the check needs no fixtures."""
    import struct
    frames = 44100
    data = bytes(2 * frames)          # silence, two bytes a frame
    header = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
              + struct.pack("<IHHIIHH", 16, 1, 1, 44100, 88200, 2, 16)
              + b"data" + struct.pack("<I", len(data)))
    path.write_bytes(header + data)


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
    print("double-click opens what is in the clip")
    check_double_click_opens_the_pattern(exe, output / "open", report)
    print()
    print("the editor says how far an edit reaches")
    check_the_editor_says_how_far_an_edit_reaches(exe, output / "reach", report)
    print()
    print("Select selects, Draw draws")
    check_select_does_not_draw(exe, output / "tools", report)
    print()
    print("the arrangement has the same two tools")
    check_select_does_not_place_clips(exe, output / "gridtools", report)
    print()
    print("Alt puts the grid away")
    check_alt_puts_the_grid_away(exe, output / "snap", report)
    print()
    print("lanes can change height")
    check_lanes_can_be_made_taller_and_shorter(exe, output / "lanes", report)
    print()
    print("an effect opens from the chain")
    check_an_effect_can_be_opened_from_the_chain(exe, output / "chain", report)
    print()
    print("one key, one command")
    check_no_two_commands_answer_to_one_key(exe, output / "keys", report)
    print()
    print("Erase and Split are chosen, not stumbled into")
    check_erase_and_split_are_chosen_not_stumbled_into(exe, output / "destructive", report)
    print()
    print("every target answers a right-click, and answering costs nothing")
    check_every_target_has_a_menu_and_opening_it_changes_nothing(exe, output / "targets", report)

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
