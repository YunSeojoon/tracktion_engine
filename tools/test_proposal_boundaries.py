"""The three holes the fourth review reproduced, turned into checks that expect refusal.

Each of these was a case where the app's own scenarios passed while a synthetic reply
could still do something it was never allowed to. The scenarios drove the happy path;
nobody had driven a reply that lied about its scope, or one that arrived after the
person had already changed their mind. These do. No model is involved - a fixture
bridge writes the reply straight into chat-reply.json, which is exactly what a
misbehaving provider would do, and the point is that the app must not care who wrote it.

    python tools/test_proposal_boundaries.py
"""
import argparse
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import atomic_write, read, tool, wait_for, apply_change
from cocompose_bridge import Liveness
from test_plugin_compatibility import Session, prepare_song

ROOT = Path(__file__).resolve().parents[1]


class Report:
    def __init__(self):
        self.failures = []

    def expect(self, name, condition, detail=''):
        print(('  ok   ' if condition else '  FAIL ') + name + (('  ' + str(detail)) if detail else ''))
        if not condition:
            self.failures.append(name)


def notes_of(project, pattern, channel):
    return tool(project, 'inspect_pattern',
                {'pattern': pattern, 'channel': channel})['result']['parts'][0]['notes']


def answer_after_reply(session, folder, request_id, change):
    """Writes a reply the way a bridge would and waits for the app to consider it."""
    atomic_write(folder / 'chat-reply.json',
                 {'request_id': request_id, 'status': 'ok', 'text': 'a synthetic reply',
                  'provider': 'fixture', 'change': change})

    def considered():
        last = read(folder / 'conversation.json')['messages'][-1]
        return last if last.get('proposal_id') or last.get('proposal_problem') else None

    return wait_for(considered, timeout=20, what='the app to consider the reply')


def with_fixture_bridge(session, folder, attach, ask_then):
    """Runs `ask_then(request)` with a fixture bridge claiming to be connected."""
    session.run([{'attach': attach}])
    with Liveness(folder / 'chat-bridge.json', 'fixture bridge'):
        wait_for(lambda: read(folder / 'chat-inspector.json').get('bridge_connected'), timeout=20,
                 what='the app to see the fixture bridge')
        session.run([{'chat': 'ask: review boundary'}])
        request = wait_for(lambda: read(folder / 'chat-request.json'), timeout=20,
                           what='the question to be written')
        return ask_then(request)


# --- R1 -------------------------------------------------------------------------------

def check_a_late_reply_cannot_overwrite_what_the_person_changed(exe, folder, report):
    """The question left at revision N. The person edited while waiting. The answer
    arrives - and it is an answer to the old question, about the old music."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / 'project.json'
    session = Session(exe, folder).open()
    try:
        pattern = prepare_song(session)
        state = session.settled()
        channel = state['channels'][0]['id']
        note = notes_of(project, pattern, channel)[0]

        def edit_while_waiting(request):
            # The person changes their mind before the answer lands.
            session.run([{'command': 'Transpose pattern up'}])
            moved = session.settled()['revision']
            report.expect('the person edited after asking',
                          moved != request['revision'], (request['revision'], moved))
            theirs = next(n for n in notes_of(project, pattern, channel) if n['id'] == note['id'])['pitch']

            answer = answer_after_reply(session, folder, request['request_id'],
                                        {'pattern': pattern, 'channel': channel,
                                         'base_revision': moved,   # a reply claiming freshness
                                         'notes': [{'id': note['id'], 'pitch': 85}]})
            return answer, theirs

        answer, theirs = with_fixture_bridge(session, folder, 'notes', edit_while_waiting)

        report.expect('a reply to an old question is not offered as a change',
                      not answer.get('proposal_id'), answer.get('proposal_id'))
        report.expect('and it says why',
                      'STALE_REVISION' in (answer.get('proposal_problem') or ''),
                      answer.get('proposal_problem'))
        report.expect("the person's own edit is untouched",
                      next(n for n in notes_of(project, pattern, channel) if n['id'] == note['id'])['pitch'] == theirs)
        report.expect("a reply's own claim about revision counts for nothing",
                      not answer.get('proposal_id'))
    finally:
        session.close()


# --- R2 -------------------------------------------------------------------------------

def check_a_reply_cannot_name_its_own_scope(exe, folder, report):
    """Only a mixer insert was attached. The reply names a real pattern, channel and
    note id anyway. The scope it names is not the scope it has."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / 'project.json'
    session = Session(exe, folder).open()
    try:
        pattern = prepare_song(session)
        state = session.settled()
        channel = state['channels'][0]['id']
        insert = state['mixer']['inserts'][0]['id']
        note = notes_of(project, pattern, channel)[0]
        before = note['pitch']

        def reply_reaching_for_notes(request):
            return answer_after_reply(session, folder, request['request_id'],
                                      {'pattern': pattern, 'channel': channel,
                                       'notes': [{'id': note['id'], 'pitch': 85}]})

        answer = with_fixture_bridge(session, folder, 'insert:' + insert, reply_reaching_for_notes)

        report.expect('with only an insert attached, a note change is not offered',
                      not answer.get('proposal_id'), answer.get('proposal_id'))
        report.expect('and the refusal is about scope',
                      'OUT_OF_SCOPE' in (answer.get('proposal_problem') or '')
                      or 'NOT_FOUND' in (answer.get('proposal_problem') or ''),
                      answer.get('proposal_problem'))
        report.expect('the note is where it was',
                      next(n for n in notes_of(project, pattern, channel) if n['id'] == note['id'])['pitch'] == before)
    finally:
        session.close()


