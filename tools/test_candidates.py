"""B3, the shelf: three ways of doing the same eight bars, and taking one of them.

Composing with an assistant is asking twice. Until now the second answer was gone by
the time the third arrived - a proposal lived in memory for as long as the app did - so
"go back to the second one" was answered by asking again and hoping, and what came back
was a fourth version described as the second.

What this holds to is everything a person needs in order not to have to remember:
three offers survive each other, taking one leaves the rest alone, throwing one away
deletes no music, an undo of the music does not un-offer anything, and closing the app
loses none of it.

    python tools/test_candidates.py
"""
import argparse
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import atomic_write, control, read, tool, wait_for
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


def shelf(folder):
    """What the app believes is on the shelf.

    Read from the app's own view rather than candidates.json, because they are not
    always the same thing and the app's is the one that matters: a copied project folder
    still holds the file written for the original, and this app will not adopt it. The
    notes are read the same way, for the same reason."""
    return (read(folder / "chat-inspector.json")
            .get("candidates", {})
            .get("candidates", []))


def shelf_once(folder, settled, what):
    """The shelf once it shows what an action has done.

    The app's view is written on a timer, so reading it the instant an action returns
    reads the moment before. That is not the app being slow - it is this check being
    early - and waiting for the state it is about to assert is the difference between a
    check and a race."""
    return wait_for(lambda: (shelf(folder) if settled(shelf(folder)) else None),
                    timeout=20, what=what)


def ask_once(session, folder, change, previous):
    """One question and one answer, inside a bridge that is already connected.

    The bridge stays up across all three questions on purpose. Each `Liveness` is a new
    instance id, and the app treats a new instance as the bridge having restarted - it
    will not attribute an answer to a question asked by the one before, which is correct
    and is also what made an earlier version of this check time out on its second ask.
    """
    session.run([{"attach": "notes"}])
    session.run([{"chat": "ask: another way of doing this"}])

    # Waiting for chat-request.json to merely exist is no wait at all on a second
    # question: it still holds the first, and a reply carrying that id is one the app
    # has already dealt with, so nothing would ever answer.
    request = wait_for(lambda: (read(folder / "chat-request.json")
                                if read(folder / "chat-request.json").get("request_id") != previous
                                else None),
                       timeout=20, what="a question other than " + str(previous))

    atomic_write(folder / "chat-reply.json",
                 {"request_id": request["request_id"], "status": "ok",
                  "text": "a synthetic reply", "provider": "fixture", "change": change})

    def considered():
        # Asking adds the question and an empty assistant message straight away, and the
        # answer fills that one in rather than appending another. Waiting for the count
        # to grow - which an earlier version of this did - waits for something that
        # never happens. What changes is that this request's answer stops streaming and
        # carries a verdict.
        for message in read(folder / "conversation.json")["messages"]:
            if (message.get("request_id") == request["request_id"]
                    and message.get("from") == "assistant"
                    and (message.get("proposal_id") or message.get("proposal_problem"))):
                return message
        return None

    wait_for(considered, timeout=20, what="the app to consider the reply")
    return request["request_id"]


