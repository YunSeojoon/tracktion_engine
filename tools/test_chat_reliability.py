"""C1-C4 from the third review: the ways a chat connection lies about itself.

Each of these is a thing that looked fine and was not. A bridge that re-asked a question
nobody asked twice. A bridge that had been dead for an hour and still said it was ready.
A conversation that was saved in full and sent in part. A region described so thinly that
two completely different pieces of music read identically.

None of them need a key or a network: three use a provider spy - something with the same
shape as a real provider that records what it was handed - and the fourth reads what the
app itself writes. A check that needs a paid API to run is a check that does not run.

    python tools/test_chat_reliability.py
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
from cocompose_bridge import build_prompt, describe_attachments, serve
from test_plugin_compatibility import Session, prepare_song

ROOT = Path(__file__).resolve().parents[1]


class Report:
    def __init__(self):
        self.failures = []

    def expect(self, name, condition, detail=''):
        print(('  ok   ' if condition else '  FAIL ') + name + (('  ' + str(detail)) if detail else ''))
        if not condition:
            self.failures.append(name)


class Spy:
    """A provider that answers instantly and remembers every question it was given."""

    name = "spy (a provider that only counts)"

    def __init__(self, reply="I looked at it."):
        self.asked = []
        self.reply = reply

    def answer(self, request, on_text):
        self.asked.append(request)
        on_text(self.reply)
        return self.reply


def write_request(folder, request_id, message="what is this?"):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "chat-request.json").write_text(json.dumps({
        "schema": 1, "request_id": request_id, "project_id": "p", "conversation_id": "c",
        "revision": 1, "message": message, "attachments": [], "history": {"messages": []},
        "asked_at_ms": int(time.time() * 1000)}), encoding="utf-8")


# --- C1 -----------------------------------------------------------------------------

def check_a_restart_does_not_ask_again(folder, report):
    """C1: the bridge's memory of what it answered used to die with the process."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    write_request(folder, "question-1")

    spy = Spy()
    serve(project, spy, once=True)
    report.expect("the question reached the provider once", len(spy.asked) == 1, len(spy.asked))

    first_reply = read(folder / "chat-reply.json")
    report.expect("and was answered", first_reply["status"] == "ok")

    # The bridge is restarted in the same folder, with the same question still sitting
    # there - which is exactly what happens when someone closes the window and reopens it.
    serve(project, spy, once=True)
    report.expect("restarting does not ask the same question again",
                  len(spy.asked) == 1, len(spy.asked))
    report.expect("and does not replace the answer that was already given",
                  read(folder / "chat-reply.json") == first_reply)

    # A genuinely new question still gets through.
    write_request(folder, "question-2")
    serve(project, spy, once=True)
    report.expect("a new question is asked once", len(spy.asked) == 2, len(spy.asked))


