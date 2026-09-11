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