def check_a_reply_with_nothing_attached_changes_nothing(exe, folder, report):
    """No attachment at all. The reply names everything it needs. Still no."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / 'project.json'
    session = Session(exe, folder).open()
    try:
        pattern = prepare_song(session)
        state = session.settled()
        channel = state['channels'][0]['id']
        note = notes_of(project, pattern, channel)[0]

        with Liveness(folder / 'chat-bridge.json', 'fixture bridge'):
            wait_for(lambda: read(folder / 'chat-inspector.json').get('bridge_connected'), timeout=20,
                     what='the app to see the fixture bridge')
            session.run([{'chat': 'ask: nothing attached'}])
            request = wait_for(lambda: read(folder / 'chat-request.json'), timeout=20,
                               what='the question to be written')
            answer = answer_after_reply(session, folder, request['request_id'],
                                        {'pattern': pattern, 'channel': channel,
                                         'notes': [{'id': note['id'], 'pitch': 85}]})

        report.expect('with nothing attached, nothing is offered',
                      not answer.get('proposal_id'), answer.get('proposal_id'))
        report.expect('the note is where it was',
                      next(n for n in notes_of(project, pattern, channel) if n['id'] == note['id'])['pitch'] == note['pitch'])
    finally:
        session.close()


def check_the_service_itself_reads_empty_as_none(exe, folder, report):
    """The service, not just the app: an empty allowed list is no permission."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / 'project.json'
    session = Session(exe, folder).open()
    try:
        pattern = prepare_song(session)
        state = session.settled()
        channel = state['channels'][0]['id']
        note = notes_of(project, pattern, channel)[0]

        made = tool(project, 'create_proposal', {
            'pattern': pattern, 'channel': channel, 'allowed_notes': [],
            'base_revision': state['revision'],
            'notes': [{'what': 'change', 'id': note['id'], 'pitch': 85}]})
        report.expect('an empty allowed list is refused, not read as everything',
                      made['status'] == 'error' and made['error']['code'] == 'OUT_OF_SCOPE',
                      made.get('error', {}).get('code', made['status']))

        made = tool(project, 'create_proposal', {
            'pattern': pattern, 'channel': channel,
            'base_revision': state['revision'],
            'notes': [{'what': 'change', 'id': note['id'], 'pitch': 85}]})
        report.expect('and so is no list at all',
                      made['status'] == 'error' and made['error']['code'] == 'OUT_OF_SCOPE',
                      made.get('error', {}).get('code', made['status']))
    finally:
        session.close()


# --- R3 -------------------------------------------------------------------------------

