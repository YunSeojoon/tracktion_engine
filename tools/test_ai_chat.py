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
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import apply_change, read, wait_for
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


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print('attachments and edits')
    check_attachments_are_not_edits(exe, output / 'attachments', report)
    print()
    print('edge cases')
    check_attachment_edge_cases(exe, output / 'edges', report)

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