def check_an_interrupted_answer_is_not_resent(folder, report):
    """C1, the harder half: an answer that was cut off mid-sentence.

    Whether the provider finished the work - and billed for it - cannot be known from
    here. Asking again could pay twice and, once writing is involved, edit twice. So it
    is not asked again quietly: the person is told, and asking again is their decision."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    write_request(folder, "cut-off")

    # What a bridge leaves behind when it dies mid-answer.
    (folder / "chat-reply.json").write_text(json.dumps({
        "request_id": "cut-off", "status": "streaming", "text": "I was about to say",
        "provider": "spy"}), encoding="utf-8")

    spy = Spy()
    serve(project, spy, once=True)

    report.expect("an interrupted answer is not quietly asked again", not spy.asked, spy.asked)

    reply = read(folder / "chat-reply.json")
    report.expect("the person is told it was interrupted", reply["status"] == "error",
                  reply.get("status"))
    report.expect("and told that nothing was changed",
                  "nothing was changed" in reply["message"].lower(), reply.get("message"))
    report.expect("and that asking again is up to them", reply["retryable"] is True)


# --- C2 -----------------------------------------------------------------------------

def check_a_dead_bridge_stops_looking_alive(exe, folder, report):
    """C2: ready was written once at startup, and a killed process never unwrites it."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"

    session = Session(exe, folder).open()
    bridge = None
    try:
        prepare_song(session)
        session.settled()

        bridge = subprocess.Popen(
            [sys.executable, str(ROOT / "tools" / "cocompose_bridge.py"),
             "--project", str(project), "--provider", "echo"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"), timeout=30)
        stated = read(folder / "chat-bridge.json")
        report.expect("a live bridge says when it last spoke",
                      stated.get("heartbeat_ms", 0) > 0 and stated.get("instance"),
                      {k: stated.get(k) for k in ("instance", "heartbeat_interval_ms")})

        beat = stated["heartbeat_ms"]
        time.sleep(3.0)
        report.expect("and keeps saying it",
                      read(folder / "chat-bridge.json")["heartbeat_ms"] > beat)

        # Killed, not asked to stop: no chance to tidy up after itself.
        bridge.kill()
        bridge.wait(timeout=10)
        bridge = None

        leftover = read(folder / "chat-bridge.json")
        report.expect("the file it leaves behind still claims to be ready",
                      leftover.get("ready") is True, leftover.get("ready"))
        def stopped_believing():
            # wait_for returns the first truthy value, so the condition has to be the
            # truthy one: "it has noticed", not "connected is False".
            return read(folder / "chat-inspector.json").get("bridge_connected") is False

        report.expect("but the app stops believing it",
                      wait_for(stopped_believing, timeout=40) is True)
    finally:
        if bridge is not None:
            bridge.kill()
        session.close()


def check_a_question_into_the_void_comes_back(exe, folder, report):
    """C2, what it costs a person: a question sent to a bridge that is no longer there
    used to take their words away and wait forever."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"

    session = Session(exe, folder).open()
    bridge = None
    try:
        prepare_song(session)
        session.settled()
        before_revision = read(folder / "sync-status.json")["revision"]

        bridge = subprocess.Popen(
            [sys.executable, str(ROOT / "tools" / "cocompose_bridge.py"),
             "--project", str(project), "--provider", "echo"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"), timeout=30)

        session.run([{"select_channel": 0}, {"attach": "notes"}])
        time.sleep(0.4)

        # Gone between the attaching and the asking.
        bridge.kill()
        bridge.wait(timeout=10)
        bridge = None
        (folder / "chat-reply.json").unlink(missing_ok=True)

        question = "does this bridge exist"
        session.run([{"chat": "ask: " + question}])

        def given_back():
            packet = read(folder / "chat-inspector.json")
            return packet if question in (packet.get("draft") or "") else None

        report.expect("the question comes back to the person rather than waiting forever",
                      wait_for(given_back, timeout=60) is not None)
        report.expect("and nothing is still waiting",
                      read(folder / "chat-inspector.json").get("waiting") is False)
        report.expect("and no music changed while all that happened",
                      read(folder / "sync-status.json")["revision"] == before_revision)
    finally:
        if bridge is not None:
            bridge.kill()
        session.close()


# --- C3 -----------------------------------------------------------------------------

def check_an_earlier_attachment_is_still_there(report):
    """C3: the conversation was saved in full and sent in part.

    "Make that part less busy" is a question about whatever was attached earlier. A
    history carrying only the words left the model reading a pronoun with no referent."""
    request = {
        "revision": 12,
        "message": "now make that part less busy",
        "attachments": [],
        "history": {"messages": [
            {"from": "person", "text": "what is going on in this bit?",
             "at_revision": 9,
             "attachments": [{"id": "a1", "kind": "notes", "taken_at_revision": 9,
                              "pattern": "phrase-1", "channel": "chan-1", "note_count": 7,
                              "start_beat": 0.0, "end_beat": 16.0}]},
            {"from": "assistant", "text": "It is a busy sixteenth line.", "attachments": []},
            {"from": "person", "text": "now make that part less busy", "attachments": []}]}}

    prompt = build_prompt(request)
    report.expect("what the earlier question was about is carried forward",
                  "phrase-1" in prompt and "chan-1" in prompt)
    report.expect("including how much of it there was", "7 note(s)" in prompt)
    report.expect("and that the music has moved since it was read",
                  "revision 9" in prompt and "now at 12" in prompt,
                  [line for line in prompt.splitlines() if "revision" in line])

    # A condition is usually at the end of what someone wrote, which is what silent
    # truncation takes away.
    request["history"]["messages"][0]["text_was_cut"] = True
    request["history"]["trimmed"] = True
    request["history"]["any_text_cut"] = True
    prompt = build_prompt(request)
    report.expect("a message that was cut short says so", "[...cut]" in prompt)
    report.expect("and the model is told to ask rather than assume",
                  "ask rather than assume" in prompt)
    report.expect("older messages that were dropped are admitted to",
                  "not shown" in prompt)


# --- C4 -----------------------------------------------------------------------------

def check_two_different_pieces_of_music_read_differently(exe, folder, report):
    """C4: a region used to be described by clip names and lengths alone, so two clips
    with the same name and length and completely different notes were indistinguishable
    to anything being asked what was wrong with them."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"

    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()

        looked = tool(project, "inspect_region", {"start_beat": 0.0, "end_beat": 16.0})["result"]
        report.expect("a region carries the notes that are played in it",
                      any(part.get("notes") for clip in looked["clips"]
                          for part in clip.get("parts", [])),
                      [c.get("pattern_name") for c in looked["clips"]])

        one = describe_attachments([{"kind": "region", "summary": "bars 1-4",
                                     "still_there": True, "detail": {"result": looked}}])

        # The same clip, the same name, the same length - different music.
        pattern_id = looked["clips"][0]["pattern"]
        channel_id = state["channels"][0]["id"]

        def transpose(live):
            pattern = next(p for p in live["patterns"] if p["id"] == pattern_id)
            for sequence in pattern["sequences"]:
                for note in sequence["notes"]:
                    note["pitch"] = min(127, note["pitch"] + 7)

        apply_change(project, transpose)
        time.sleep(1.0)

        after = tool(project, "inspect_region", {"start_beat": 0.0, "end_beat": 16.0})["result"]
        two = describe_attachments([{"kind": "region", "summary": "bars 1-4",
                                     "still_there": True, "detail": {"result": after}}])

        report.expect("the same clip with different notes reads differently", one != two)
        report.expect("and the difference is the music, not the labels",
                      [c["pattern_name"] for c in looked["clips"]]
                      == [c["pattern_name"] for c in after["clips"]])

        # Everything left out is counted rather than dropped in silence.
        report.expect("a region says whether it left anything out",
                      "notes_left_out" in looked and "notes_budget" in looked)

        insert_id = state["mixer"]["inserts"][0]["id"]
        insert = tool(project, "inspect_insert", {"insert": insert_id})["result"]
        spoken = describe_attachments([{"kind": "insert", "summary": "insert 1",
                                        "still_there": True, "detail": {"result": insert}}])
        report.expect("an insert says where it sends a copy of itself, or that it does not",
                      "send" in spoken.lower(), spoken[:200])
    finally:
        session.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("C1: a restart must not re-ask")
    check_a_restart_does_not_ask_again(output / "restart", report)
    print()
    print("C1: an answer that was cut off")
    check_an_interrupted_answer_is_not_resent(output / "interrupted", report)
    print()
    print("C2: a bridge that has died")
    check_a_dead_bridge_stops_looking_alive(exe, output / "dead", report)
    print()
    print("C2: a question sent into the void")
    check_a_question_into_the_void_comes_back(exe, output / "void", report)
    print()
    print("C3: what an earlier question was about")
    check_an_earlier_attachment_is_still_there(report)
    print()
    print("C4: a region that reads as music")
    check_two_different_pieces_of_music_read_differently(exe, output / "region", report)

    print()
    print("FAILURES:", report.failures if report.failures else "none")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path,
                        default=ROOT / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build-cocompose" / ("reliability-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