def check_an_added_note_fits_the_pattern_it_is_added_to(exe, folder, report):
    """A half-beat pattern, an add with no length. Whatever the default is, the note
    that is checked has to be the note that is written."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / 'project.json'
    session = Session(exe, folder).open()
    try:
        pattern = prepare_song(session)
        state = session.settled()
        channel = state['channels'][0]['id']

        def shorten(data):
            p = next(p for p in data['patterns'] if p['id'] == pattern)
            p['length'] = 0.5
            p['sequences'] = []

        apply_change(project, shorten)
        session.run([{'note': [60, 0.0, 0.25, 100]}])
        state = session.settled()
        existing = [n['id'] for n in notes_of(project, pattern, channel)]

        made = tool(project, 'create_proposal', {
            'pattern': pattern, 'channel': channel, 'allowed_notes': existing,
            'base_revision': state['revision'],
            'notes': [{'what': 'add', 'pitch': 60}]})

        if made['status'] == 'error':
            report.expect('an add that cannot fit is refused rather than misdescribed',
                          made['error']['code'] in ('OUT_OF_SCOPE', 'INVALID_ARGUMENT'),
                          made['error']['code'])
            return

        added = [n for n in made['result']['diff']['notes'] if n['what'] == 'add']
        report.expect('the diff says what length the note will have',
                      added and 'length_beats' in added[0], added)
        report.expect('and that length fits inside the pattern',
                      added and added[0]['length_beats'] <= 0.5 + 1e-9,
                      added[0].get('length_beats') if added else None)

        applied = tool(project, 'apply_proposal', {'proposal': made['result']['proposal']['id']})
        report.expect('applying it works', applied['status'] == 'ok', applied.get('error'))

        longest = max(n['length_beats'] for n in notes_of(project, pattern, channel))
        report.expect('no note in the pattern is longer than the pattern',
                      longest <= 0.5 + 1e-9, longest)
        report.expect('and what was written is what the diff said',
                      any(abs(n['length_beats'] - added[0]['length_beats']) < 1e-9
                          for n in notes_of(project, pattern, channel)))
    finally:
        session.close()


def check_an_unknown_verb_is_not_a_change(exe, folder, report):
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / 'project.json'
    session = Session(exe, folder).open()
    try:
        pattern = prepare_song(session)
        state = session.settled()
        channel = state['channels'][0]['id']
        note = notes_of(project, pattern, channel)[0]

        made = tool(project, 'create_proposal', {
            'pattern': pattern, 'channel': channel, 'allowed_notes': [note['id']],
            'base_revision': state['revision'],
            'notes': [{'what': 'move', 'id': note['id'], 'pitch': 85}]})
        report.expect('a verb the service does not know is refused, not read as "change"',
                      made['status'] == 'error' and made['error']['code'] == 'INVALID_ARGUMENT',
                      made.get('error', {}).get('code', made['status']))
    finally:
        session.close()


def check_numbers_have_to_be_numbers(exe, folder, report):
    """static_cast on a var turned "abc" into 0 and 60.7 into 60 - both inside the MIDI
    range, neither what anybody sent. A value that was not understood is refused."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / 'project.json'
    session = Session(exe, folder).open()
    try:
        pattern = prepare_song(session)
        state = session.settled()
        channel = state['channels'][0]['id']
        note = notes_of(project, pattern, channel)[0]

        for label, bad in (('a string pitch', 'abc'), ('a fractional pitch', 60.7),
                           ('a boolean pitch', True), ('a null pitch', None)):
            made = tool(project, 'create_proposal', {
                'pattern': pattern, 'channel': channel, 'allowed_notes': [note['id']],
                'base_revision': state['revision'],
                'notes': [{'what': 'change', 'id': note['id'], 'pitch': bad}]})
            report.expect(label + ' is refused, not rounded into range',
                          made['status'] == 'error' and made['error']['code'] == 'INVALID_ARGUMENT',
                          made.get('error', {}).get('code', made['status']))

        made = tool(project, 'create_proposal', {
            'pattern': pattern, 'channel': channel, 'allowed_notes': [note['id']],
            'base_revision': state['revision'],
            'notes': [{'what': 'change', 'id': note['id'], 'pitch': 62.0}]})
        report.expect('a whole number written as a float is still a whole number',
                      made['status'] == 'ok', made.get('error'))
        report.expect('and none of that touched the music',
                      read(folder / 'sync-status.json')['revision'] == state['revision'])
    finally:
        session.close()


def check_a_reply_for_another_project_is_ignored(exe, folder, report):
    """A reply that names a project is held to it. The request id already makes this
    nearly impossible by accident; this makes it a rule rather than a probability."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / 'project.json'
    session = Session(exe, folder).open()
    try:
        pattern = prepare_song(session)
        state = session.settled()
        channel = state['channels'][0]['id']
        note = notes_of(project, pattern, channel)[0]

        def reply_from_elsewhere(request):
            atomic_write(folder / 'chat-reply.json',
                         {'request_id': request['request_id'], 'status': 'ok',
                          'project_id': 'some-other-project', 'text': 'wrong song',
                          'provider': 'fixture',
                          'change': {'notes': [{'id': note['id'], 'pitch': 85}]}})
            time.sleep(2.0)
            return read(folder / 'conversation.json')['messages'][-1]

        answer = with_fixture_bridge(session, folder, 'notes', reply_from_elsewhere)
        report.expect("a reply addressed to another project is not taken as this one's",
                      answer.get('streaming') is True or not answer.get('text'),
                      {k: answer.get(k) for k in ('streaming', 'text', 'proposal_id')})
        report.expect('and nothing was offered', not answer.get('proposal_id'))
        session.run([{'chat': 'cancel'}])
    finally:
        session.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print('R1: a late reply and a person who changed their mind')
    check_a_late_reply_cannot_overwrite_what_the_person_changed(exe, output / 'stale', report)
    print()
    print('R2: a reply that names its own scope')
    check_a_reply_cannot_name_its_own_scope(exe, output / 'insert-only', report)
    print()
    print('R2: a reply with nothing attached')
    check_a_reply_with_nothing_attached_changes_nothing(exe, output / 'nothing', report)
    print()
    print('a reply for another project')
    check_a_reply_for_another_project_is_ignored(exe, output / 'elsewhere', report)
    print()
    print('R2: the service reads empty as none')
    check_the_service_itself_reads_empty_as_none(exe, output / 'empty', report)
    print()
    print('R3: an added note fits its pattern')
    check_an_added_note_fits_the_pattern_it_is_added_to(exe, output / 'add', report)
    print()
    print('R3: numbers have to be numbers')
    check_numbers_have_to_be_numbers(exe, output / 'numbers', report)
    print()
    print('R3: an unknown verb')
    check_an_unknown_verb_is_not_a_change(exe, output / 'verb', report)

    print()
    print('FAILURES:', report.failures if report.failures else 'none')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--exe', type=Path,
                        default=ROOT / 'build-cocompose/CoCompose_artefacts/Release/CoCompose.exe')
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build-cocompose' / ('boundaries-' + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