def check_three_ways_of_the_same_bars(exe, folder, report):
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        pattern_id = prepare_song(session)
        state = session.settled()
        channel_id = state["channels"][0]["id"]

        part = tool(project, "inspect_pattern",
                    {"pattern": pattern_id, "channel": channel_id})["result"]
        notes = [n for p in part["parts"] for n in p["notes"]]
        report.expect("there is something to offer alternatives for", len(notes) >= 1, len(notes))
        if not notes:
            return

        original = {n["id"]: n["pitch"] for n in notes}
        subject = notes[0]

        # Three answers, each doing something different, from one connected bridge.
        with Liveness(folder / "chat-bridge.json", "fixture bridge"):
            wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"),
                     timeout=20, what="the app to see the fixture bridge")

            asked = None
            for lift in (2, 5, 7):
                asked = ask_once(session, folder,
                                 {"description": "AI: up %d" % lift,
                                  "notes": [{"what": "change", "id": subject["id"],
                                             "pitch": min(127, subject["pitch"] + lift)}]},
                                 asked)
                time.sleep(0.4)

        kept = shelf(folder)
        report.expect("all three are on the shelf, not just the last one",
                      len(kept) == 3, [c["description"] for c in kept])
        if len(kept) != 3:
            return

        report.expect("each says what it would do",
                      all(c["diff"]["notes"] for c in kept),
                      [len(c["diff"]["notes"]) for c in kept])
        report.expect("and which music it was worked out against",
                      all(c["base_revision"] > 0 for c in kept),
                      [c["base_revision"] for c in kept])
        # Three asks are three questions - that is what asking again is - so what makes
        # these alternatives is not a shared request id but that they are kept in the
        # order they were offered, with the question each one answered. That order is
        # what "the second one" means, and it is the only thing that can make the phrase
        # resolve to the same candidate for the person and for the app.
        report.expect("each one names the question it answered",
                      all(c["request_id"] for c in kept),
                      [c["request_id"][:8] for c in kept])
        report.expect("and they are in the order they were offered",
                      [c["description"] for c in kept] == ["AI: up 2", "AI: up 5", "AI: up 7"],
                      [c["description"] for c in kept])
        report.expect("none of them is taken yet",
                      not any(c["adopted"] for c in kept))
        report.expect("and offering them changed no music",
                      all(original[n["id"]] == n["pitch"]
                          for n in tool(project, "inspect_pattern",
                                        {"pattern": pattern_id, "channel": channel_id})
                             ["result"]["parts"][0]["notes"]))

        # Listen to the second one. A comparison is rendered, and the shelf records what
        # it was heard with.
        second = kept[1]
        (folder / "preview-status.json").unlink(missing_ok=True)
        session.run([{"preview": [second["id"], 0.0, 8.0]}])
        status = wait_for(lambda: (read(folder / "preview-status.json")
                                   if (folder / "preview-status.json").exists()
                                   and read(folder / "preview-status.json").get("running") is False
                                   else None),
                          timeout=240, what="the comparison to finish")

        kept_now = shelf_once(folder,
                              lambda entries: any(c["id"] == second["id"]
                                                  and c["preview"]["after_fingerprint"]
                                                  for c in entries),
                              "the shelf to record the comparison")
        heard = next(c for c in kept_now if c["id"] == second["id"])
        report.expect("the one that was listened to remembers the comparison",
                      heard["preview"]["after_fingerprint"] == status["after"]["fingerprint"],
                      (heard["preview"]["after_fingerprint"][:8],
                       status["after"]["fingerprint"][:8]))
        report.expect("and says what stretch it covered",
                      "beats" in heard["preview"]["note"], heard["preview"]["note"])
        report.expect("the others were not listened to, and do not claim to have been",
                      all(not c["preview"]["after_fingerprint"]
                          for c in shelf(folder) if c["id"] != second["id"]))

        # Take the second one.
        session.run([{"candidate": ["adopt", second["id"]]}])
        time.sleep(1.0)
        session.settled()

        after = shelf_once(folder,
                           lambda entries: any(c["id"] == second["id"] and c["adopted"]
                                               for c in entries),
                           "the shelf to mark the second one taken")
        report.expect("the second one is marked as taken",
                      next(c for c in after if c["id"] == second["id"])["adopted"])
        report.expect("and the other two are untouched - not rejected, just not taken",
                      not any(c["adopted"] for c in after if c["id"] != second["id"]))

        now = {n["id"]: n["pitch"] for n in tool(project, "inspect_pattern",
                                                {"pattern": pattern_id, "channel": channel_id})
                                     ["result"]["parts"][0]["notes"]}
        report.expect("the music is what the second one said it would be",
                      now[subject["id"]] == min(127, original[subject["id"]] + 5),
                      (original[subject["id"]], now[subject["id"]]))

        # Throwing away one that was not taken deletes an offer, never any music.
        third = after[2]["id"]
        session.run([{"candidate": ["discard", third]}])
        report.expect("a discarded candidate is off the shelf",
                      shelf_once(folder,
                                 lambda entries: third not in {c["id"] for c in entries},
                                 "the discarded candidate to leave the shelf") is not None)
        still = {n["id"]: n["pitch"] for n in tool(project, "inspect_pattern",
                                                  {"pattern": pattern_id, "channel": channel_id})
                                       ["result"]["parts"][0]["notes"]}
        report.expect("and discarding it deleted no music",
                      all(still[i] == now[i] for i in now))

        # Undo takes back the music. It does not take back the offers: a person who
        # undoes an edit is undoing an edit, not forgetting what they were shown.
        control(project, "undo")
        time.sleep(1.0)
        session.settled()

        back = {n["id"]: n["pitch"] for n in tool(project, "inspect_pattern",
                                                 {"pattern": pattern_id, "channel": channel_id})
                                      ["result"]["parts"][0]["notes"]}
        report.expect("one undo puts the music back",
                      back[subject["id"]] == original[subject["id"]],
                      (original[subject["id"]], back[subject["id"]]))
        report.expect("and the shelf still holds what was offered",
                      len(shelf(folder)) == 2, len(shelf(folder)))

        # Close it and open it again.
        session.close()
        session = Session(exe, folder).open()
        session.settled()

        reopened = shelf_once(folder, lambda entries: len(entries) == 2,
                              "the reopened app to show what it kept")
        report.expect("the shelf survives closing the app",
                      len(reopened) == 2, len(reopened))
        report.expect("and still knows which one was taken",
                      [c["adopted"] for c in reopened] == [False, True],
                      [(c["description"], c["adopted"]) for c in reopened])
        report.expect("and what each one would do",
                      all(c["diff"]["notes"] for c in reopened))
        report.expect("the conversation came back too",
                      len(read(folder / "conversation.json")["messages"]) >= 6,
                      len(read(folder / "conversation.json")["messages"]))

        report.for_a_person("whether the three alternatives are actually different music "
                            "worth choosing between",
                            "three renders were made; choosing is listening")
    finally:
        session.close()


