"""B4: changing the model, without changing what was already said.

A person can stop one bridge and start another pointed at a different model half way
through a conversation. Everything that was true before the switch is still true: the
same project, the same conversation, the same decisions written down, the same
alternatives on the shelf. The one thing that must not carry over is authorship - an
answer written before the switch was written by the model that was asked, and showing
it as the new one's is a lie that cannot be spotted afterwards.

No model is called here. Two fixture bridges stand in for two models, because what is
being checked is the app's bookkeeping across a handover and not anybody's musicality.

    python tools/test_model_handover.py
"""
import argparse
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import atomic_write, read, tool, wait_for
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


def messages(folder):
    return read(folder / "conversation.json")["messages"]


def ask_and_answer(session, folder, text, provider, model, previous):
    """Asks, then answers as `provider`/`model` would. Returns the request id."""
    session.run([{"attach": "notes"}])
    session.run([{"chat": "ask: " + text}])

    request = wait_for(lambda: (read(folder / "chat-request.json")
                                if read(folder / "chat-request.json").get("request_id") != previous
                                else None),
                       timeout=20, what="a question other than " + str(previous))

    atomic_write(folder / "chat-reply.json",
                 {"request_id": request["request_id"], "status": "ok",
                  "text": "answered by " + model, "provider": provider, "model": model})

    wait_for(lambda: next((m for m in messages(folder)
                           if m.get("request_id") == request["request_id"]
                           and m.get("from") == "assistant"
                           and not m.get("streaming")), None),
             timeout=20, what="the answer to be taken")

    return request["request_id"]


def check_a_handover_keeps_the_work_and_not_the_authorship(exe, folder, report):
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        session.settled()

        # Something decided before the switch, which has to survive it.
        session.run([{"project_note": ["condition", "the drums stay as they are"]}])
        time.sleep(0.5)

        conversation_before = read(folder / "chat-inspector.json").get("conversation_id")
        report.expect("there is a conversation to carry over", bool(conversation_before))

        with Liveness(folder / "chat-bridge.json", "first model",
                      provider="fixture", model="model-one",
                      capabilities={"suggests_changes": False, "hears_audio": False,
                                    "runs_locally": True, "is_a_model": True}):
            wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"),
                     timeout=20, what="the app to see the first bridge")

            seen = read(folder / "chat-inspector.json").get("bridge") or {}
            report.expect("the app can say which model is connected",
                          seen.get("model") == "model-one", seen.get("model"))
            report.expect("and what that connection can do",
                          (seen.get("capabilities") or {}).get("hears_audio") is False,
                          seen.get("capabilities"))

            first = ask_and_answer(session, folder, "one", "fixture", "model-one", None)

        # The bridge goes away and a different one starts, pointed somewhere else.
        wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected") is False,
                 timeout=30, what="the app to notice the first bridge left")

        with Liveness(folder / "chat-bridge.json", "second model",
                      provider="fixture", model="model-two",
                      capabilities={"suggests_changes": True, "hears_audio": False,
                                    "runs_locally": False, "is_a_model": True}):
            wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"),
                     timeout=20, what="the app to see the second bridge")

            now = read(folder / "chat-inspector.json").get("bridge") or {}
            report.expect("the app says the model changed",
                          now.get("model") == "model-two", now.get("model"))
            report.expect("and that what it can do changed with it",
                          (now.get("capabilities") or {}).get("runs_locally") is False,
                          now.get("capabilities"))

            ask_and_answer(session, folder, "two", "fixture", "model-two", first)

        # The bookkeeping, after the handover.
        said = messages(folder)
        answers = [m for m in said if m["from"] == "assistant"]
        report.expect("both answers are in the conversation", len(answers) == 2,
                      [a.get("text") for a in answers])
        if len(answers) != 2:
            return

        report.expect("the first answer is still the first model's",
                      answers[0]["answered_by"]["model"] == "model-one",
                      answers[0]["answered_by"])
        report.expect("and the second is the second model's",
                      answers[1]["answered_by"]["model"] == "model-two",
                      answers[1]["answered_by"])
        report.expect("changing the model did not relabel what was already said",
                      answers[0]["answered_by"]["model"] != answers[1]["answered_by"]["model"])

        report.expect("the conversation is the same conversation",
                      read(folder / "chat-inspector.json").get("conversation_id")
                      == conversation_before,
                      (conversation_before,
                       read(folder / "chat-inspector.json").get("conversation_id")))

        kept = [n for n in (read(folder / "chat-inspector.json").get("notes") or {}).get("notes", [])
                if n["kind"] == "condition"]
        report.expect("what the person decided survived the handover",
                      any("drums" in n["text"] for n in kept), [n["text"] for n in kept])

        # And it all comes back after a restart, still attributed.
        session.close()
        session = Session(exe, folder).open()
        session.settled()

        reopened = [m for m in messages(folder) if m["from"] == "assistant"]
        report.expect("the attribution survives closing the app",
                      [a["answered_by"]["model"] for a in reopened] == ["model-one", "model-two"],
                      [a["answered_by"]["model"] for a in reopened])

        report.for_a_person("that two real models both answer usefully through this path",
                            "only the local ollama model has been exercised here; the "
                            "hosted provider has never been called from this machine")
    finally:
        session.close()


def check_no_key_is_written_anywhere(exe, folder, report):
    """B4: a key belongs in the environment, not in the project or the conversation.

    The app never sees one - it does not speak to a provider - so what this holds to is
    that nothing the app writes has picked one up from anywhere else."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        session.settled()

        with Liveness(folder / "chat-bridge.json", "a bridge with a key",
                      provider="fixture", model="model-with-a-key"):
            wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"),
                     timeout=20, what="the app to see the bridge")
            ask_and_answer(session, folder, "anything", "fixture", "model-with-a-key", None)
    finally:
        session.close()

    # Nothing that looks like a key, in anything the app wrote.
    suspicious = []
    for path in sorted(folder.glob("*.json")):
        text = path.read_text(encoding="utf-8-sig", errors="replace").lower()
        for word in ("sk-", "api_key", "apikey", "authorization", "bearer "):
            if word in text:
                suspicious.append((path.name, word))

    report.expect("nothing the app writes carries a key or a header that would hold one",
                  not suspicious, suspicious)


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("a handover keeps the work and not the authorship")
    check_a_handover_keeps_the_work_and_not_the_authorship(exe, output / "handover", report)
    print()
    print("no key is written anywhere")
    check_no_key_is_written_anywhere(exe, output / "keys", report)

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
                        default=ROOT / "build-cocompose" / ("handover-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
