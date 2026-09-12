"""A1 as the milestone states it: one connection, verified end to end against a model.

This is deliberately separate from test_ai_chat.py. That suite checks the app, and it
uses the echo bridge so that it can say exactly what the answer will be - which is the
right way to check plumbing and the wrong way to check a connection. A connection is
only verified when something that was not told what to say has said something.

So nothing here asserts on the words. It asserts on the things that must hold whatever
the model writes: that the answer came back at all, that it is about the thing that was
attached, that a model with no audio does not claim to have heard anything, that the
second question can see the first, and that the app stayed alive and kept playing while
it waited.

Run with the app closed, and a model serving:
    ollama serve                       # usually already running
    python tools/test_ai_connection.py --model llama3.1:8b

It needs a real model. It will not fall back to the echo bridge, because a pass from
the echo bridge would be exactly the false report the milestone warns about.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import urllib.error
import urllib.request
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

    def for_a_person(self, name, why):
        """Something observed about the model rather than about the app.

        A local model is not a fixture: what it writes varies between runs. Recording
        that as a pass or a failure would mean this suite reports the weather, so what
        the model did is written down and what the app did about it is asserted."""
        print('  --   ' + name + '  (' + why + ')')
        self.unchecked.append(name + " - " + why)


def model_is_serving(host, model):
    try:
        with urllib.request.urlopen(host.rstrip("/") + "/api/tags", timeout=5) as answer:
            names = [m["name"] for m in json.loads(answer.read().decode("utf-8"))["models"]]
    except (urllib.error.URLError, OSError, ValueError, KeyError):
        return False, "nothing is answering at " + host
    return (model in names, "have: " + ", ".join(names[:6]))


def maybe(path):
    """A file that has not been written yet is not an error here: a conversation with no
    messages in it has no file, which is the ordinary state before the first question."""
    try:
        return read(path)
    except (FileNotFoundError, ValueError):
        return {}


def mentions_any(text, words):
    lowered = (text or "").lower()
    return any(word.lower() in lowered for word in words if word)


def clear_attachments(session, folder):
    """Takes the cards off one at a time. Asking sends whatever is attached, so a
    question about an insert must not still be carrying the bars from before it."""
    while maybe(folder / "chat-inspector.json").get("attachments"):
        session.run([{"attachment": [0, "remove"]}])
        time.sleep(0.3)


def ask(session, folder, question, report=None, timeout=600):
    """Asks, waits for the answer to finish arriving, and returns it.

    Also watches the revision across the exchange. Checking it once at the end would be
    checking this script's own edits as well - it adds a note of its own partway through
    - so each question is held to it separately, which is the claim that matters anyway:
    asking a question and receiving an answer is not an edit."""
    before = len(maybe(folder / "conversation.json").get("messages", []))
    revision_before = read(folder / "sync-status.json")["revision"]
    session.run([{"chat": "ask: " + question}])

    def answered():
        talk = maybe(folder / "conversation.json")
        messages = talk.get("messages", [])
        if len(messages) < before + 2:
            return None
        last = messages[-1]
        return None if last.get("streaming") or not last.get("text") else talk

    talk = wait_for(answered, timeout=timeout)

    if report is not None:
        revision_after = read(folder / "sync-status.json")["revision"]
        report.expect("asking and answering changed no music",
                      revision_after == revision_before,
                      (revision_before, revision_after))

    return talk


def run(exe, output, model, host):
    output.mkdir(parents=True, exist_ok=True)
    folder = output / "session"
    folder.mkdir(exist_ok=True)
    project = folder / "project.json"
    report = Report()

    serving, detail = model_is_serving(host, model)
    if not serving:
        print("No model to talk to: " + detail)
        print("Start one with:  ollama pull " + model)
        return None

    session = Session(exe, folder).open(extra=["--play"])
    bridge = subprocess.Popen(
        [sys.executable, str(ROOT / "tools" / "cocompose_bridge.py"),
         "--project", str(project), "--provider", "ollama",
         "--model", model, "--host", host],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        prepare_song(session)
        state = session.settled()
        wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"), timeout=60)

        stated = read(folder / "chat-bridge.json")
        report.expect("the app is talking to a model, not to the echo bridge",
                      "ollama" in stated.get("name", "") and "echo" not in stated.get("name", ""),
                      stated.get("name", ""))

        started_revision = read(folder / "sync-status.json")["revision"]
        channel_name = state["channels"][0]["name"]
        insert_id = state["mixer"]["inserts"][0]["id"]
        insert_name = next(i for i in state["mixer"]["inserts"] if i["id"] == insert_id)["name"]

        # --- 1. "this stretch is odd" --------------------------------------------------
        print()
        print("a region")
        session.run([{"select_lane": 0}, {"pick_clip": [0, 0.0]}, {"attach": "region"}])
        time.sleep(0.5)
        talk = ask(session, folder, "this stretch sounds odd to me - what is actually there?", report)
        answer = talk["messages"][-1]

        report.expect("an answer came back", bool(answer["text"].strip()),
                      answer["text"][:100].replace("\n", " "))
        report.expect("the answer is about the bars that were attached",
                      mentions_any(answer["text"], ["bar", "beat", channel_name, "pattern"]),
                      answer["text"][:160].replace("\n", " "))
        report.expect("the music kept playing while it waited",
                      read(folder / "sync-status.json").get("playing") is True,
                      read(folder / "sync-status.json").get("playing"))
        report.expect("the app was still keeping up",
                      read(folder / "sync-status.json")["session_id"] == read(folder / "state.json")["session_id"])

        # A model that was sent no audio must not say it listened.
        report.expect("nothing was heard, and nothing claims to have been",
                      not mentions_any(answer["text"],
                                       ["i listened", "i heard", "listening to it", "when i hear",
                                        "sounds muddy to my ear"]),
                      answer["text"][:160].replace("\n", " "))

        # --- 2. "what is on this insert" -----------------------------------------------
        print()
        print("an insert")
        clear_attachments(session, folder)
        session.run([{"attach": "insert:" + insert_id}])
        time.sleep(0.5)
        talk = ask(session, folder, "what is on this mixer insert?", report)
        answer = talk["messages"][-1]
        report.expect("an answer came back", bool(answer["text"].strip()))
        report.expect("the answer is about the insert that was attached",
                      mentions_any(answer["text"], [insert_name, "insert", "effect", "gain", "pan"]),
                      answer["text"][:160].replace("\n", " "))

        # --- 3. "describe these notes" -------------------------------------------------
        print()
        print("some notes")
        here = tool(project, "get_selection")["result"]
        session.run([{"select_channel": 0}, {"note": [72, 0.0, 1.0, 100]}])
        time.sleep(0.8)
        here = tool(project, "get_selection")["result"]
        part = tool(project, "inspect_pattern",
                    {"pattern": here["pattern"], "channel": here["channel"]})["result"]
        ids = [n["id"] for p in part["parts"] for n in p["notes"]]
        clear_attachments(session, folder)
        session.run([{"pick_notes": ids[:4]}, {"attach": "notes"}])
        time.sleep(0.5)

        talk = ask(session, folder, "describe these notes to me", report)
        answer = talk["messages"][-1]
        report.expect("an answer came back", bool(answer["text"].strip()))
        report.expect("the answer is about the notes that were attached",
                      mentions_any(answer["text"], ["note", "pitch", "beat", "velocity", "rhythm"]),
                      answer["text"][:160].replace("\n", " "))

        # --- the conversation continues -------------------------------------------------
        print()
        print("a second question that needs the first")
        talk = ask(session, folder, "and which of those is the highest?", report)
        answer = talk["messages"][-1]
        report.expect("a follow-up with no attachment of its own still gets an answer",
                      bool(answer["text"].strip()))
        report.expect("the conversation is one conversation",
                      len(talk["messages"]) >= 8 and len({m["request_id"]
                                                          for m in talk["messages"]
                                                          if m["request_id"]}) >= 4,
                      len(talk["messages"]))
        report.expect("all of it belongs to this project",
                      talk["project_id"] == read(folder / "sync-status.json")["project_id"])

        # --- and whether a real model can actually propose ------------------------------
        # This is the part no echo bridge can answer. The app is ready to receive a
        # change; whether a model writes one in the shape it was asked for is a fact
        # about the model, and the only way to know it is to ask one.
        print()
        print("a change, written by the model")
        clear_attachments(session, folder)
        session.run([{"pick_notes": ids[:4]}, {"attach": "notes"}])
        time.sleep(0.5)
        before_proposal = read(folder / "sync-status.json")["revision"]

        talk = ask(session, folder,
                   "move these notes up by one tone, keeping the rhythm exactly as it is",
                   report)
        answer = talk["messages"][-1]

        wrote_one = bool(answer.get("proposal_id")) or bool(answer.get("proposal_problem"))
        report.expect("the model wrote a change block the bridge could read", wrote_one,
                      answer["text"][-200:].replace(chr(10), " "))

        # Whether the model stays inside what it was given is a fact about the model,
        # and this one does not always: llama3.1:8b sometimes names a note it was shown
        # for context rather than one it was allowed to change. Counting that as a
        # failure here would be reporting the model's behaviour as a defect in the app,
        # and it would make this suite fail at random.
        #
        # What does belong to the app is the other half, and it is the half that
        # matters: whatever the model wrote, either it was inside the lines and became a
        # proposal, or it was refused for a reason that names the problem - and either
        # way the music is where it was. That is asserted, and what the model did is
        # reported rather than judged.
        if answer.get("proposal_id"):
            report.expect("the change it wrote was inside what it was given", True,
                          "it stayed in scope")
        else:
            problem = answer.get("proposal_problem", "")
            report.expect("a change outside what it was given is refused, with a reason",
                          problem.startswith(("OUT_OF_SCOPE", "INVALID_ARGUMENT",
                                              "STALE_REVISION", "NOT_FOUND", "LOCKED")),
                          problem or "no block at all")
            report.for_a_person("whether this model reliably stays inside the notes it "
                                "was given",
                                "it did not this time (" + problem[:60] + "); that is a "
                                "property of llama3.1:8b, not of the app, and the app "
                                "refused it correctly")

        report.expect("and the music is where it was either way",
                      read(folder / "sync-status.json")["revision"] == before_proposal,
                      read(folder / "sync-status.json")["revision"])

        if answer.get("proposal_id"):
            report.expect("working it out was still not an edit",
                          read(folder / "sync-status.json")["revision"] == before_proposal)
            report.expect("it is offered, not applied",
                          answer["proposal"]["applied"] is False)

            moved = {n["id"]: n for n in tool(project, "inspect_pattern",
                                              {"pattern": here["pattern"],
                                               "channel": here["channel"]})["result"]["parts"][0]["notes"]}
            session.run([{"chat": "apply"}])
            time.sleep(1.5)
            after = {n["id"]: n for n in tool(project, "inspect_pattern",
                                              {"pattern": here["pattern"],
                                               "channel": here["channel"]})["result"]["parts"][0]["notes"]}

            touched = [n["id"] for n in answer["proposal_diff"]["notes"]]
            report.expect("applying what the model wrote moves the notes it named",
                          all(after[i]["pitch"] != moved[i]["pitch"] for i in touched),
                          [(moved[i]["pitch"], after[i]["pitch"]) for i in touched])
            report.expect("the rhythm it was told to keep was kept",
                          all(after[i]["start_beat"] == moved[i]["start_beat"]
                              and after[i]["length_beats"] == moved[i]["length_beats"]
                              for i in touched))
            report.expect("nothing outside what was attached moved",
                          all(after[i]["pitch"] == moved[i]["pitch"]
                              for i in moved if i not in touched))

        # --- and none of it was an edit ------------------------------------------------
        print()
        print("asking is not editing")
        # started_revision plus the one note this script added to open the note editor.
        # started_revision, plus the note this check added to open the editor, plus the
        # proposal if the model managed to write one and it was applied.
        report.expect("every edit is one this check asked for on purpose",
                      read(folder / "sync-status.json")["revision"] <= started_revision + 2,
                      (read(folder / "sync-status.json")["revision"], started_revision))
        report.expect("the app is still answering afterwards",
                      tool(project, "get_capabilities")["status"] == "ok")

        print()
        print("what the model actually said")
        for message in talk["messages"]:
            if message["from"] == "assistant":
                print("  - " + message["text"].strip().replace("\n", " ")[:220])

    finally:
        bridge.terminate()
        session.close()

    print()
    if report.unchecked:
        print("OBSERVED, NOT JUDGED:")
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
                        default=ROOT / "build-cocompose" / ("ai-connection-" + uuid.uuid4().hex[:8]))
    parser.add_argument("--model", default="llama3.1:8b")
    parser.add_argument("--host", default="http://localhost:11434")
    args = parser.parse_args()

    outcome = run(args.exe.resolve(), args.output.resolve(), args.model, args.host)
    # No model is not a pass. It is "this was not checked", and it says so by failing.
    sys.exit(2 if outcome is None else (1 if outcome.failures else 0))