def check_a_copied_project_does_not_inherit_the_shelf(exe, folder, report):
    """The same rule the notes follow. A copied folder is a different project, and the
    alternatives offered for the original were about music that is now somewhere else."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        pattern_id = prepare_song(session)
        state = session.settled()
        channel_id = state["channels"][0]["id"]
        notes = tool(project, "inspect_pattern",
                     {"pattern": pattern_id, "channel": channel_id})["result"]["parts"][0]["notes"]

        with Liveness(folder / "chat-bridge.json", "fixture bridge"):
            wait_for(lambda: read(folder / "chat-inspector.json").get("bridge_connected"),
                     timeout=20, what="the app to see the fixture bridge")
            ask_once(session, folder,
                     {"description": "AI: something",
                      "notes": [{"what": "change", "id": notes[0]["id"],
                                 "pitch": min(127, notes[0]["pitch"] + 3)}]}, None)
        report.expect("there is a candidate to not inherit",
                      shelf_once(folder, lambda entries: len(entries) == 1,
                                 "the candidate to appear on the shelf") is not None)
    finally:
        session.close()

    copy = folder.parent / (folder.name + "-copied")
    copy.mkdir(parents=True, exist_ok=True)
    for name in ("project.json", "candidates.json"):
        if (folder / name).exists():
            (copy / name).write_bytes((folder / name).read_bytes())

    other = Session(exe, copy).open()
    try:
        other.settled()
        report.expect("a copied project starts with an empty shelf",
                      not shelf(copy), shelf(copy))
    finally:
        other.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("three ways of the same bars, and taking one")
    check_three_ways_of_the_same_bars(exe, output / "three", report)
    print()
    print("a copied project does not inherit what was offered for the original")
    check_a_copied_project_does_not_inherit_the_shelf(exe, output / "copied", report)

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
                        default=ROOT / "build-cocompose" / ("candidates-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
