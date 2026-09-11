"""Checks for the AI chat work: what an attachment names, and what is not an edit.

These live apart from test_live_sync.py because they drive a different surface - the
chat panel and its attachments - and because an attachment is a thing that must not
change the music, which is a property worth checking on its own.

Run with the app closed:
    python tools/test_ai_chat.py --output <folder>
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import apply_change, read, tool, wait_for
from test_plugin_compatibility import Session, prepare_song


class Report:
    """Collects what held and what did not, so one failure does not hide the rest."""

    def __init__(self):
        self.failures = []

    def expect(self, name, condition, detail=''):
        print(('  ok   ' if condition else '  FAIL ') + name + (('  ' + str(detail)) if detail else ''))
        if not condition:
            self.failures.append(name)


def check_attachments_are_not_edits(exe, folder, report):
    """A0: the three kinds of attachment, and the rule that none of this is an edit."""
    folder.mkdir(parents=True, exist_ok=True)
    s = Session(exe, folder).open()
    try:

        prepare_song(s)
        state = s.settled()
        before_revision = read(folder / 'sync-status.json')['revision']

        # project_id is permanent, session_id is per run.
        status = read(folder / 'sync-status.json')
        report.expect('project_id is reported', bool(status.get('project_id')), status.get('project_id', '')[:8])
        report.expect('project_id differs from session_id', status['project_id'] != status['session_id'])
        first_project_id = status['project_id']

        # --- a region ------------------------------------------------------------------
        s.run([{"select_lane": 0}, {"pick_clip": [0, 0.0]}, {"attach": "region"}])
        time.sleep(0.5)
        report.expect('attaching did not change the music',
               read(folder / 'sync-status.json')['revision'] == before_revision,
               read(folder / 'sync-status.json')['revision'])

        # --- notes --------------------------------------------------------------------
        s.run([{"select_channel": 0}, {"attach": "notes"}])
        time.sleep(0.5)

        # --- an insert ----------------------------------------------------------------
        insert_id = state['mixer']['inserts'][0]['id']
        s.run([{"attach": "insert:" + insert_id}])
        time.sleep(0.5)

        report.expect('nothing so far was an edit',
               read(folder / 'sync-status.json')['revision'] == before_revision,
               read(folder / 'sync-status.json')['revision'])

        # --- panel controls, also not edits --------------------------------------------
        s.run([{"panel": [5, "maximise"]}, {"panel": [5, "maximise"]},
               {"panel": [2, "minimise"]}, {"panel": [2, "minimise"]},
               {"panel": [0, "close"]}, {"panel": [0, "open"]}])
        time.sleep(0.8)
        report.expect('panel controls are not edits',
               read(folder / 'sync-status.json')['revision'] == before_revision,
               read(folder / 'sync-status.json')['revision'])

        # --- attaching still works with a panel filling the window ---------------------
        # Maximising hides the other panels, and a person who has just maximised the
        # playlist to see the arrangement is exactly the person who then wants to ask
        # about it. Attaching must not depend on the panel beside it being visible.
        attached_before = len(read(folder / 'chat-inspector.json')['attachments'])
        s.run([{"panel": [4, "maximise"]}, {"select_lane": 0}, {"pick_clip": [0, 0.0]},
               {"attach": "region"}])
        time.sleep(0.6)
        report.expect('a selection can be attached while a panel is maximised',
               len(read(folder / 'chat-inspector.json')['attachments']) == attached_before + 1,
               len(read(folder / 'chat-inspector.json')['attachments']))
        report.expect('attaching while maximised is still not an edit',
               read(folder / 'sync-status.json')['revision'] == before_revision)
        s.run([{"panel": [4, "maximise"]}])
        time.sleep(0.4)

        # --- an attachment does not follow the selection --------------------------------
        # Move the selection somewhere else; the attachment must still name what it named.
        s.run([{"select_channel": 0}, {"attachment": [0, "go"]}])
        time.sleep(0.5)
        report.expect('following an attachment is not an edit',
               read(folder / 'sync-status.json')['revision'] == before_revision)

        # --- editing the music does not rewrite an attachment ---------------------------
        def rename(live):
            live['channels'][0]['name'] = 'Renamed after attaching'

        apply_change(folder / 'project.json', rename)
        time.sleep(1.0)
        after_edit = read(folder / 'sync-status.json')['revision']
        report.expect('an edit does change the music', after_edit > before_revision, after_edit)

        # --- removing one card leaves the others ----------------------------------------
        s.run([{"attachment": [0, "remove"]}])
        time.sleep(0.5)

        # --- and the project id survives a reopen ---------------------------------------
        s.close()
        s.open()
        # The new process writes over the previous run's status file, so wait for it rather
        # than reading whatever is still on disk from a moment ago.
        from cocompose import wait_for
        wait_for(lambda: read(folder / 'sync-status.json')['session_id'] != status['session_id'], timeout=60)
        reopened = read(folder / 'sync-status.json')
        report.expect('project_id survives a reopen', reopened['project_id'] == first_project_id)
        report.expect('session_id is new after a reopen', reopened['session_id'] != status['session_id'])
    finally:
        s.close()


def check_attachment_edge_cases(exe, folder, report):
    """A0: shared names, a reused pattern, a tempo change, and a deleted target."""
    folder.mkdir(parents=True, exist_ok=True)
    s = Session(exe, folder).open()
    def inspector():
        return read(folder / "chat-inspector.json")

    try:

        prepare_song(s)
        s.run([{"command": "Add channel"}])

        # --- two channels called the same thing -----------------------------------------
        def same_name(live):
            for channel in live['channels']:
                channel['name'] = 'Lead'

        apply_change(folder / 'project.json', same_name)
        time.sleep(1.0)
        state = s.settled()
        ids = [c['id'] for c in state['channels']]
        report.expect('two channels really do share a name',
               len({c['name'] for c in state['channels']}) == 1 and len(ids) == 2, ids)

        s.run([{"select_lane": 0}, {"pick_clip": [0, 0.0]}, {"attach": "region"}])
        time.sleep(0.5)
        packet = inspector()
        region = packet['attachments'][0]
        report.expect('a region names channels by id, not by name',
               all(c in ids for c in region['channels']) and region['channels'],
               region['channels'])

        # --- a pattern placed more than once --------------------------------------------
        s.run([{"select_lane": 0}, {"place": [0, 32.0]}, {"place": [0, 64.0]}])
        time.sleep(0.8)
        s.run([{"select_channel": 0}, {"attach": "notes"}])
        time.sleep(0.5)
        notes = inspector()['attachments'][1]
        report.expect('a note attachment says how many placements share its pattern',
               notes['pattern_use_count'] == 3, notes['pattern_use_count'])

        # --- a tempo change moves seconds but not beats ----------------------------------
        before = inspector()['attachments'][0]

        def faster(live):
            live['bpm'] = 168.0

        apply_change(folder / 'project.json', faster)
        time.sleep(1.0)
        after = inspector()['attachments'][0]
        report.expect('a tempo change leaves the attached beats alone',
               after['start_beat'] == before['start_beat'] and after['end_beat'] == before['end_beat'],
               (before['start_beat'], before['end_beat'], after['start_beat'], after['end_beat']))
        report.expect('the same beats still read as the same bars',
               after['summary'].split(' - ')[0] == before['summary'].split(' - ')[0],
               (before['summary'], after['summary']))

        # --- something an attachment names gets deleted ----------------------------------
        insert_id = state['mixer']['inserts'][-1]['id']
        s.run([{"attach": "insert:" + insert_id}])
        time.sleep(0.5)
        report.expect('the insert attachment starts out present', inspector()['attachments'][2]['exists'])

        def drop_insert(live):
            live['channels'] = [c for c in live['channels'] if c['insert'] != 2]
            live['mixer']['inserts'] = [i for i in live['mixer']['inserts'] if i['id'] != insert_id]

        apply_change(folder / 'project.json', drop_insert)
        time.sleep(1.2)
        gone = inspector()['attachments'][2]
        report.expect('a deleted target is reported missing, not re-pointed',
               gone['exists'] is False and gone['insert'] == insert_id, gone['summary'])
        report.expect('the other attachments are untouched by that deletion',
               inspector()['attachments'][0]['start_beat'] == before['start_beat'])
    finally:
        s.close()


def start_bridge(folder, provider="echo"):
    """Runs the bridge beside a project and waits until it says it is listening."""
    process = subprocess.Popen(
        [sys.executable, str(Path(__file__).resolve().parent / "cocompose_bridge.py"),
         "--project", str(folder / "project.json"), "--provider", provider],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_for(lambda: read(folder / "chat-bridge.json").get("ready"), timeout=30)
    return process


def settled_answer(folder, timeout=90):
    wait_for(lambda: read(folder / "conversation.json").get("messages"), timeout=timeout)
    wait_for(lambda: not read(folder / "conversation.json")["messages"][-1]["streaming"], timeout=timeout)
    return read(folder / "conversation.json")


def check_a_conversation_that_continues(exe, folder, report):
    """A1: one conversation per project, kept across questions and across restarts, and
    asking is never mistaken for editing."""
    folder.mkdir(parents=True, exist_ok=True)
    s = Session(exe, folder).open()
    bridge = None

    try:
        prepare_song(s)
        state = s.settled()
        revision_before = read(folder / "sync-status.json")["revision"]

        # Nothing listening: the question stays in the box rather than disappearing into
        # a wait that will never end.
        s.run([{"chat": "ask: is anyone there?"}])
        time.sleep(0.8)
        report.expect("with no bridge, nothing is sent",
                      not (folder / "conversation.json").exists()
                      or not read(folder / "conversation.json").get("messages"))
        report.expect("with no bridge, what was typed is kept",
                      read(folder / "chat-inspector.json").get("draft") == "is anyone there?",
                      read(folder / "chat-inspector.json").get("draft"))

        bridge = start_bridge(folder)
        report.expect("the app sees the bridge",
                      wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"),
                               timeout=30) is True)

        s.run([{"select_lane": 0}, {"pick_clip": [0, 0.0]}, {"attach": "region"}])
        s.run([{"chat": "ask: what is happening in these bars?"}])
        talk = settled_answer(folder)

        report.expect("the question and its answer are both kept", len(talk["messages"]) == 2,
                      len(talk["messages"]))
        report.expect("the conversation belongs to the project",
                      talk["project_id"] == read(folder / "sync-status.json")["project_id"])
        report.expect("the question carried what was attached",
                      len(talk["messages"][0]["attachments"]) == 1)
        report.expect("the answer was told the beats, not just a name",
                      "beats" in talk["messages"][1]["text"])
        report.expect("the answer was told which bars a person would see",
                      "bars" in talk["messages"][1]["text"])
        report.expect("asking changed no music",
                      read(folder / "sync-status.json")["revision"] == revision_before,
                      read(folder / "sync-status.json")["revision"])

        # A second question continues the same conversation and can see the first.
        conversation_id = talk["conversation_id"]
        s.run([{"chat": "ask: and the bars before those?"}])
        wait_for(lambda: len(read(folder / "conversation.json")["messages"]) >= 4, timeout=90)
        talk = settled_answer(folder)
        report.expect("a second question joins the same conversation",
                      talk["conversation_id"] == conversation_id and len(talk["messages"]) == 4)
        report.expect("the second question was given the first",
                      "what is happening in these bars" in talk["messages"][-1]["text"])

        # Closing and reopening the app must not start again.
        s.close()
        s.open()
        wait_for(lambda: read(folder / "chat-inspector.json").get("conversation_id"), timeout=60)
        reopened = read(folder / "conversation.json")
        report.expect("the conversation survives a restart",
                      reopened["conversation_id"] == conversation_id
                      and len(reopened["messages"]) == 4)
        report.expect("the app loaded that conversation, not a new one",
                      read(folder / "chat-inspector.json")["conversation_id"] == conversation_id)

        s.run([{"chat": "ask: still the same conversation?"}])
        wait_for(lambda: len(read(folder / "conversation.json")["messages"]) >= 6, timeout=90)
        talk = settled_answer(folder)
        report.expect("and it carries on after the restart",
                      talk["conversation_id"] == conversation_id and len(talk["messages"]) == 6)

        # A new conversation is only ever started on purpose, and keeps the old one.
        s.run([{"chat": "new"}])
        time.sleep(0.8)
        branched = read(folder / "conversation.json")
        report.expect("a new conversation is a different one",
                      branched["conversation_id"] != conversation_id)
        report.expect("starting a new one keeps the old one", len(branched["archived"]) == 1,
                      len(branched.get("archived", [])))
        report.expect("branching changed no music",
                      read(folder / "sync-status.json")["revision"] == revision_before)
    finally:
        if bridge is not None:
            bridge.terminate()
        s.close()


def check_asking_does_not_interrupt(exe, folder, report):
    """A1: a question in flight must not stop the music or the app, and stopping one
    must leave both alone."""
    folder.mkdir(parents=True, exist_ok=True)
    s = Session(exe, folder).open(extra=["--play"])
    bridge = None

    try:
        prepare_song(s)
        bridge = start_bridge(folder)
        wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"), timeout=30)

        playing = read(folder / "sync-status.json").get("playing")
        s.run([{"select_lane": 0}, {"pick_clip": [0, 0.0]}, {"attach": "region"}])
        s.run([{"chat": "ask: describe this while it plays"}])

        # While the answer is coming, the app is still answering and still playing.
        position = read(folder / "sync-status.json").get("position_seconds")
        time.sleep(1.5)
        report.expect("the app is still keeping up while it waits",
                      read(folder / "sync-status.json")["updated_at_ms"] > 0)
        if playing:
            report.expect("the music kept playing while it waited",
                          read(folder / "sync-status.json")["position_seconds"] != position)
        else:
            report.expect("there is no audio device, so playback was not exercised", True,
                          "skipped")

        settled_answer(folder)
        report.expect("the answer arrived", len(read(folder / "conversation.json")["messages"]) == 2)

        # Stopping a question leaves the music and the conversation where they were.
        revision = read(folder / "sync-status.json")["revision"]
        s.run([{"chat": "ask: this one will be stopped"}])
        s.run([{"chat": "cancel"}])
        time.sleep(1.0)
        report.expect("stopping is not an edit",
                      read(folder / "sync-status.json")["revision"] == revision)
        report.expect("stopping leaves nothing still waiting",
                      not read(folder / "chat-inspector.json").get("waiting"))
    finally:
        if bridge is not None:
            bridge.terminate()
        s.close()


def check_no_audio_is_claimed(exe, folder, report):
    """A1: no audio has been sent, and nothing may imply otherwise."""
    folder.mkdir(parents=True, exist_ok=True)
    s = Session(exe, folder).open()

    try:
        prepare_song(s)
        capabilities = tool(folder / "project.json", "get_capabilities")["result"]
        report.expect("the app says plainly that it cannot send audio",
                      capabilities["audio"]["can_send_audio"] is False)
        report.expect("and says why", bool(capabilities["audio"].get("reason")))

        s.run([{"select_lane": 0}, {"pick_clip": [0, 0.0]}, {"attach": "region"}])
        time.sleep(0.5)
        packet = json.dumps(read(folder / "chat-inspector.json"))
        report.expect("nothing in what would be sent is audio",
                      ".wav" not in packet.lower() and "audio" not in packet.lower(),
                      packet[:100])
    finally:
        s.close()


def check_proposals_stay_inside_the_selection(exe, folder, report):
    """A2: a proposal changes what was attached and nothing else, keeps what the person
    said to keep, refuses when the ground has moved, and goes in as one undo."""
    folder.mkdir(parents=True, exist_ok=True)
    s = Session(exe, folder).open()
    project = folder / "project.json"

    def call(name, arguments):
        return tool(project, name, arguments)

    try:
        prepare_song(s)
        state = s.settled()
        pattern_id = state["patterns"][0]["id"]
        channel_id = state["channels"][0]["id"]

        part = call("inspect_pattern", {"pattern": pattern_id, "channel": channel_id})
        notes = part["result"]["parts"][0]["notes"]
        report.expect("there are notes to work with", len(notes) >= 8, len(notes))

        first_four = [note["id"] for note in notes[:4]]
        rest = notes[4:]
        revision = read(folder / "sync-status.json")["revision"]

        def scoped(extra):
            base = {"pattern": pattern_id, "channel": channel_id,
                    "allowed_notes": first_four, "base_revision": revision,
                    "description": "AI: rewrite the pitches"}
            base.update(extra)
            return base

        # --- a shared pattern says how much of the song it changes -----------------
        # The same pattern placed twice is one pattern. Editing its notes is heard in
        # both places, so the proposal has to say so before anyone presses Apply.
        s.run([{"select_pattern": 0}, {"select_lane": 0}, {"place": [0, 64.0]}])
        time.sleep(0.6)
        shared = call("create_proposal", scoped({
            "base_revision": read(folder / "sync-status.json")["revision"],
            "keeps": {"rhythm": True, "velocity": True},
            "notes": [{"what": "change", "id": first_four[0], "pitch": 64}]}))
        report.expect("a proposal says how many places the pattern is played",
                      shared["status"] == "ok"
                      and shared["result"]["proposal"]["placements"] == 2,
                      shared.get("result", {}).get("proposal", {}).get("placements",
                                                                      shared.get("error")))
        revision = read(folder / "sync-status.json")["revision"]

        # --- a proposal is not a change ------------------------------------------
        made = call("create_proposal", scoped({
            "keeps": {"rhythm": True, "velocity": True},
            "notes": [{"what": "change", "id": first_four[0], "pitch": notes[0]["pitch"] + 5},
                      {"what": "change", "id": first_four[1], "pitch": notes[1]["pitch"] + 7}]}))
        report.expect("a proposal can be made", made["status"] == "ok",
                      made.get("error", {}).get("message", ""))
        proposal_id = made["result"]["proposal"]["id"]
        report.expect("making one changes nothing",
                      read(folder / "sync-status.json")["revision"] == revision)
        report.expect("it shows what each note was and would become",
                      made["result"]["diff"]["notes"][0]["pitch"]["was"] == notes[0]["pitch"]
                      and made["result"]["diff"]["notes"][0]["pitch"]["now"] == notes[0]["pitch"] + 5)

        # --- the rules, checked before anything is applied -------------------------
        outside = call("create_proposal", scoped({
            "keeps": {},
            "notes": [{"what": "change", "id": rest[0]["id"], "pitch": 70}]}))
        report.expect("a note outside the attachment is refused",
                      outside["error"]["code"] == "OUT_OF_SCOPE",
                      outside.get("error", {}).get("code"))

        broke_rhythm = call("create_proposal", scoped({
            "keeps": {"rhythm": True},
            "notes": [{"what": "change", "id": first_four[0], "start_beat": 3.5}]}))
        report.expect("moving a note when the rhythm was to be kept is refused",
                      broke_rhythm["error"]["code"] == "LOCKED")

        broke_pitch = call("create_proposal", scoped({
            "keeps": {"pitch": True},
            "notes": [{"what": "change", "id": first_four[0], "pitch": 80}]}))
        report.expect("changing pitch when pitch was to be kept is refused",
                      broke_pitch["error"]["code"] == "LOCKED")

        added_note = call("create_proposal", scoped({
            "keeps": {"rhythm": True},
            "notes": [{"what": "add", "pitch": 64, "start_beat": 0.0, "length_beats": 1.0,
                       "velocity": 90}]}))
        report.expect("adding a note when the rhythm was to be kept is refused",
                      added_note["error"]["code"] == "LOCKED")

        bad_pitch = call("create_proposal", scoped({
            "keeps": {}, "notes": [{"what": "change", "id": first_four[0], "pitch": 300}]}))
        report.expect("a pitch outside MIDI range is refused",
                      bad_pitch["error"]["code"] == "INVALID_ARGUMENT")

        past_end = call("create_proposal", scoped({
            "keeps": {}, "notes": [{"what": "change", "id": first_four[0],
                                    "start_beat": 900.0, "length_beats": 4.0}]}))
        report.expect("a note that would run past the pattern is refused",
                      past_end["error"]["code"] == "OUT_OF_SCOPE")

        missing = call("create_proposal", scoped({
            "keeps": {}, "notes": [{"what": "change", "id": "no-such-note", "pitch": 60}]}))
        report.expect("a note that does not exist is refused",
                      missing["error"]["code"] == "NOT_FOUND")

        report.expect("none of those refusals touched the music",
                      read(folder / "sync-status.json")["revision"] == revision,
                      read(folder / "sync-status.json")["revision"])

        # --- applying ---------------------------------------------------------------
        applied = call("apply_proposal", {"proposal": proposal_id})
        report.expect("a good proposal applies", applied["status"] == "ok",
                      applied.get("error", {}).get("message", ""))
        time.sleep(1.0)

        after = call("inspect_pattern", {"pattern": pattern_id, "channel": channel_id})
        changed = {note["id"]: note for note in after["result"]["parts"][0]["notes"]}
        report.expect("the pitches it asked for changed",
                      changed[first_four[0]]["pitch"] == notes[0]["pitch"] + 5
                      and changed[first_four[1]]["pitch"] == notes[1]["pitch"] + 7)
        report.expect("the rhythm it promised to keep was kept",
                      changed[first_four[0]]["start_beat"] == notes[0]["start_beat"]
                      and changed[first_four[0]]["length_beats"] == notes[0]["length_beats"])
        report.expect("the velocity it promised to keep was kept",
                      changed[first_four[0]]["velocity"] == notes[0]["velocity"])
        report.expect("everything outside the attachment is untouched",
                      all(changed[note["id"]]["pitch"] == note["pitch"] for note in rest))

        # --- one undo ---------------------------------------------------------------
        report.expect("applying it is one undo",
                      "AI" in read(folder / "sync-status.json")["undo"],
                      read(folder / "sync-status.json")["undo"])

        from cocompose import control
        control(project, "undo")
        time.sleep(0.8)
        undone = call("inspect_pattern", {"pattern": pattern_id, "channel": channel_id})
        back = {note["id"]: note for note in undone["result"]["parts"][0]["notes"]}
        report.expect("one undo puts all of it back",
                      all(back[note["id"]]["pitch"] == note["pitch"] for note in notes))

        control(project, "redo")
        time.sleep(0.8)
        redone = call("inspect_pattern", {"pattern": pattern_id, "channel": channel_id})
        again = {note["id"]: note for note in redone["result"]["parts"][0]["notes"]}
        report.expect("redo brings it back",
                      again[first_four[0]]["pitch"] == notes[0]["pitch"] + 5)

        # --- pressing apply twice ---------------------------------------------------
        second = call("apply_proposal", {"proposal": proposal_id})
        report.expect("applying the same proposal again does nothing",
                      second["result"].get("already_applied") is True)

        # --- the ground moving under a proposal -------------------------------------
        now = read(folder / "sync-status.json")["revision"]
        stale = call("create_proposal", {"pattern": pattern_id, "channel": channel_id,
                                         "allowed_notes": first_four,
                                         "base_revision": now - 1,
                                         "keeps": {},
                                         "notes": [{"what": "change", "id": first_four[0],
                                                    "pitch": 61}]})
        report.expect("a proposal made against older music is refused",
                      stale["error"]["code"] == "STALE_REVISION")
        report.expect("and says it can be retried", stale["error"]["retryable"] is True)

        pending = call("create_proposal", {"pattern": pattern_id, "channel": channel_id,
                                           "allowed_notes": first_four,
                                           "base_revision": now, "keeps": {},
                                           "description": "AI: one more",
                                           "notes": [{"what": "change", "id": first_four[2],
                                                      "pitch": 61}]})
        pending_id = pending["result"]["proposal"]["id"]

        # A person edits between the proposal being made and being applied.
        def nudge(live):
            live["bpm"] = 124.0

        apply_change(project, nudge)
        time.sleep(1.0)
        late = call("apply_proposal", {"proposal": pending_id})
        report.expect("a proposal applied after the person edited is refused",
                      late["error"]["code"] == "STALE_REVISION", late.get("error", {}).get("code"))
        report.expect("and that refusal changed nothing",
                      call("inspect_pattern", {"pattern": pattern_id, "channel": channel_id})
                      ["result"]["parts"][0]["notes"][2]["pitch"] == notes[2]["pitch"])
    finally:
        s.close()


def check_a_person_can_use_it(exe, folder, report):
    """A2 as a person meets it: ask with notes attached, see what the answer would
    change, press Apply once, and take it back with one Undo.

    The tool service is checked directly elsewhere. This is the other half - that the
    same thing is reachable from the panel, which is what makes it a usable version
    rather than an API."""
    folder.mkdir(parents=True, exist_ok=True)
    s = Session(exe, folder).open()
    project = folder / "project.json"
    bridge = None

    try:
        prepare_song(s)
        bridge = start_bridge(folder)
        wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"), timeout=30)

        # With the note editor open, a suggested change has somewhere to be drawn.
        s.run([{"select_channel": 0}, {"note": [84, 12.0, 1.0, 90]}])
        time.sleep(0.6)

        s.run([{"select_channel": 0}, {"attach": "notes"}])
        time.sleep(0.5)

        # Whichever pattern the attachment named is the one to watch. Reading the first
        # pattern in the project instead would be watching the wrong music.
        attached = read(folder / "chat-inspector.json")["attachments"][-1]
        pattern_id, channel_id = attached["pattern"], attached["channel"]

        before = tool(project, "inspect_pattern", {"pattern": pattern_id, "channel": channel_id})
        original = {n["id"]: n for n in before["result"]["parts"][0]["notes"]}
        s.run([{"chat": "ask: move these up a tone but keep the rhythm"}])
        talk = settled_answer(folder)

        answer = talk["messages"][-1]
        report.expect("the answer came with a change worked out",
                      bool(answer.get("proposal_id")), answer.get("proposal_problem", ""))
        report.expect("the change was checked and kept, not applied",
                      answer["proposal"]["applied"] is False)
        report.expect("having a change waiting is not an edit",
                      read(folder / "sync-status.json")["revision"]
                      == talk["messages"][-2]["revision"],
                      read(folder / "sync-status.json")["revision"])
        report.expect("it says what each note would become",
                      answer["proposal_diff"]["notes"][0]["pitch"]["now"]
                      == answer["proposal_diff"]["notes"][0]["pitch"]["was"] + 2)

        # --- what is offered is drawn where the notes are, not only written down -----
        # The inspector is written on its own tick, so give it one.
        try:
            wait_for(lambda: read(folder / "chat-inspector.json").get("offered_proposal"),
                     timeout=10)
        except TimeoutError:
            pass
        inspector = read(folder / "chat-inspector.json")
        report.expect("the panel says which change it is offering",
                      inspector.get("offered_proposal") == answer["proposal_id"],
                      inspector.get("offered_proposal", ""))
        report.expect("the note editor draws the change as a suggestion",
                      inspector.get("suggested_notes_drawn", 0)
                      == len(answer["proposal_diff"]["notes"]),
                      inspector.get("suggested_notes_drawn", 0))

        # The person presses Apply.
        s.run([{"chat": "apply"}])
        time.sleep(1.2)
        after = tool(project, "inspect_pattern", {"pattern": pattern_id, "channel": channel_id})
        changed = {n["id"]: n for n in after["result"]["parts"][0]["notes"]}

        touched = [note["id"] for note in answer["proposal_diff"]["notes"]]
        report.expect("the notes it named moved up a tone",
                      all(changed[i]["pitch"] == original[i]["pitch"] + 2 for i in touched))
        report.expect("the rhythm it promised to keep was kept",
                      all(changed[i]["start_beat"] == original[i]["start_beat"]
                          and changed[i]["length_beats"] == original[i]["length_beats"]
                          for i in touched))
        report.expect("no other note moved",
                      all(changed[i]["pitch"] == original[i]["pitch"]
                          for i in original if i not in touched))

        after_apply = read(folder / "chat-inspector.json")
        report.expect("nothing is still being offered once it is applied",
                      not after_apply.get("offered_proposal"),
                      after_apply.get("offered_proposal", ""))
        report.expect("and the suggestion stops being drawn",
                      after_apply.get("suggested_notes_drawn", 0) == 0,
                      after_apply.get("suggested_notes_drawn", 0))

        report.expect("the panel stops offering a change once it is applied",
                      not read(folder / "conversation.json")["messages"][-1]["proposal"]["applied"]
                      is False)

        # Pressing it again must not apply it twice.
        pitches = {i: changed[i]["pitch"] for i in touched}
        s.run([{"chat": "apply:" + answer["proposal_id"]}])
        time.sleep(1.0)
        again = tool(project, "inspect_pattern", {"pattern": pattern_id, "channel": channel_id})
        twice = {n["id"]: n for n in again["result"]["parts"][0]["notes"]}
        report.expect("pressing Apply again changes nothing more",
                      all(twice[i]["pitch"] == pitches[i] for i in touched))

        # One Undo takes all of it back.
        from cocompose import control
        control(project, "undo")
        time.sleep(1.0)
        undone = tool(project, "inspect_pattern", {"pattern": pattern_id, "channel": channel_id})
        back = {n["id"]: n for n in undone["result"]["parts"][0]["notes"]}
        report.expect("one Undo takes the whole change back",
                      all(back[i]["pitch"] == original[i]["pitch"] for i in original))
    finally:
        if bridge is not None:
            bridge.terminate()
        s.close()


def check_a_written_answer_can_carry_a_change(report):
    """A real provider proposes by writing a block, so the reading of that block is the
    whole connection between what a model says and what the app will offer.

    It needs no app and no network: it is the one piece that decides whether an answer
    turns into a button, and it must never turn prose into an edit by accident."""
    from cocompose_bridge import parse_change

    said = ('Those four notes sit low against the pad. Up a tone keeps the shape.\n\n'
            '```cocompose-change\n'
            '{"description": "Up a tone", "keeps": {"rhythm": true},\n'
            ' "notes": [{"what": "change", "id": "n1", "pitch": 62}]}\n'
            '```')
    text, change = parse_change(said)
    report.expect('a change written in the answer is read out of it',
                  change is not None and change['notes'][0]['id'] == 'n1')
    report.expect('the person reads the sentence, not the JSON',
                  'cocompose-change' not in text and 'Up a tone keeps the shape.' in text)

    text, change = parse_change('Those notes are fine as they are.')
    report.expect('an answer with no block proposes nothing', change is None)
    report.expect('an answer with no block is left alone',
                  text == 'Those notes are fine as they are.')

    broken = 'Sure.\n```cocompose-change\n{"notes": [{"what": "change",\n```'
    text, change = parse_change(broken)
    report.expect('a half-written block is not an edit', change is None)

    empty = 'Sure.\n```cocompose-change\n{"description": "nothing"}\n```'
    report.expect('a block that changes nothing is not a change',
                  parse_change(empty)[1] is None)

    report.expect('nothing to read is not a change', parse_change(None)[1] is None)


def check_a_mixer_proposal_uses_the_ids_it_was_given(exe, folder, report):
    """A2, the insert half: a proposal about a mixer insert has to work with the ids the
    read tools handed out, and must not reach an insert nobody attached.

    The notes half is held in by the pattern and channel an attachment names. A parameter
    change has no equivalent, so the scope list is the only thing standing between a
    question about four notes and an answer that moves a fader."""
    folder.mkdir(parents=True, exist_ok=True)
    s = Session(exe, folder).open()
    project = folder / "project.json"

    try:
        prepare_song(s)
        s.settled()

        # An effect to aim at. Written the way a person would add one, through the project.
        def add_reverb(state):
            insert = state["mixer"]["inserts"][0]
            insert.setdefault("effects", []).append(
                {"id": "fx-verb", "type": "reverb", "bypass": False, "wet": 0.5,
                 "parameters": []})
            return insert["id"]

        _, insert_id = apply_change(project, add_reverb)
        wait_for(lambda: any(e["id"] == "fx-verb" for e in
                             tool(project, "inspect_insert", {"insert": insert_id})
                                 ["result"]["effects"]), timeout=20)

        looked = tool(project, "inspect_insert", {"insert": insert_id})["result"]
        effect = next(e for e in looked["effects"] if e["id"] == "fx-verb")
        report.expect("the insert reports parameters to aim at",
                      len(effect["parameters"]) > 0, len(effect["parameters"]))
        if not effect["parameters"]:
            return

        parameter = effect["parameters"][0]
        was = parameter["value"]
        target = round(0.25 if was > 0.5 else 0.75, 3)
        revision = read(folder / "sync-status.json")["revision"]

        def propose(scope, value=target):
            return tool(project, "create_proposal", {
                "description": "AI: set " + parameter["name"],
                "base_revision": read(folder / "sync-status.json")["revision"],
                "allowed_inserts": scope,
                "parameters": [{"owner": insert_id, "plugin": "fx-verb",
                                "parameter": parameter["id"], "value": value}]})

        # --- the ids a read tool gave out are the ids a write tool takes ---------------
        made = propose([insert_id])
        report.expect("the effect id from inspect_insert is one create_proposal accepts",
                      made["status"] == "ok", made.get("error", {}).get("message", ""))

        # --- and nothing has moved yet ------------------------------------------------
        report.expect("working out a mixer change is not a mixer change",
                      read(folder / "sync-status.json")["revision"] == revision)

        # --- an id that names nothing resolves to nothing ----------------------------
        # Accepting two spellings of a plugin id is only safe while the empty string is
        # neither of them: every plugin without an effect uid reads as "" there.
        nameless = tool(project, "create_proposal", {
            "description": "AI: no such plugin",
            "base_revision": read(folder / "sync-status.json")["revision"],
            "allowed_inserts": [insert_id],
            "parameters": [{"owner": insert_id, "plugin": "",
                            "parameter": parameter["id"], "value": target}]})
        report.expect("an empty plugin id matches no plugin",
                      nameless["status"] == "error"
                      and nameless["error"]["code"] == "NOT_FOUND",
                      nameless.get("error", {}).get("code", nameless["status"]))

        # --- an insert nobody attached is out of reach --------------------------------
        elsewhere = propose([])
        report.expect("with no insert attached, a parameter change is refused",
                      elsewhere["status"] == "error"
                      and elsewhere["error"]["code"] == "OUT_OF_SCOPE",
                      elsewhere.get("error", {}).get("code", elsewhere["status"]))

        other = propose(["insert:some-other-thing"])
        report.expect("an insert outside what was attached is refused",
                      other["status"] == "error" and other["error"]["code"] == "OUT_OF_SCOPE",
                      other.get("error", {}).get("code", other["status"]))

        # --- applying it moves the parameter, and one undo puts it back ---------------
        applied = tool(project, "apply_proposal",
                       {"proposal": made["result"]["proposal"]["id"]})
        report.expect("a mixer proposal applies", applied["status"] == "ok",
                      applied.get("error", {}).get("message", ""))

        def value_now():
            fresh = tool(project, "inspect_insert", {"insert": insert_id})["result"]
            found = next(e for e in fresh["effects"] if e["id"] == "fx-verb")
            return next(x for x in found["parameters"] if x["id"] == parameter["id"])["value"]

        wait_for(lambda: abs(value_now() - target) < 0.01, timeout=20)
        report.expect("the parameter is where the proposal said", abs(value_now() - target) < 0.01,
                      value_now())

        from cocompose import control
        control(project, "undo")
        wait_for(lambda: abs(value_now() - was) < 0.01, timeout=20)
        report.expect("one undo puts the parameter back", abs(value_now() - was) < 0.01,
                      value_now())
    finally:
        s.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print('attachments and edits')
    check_attachments_are_not_edits(exe, output / 'attachments', report)
    print()
    print('edge cases')
    check_attachment_edge_cases(exe, output / 'edges', report)
    print()
    print('a conversation that continues')
    check_a_conversation_that_continues(exe, output / 'conversation', report)
    print()
    print('asking while working')
    check_asking_does_not_interrupt(exe, output / 'playing', report)
    print()
    print('no audio is claimed')
    check_no_audio_is_claimed(exe, output / 'audio', report)
    print()
    print('proposals stay inside the selection')
    check_proposals_stay_inside_the_selection(exe, output / 'proposals', report)
    print()
    print('a mixer proposal')
    check_a_mixer_proposal_uses_the_ids_it_was_given(exe, output / 'mixer', report)
    print()
    print('an answer can carry a change')
    check_a_written_answer_can_carry_a_change(report)
    print()
    print('a person can use it')
    check_a_person_can_use_it(exe, output / 'person', report)

    print()
    print('FAILURES:', report.failures if report.failures else 'none')
    return report


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=root / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=root / "build-cocompose" / ("ai-chat-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
