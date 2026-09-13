"""Reproduce review findings against a real, isolated CoCompose project. No API calls."""
import json
import time
import uuid
from pathlib import Path
from cocompose import atomic_write, read, tool, wait_for, apply_change
from test_plugin_compatibility import Session, prepare_song
from cocompose_bridge import Liveness

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build-cocompose/review-bin/CoCompose.exe'
OUT = ROOT / 'build-cocompose' / ('proposal-review-' + uuid.uuid4().hex[:8])

def run_case(kind):
    folder = OUT / kind
    folder.mkdir(parents=True)
    s = Session(EXE, folder).open()
    project = folder / 'project.json'
    try:
        pattern = prepare_song(s)
        state = s.settled()
        channel = state['channels'][0]['id']
        def inspect():
            return tool(project, 'inspect_pattern', {'pattern': pattern, 'channel': channel})['result']['parts'][0]['notes']
        note = inspect()[0]
        if kind == 'add_default':
            def shorten(data):
                p = next(p for p in data['patterns'] if p['id'] == pattern)
                p['length'] = 0.5
                p['sequences'] = []
            apply_change(project, shorten)
            s.run([{'note': [60, 0.0, 0.25, 100]}])
            made = tool(project, 'create_proposal', {'pattern': pattern, 'channel': channel,
                'notes': [{'what': 'add', 'pitch': 60}], 'base_revision': s.settled()['revision']})
        else:
            attachment = 'notes' if kind == 'stale_reply' else 'insert:' + state['mixer']['inserts'][0]['id']
            s.run([{'attach': attachment}])
            with Liveness(folder / 'chat-bridge.json', 'review synthetic reply'):
                wait_for(lambda: read(folder / 'chat-inspector.json').get('bridge_connected'), timeout=20)
                s.run([{'chat': 'ask: review test change'}])
                request = read(folder / 'chat-request.json')
                if kind == 'stale_reply':
                    s.run([{'command': 'Transpose pattern up'}])
                revision = s.settled()['revision']
                atomic_write(folder / 'chat-reply.json', {'request_id': request['request_id'],
                    'status': 'ok', 'text': 'Synthetic review reply', 'provider': 'review fixture',
                    'change': {'pattern': pattern, 'channel': channel, 'base_revision': request['revision'],
                               'notes': [{'id': note['id'], 'pitch': 85}]}})
                wait_for(lambda: read(folder / 'conversation.json')['messages'][-1].get('proposal_id')
                         or read(folder / 'conversation.json')['messages'][-1].get('proposal_problem'), timeout=20)
                answer = read(folder / 'conversation.json')['messages'][-1]
                made = {'request_revision': request['revision'], 'current_revision': revision, 'answer': answer}
        proposal_id = (made.get('result', {}).get('proposal', {}).get('id') if kind == 'add_default'
                       else made['answer'].get('proposal_id'))
        if proposal_id:
            made['apply'] = tool(project, 'apply_proposal', {'proposal': proposal_id})
            made['model_readback'] = inspect()
        return made
    finally:
        s.close()

if __name__ == '__main__':
    results = {}
    for case in ['stale_reply', 'insert_scope', 'add_default']:
        try:
            results[case] = run_case(case)
        except Exception as e:
            results[case] = {'test_error': str(e)}
        print(case, json.dumps(results[case], ensure_ascii=False), flush=True)
    atomic_write(OUT / 'report.json', results)
    print(OUT, flush=True)
