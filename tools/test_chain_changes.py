"""B5, the last kinds: what is on an insert's chain, in what order, and what it feeds.

The plugins on a mixer bus are derived from the project tree, so a chain change is a
tree edit - which is why a comparison can include one, and why these could be offered
at all. That order is the rule: previewable first, announced second.

What this holds to is the same contract the other write kinds are held to, plus the two
things that are specific to a mixer. An effect has to be one this machine actually has,
because naming a plugin that is not installed produces a chain with a hole in it that
nobody finds until they press play. And a send that would feed a signal back into
itself is refused, because that does not get quieter.

    python tools/test_chain_changes.py
"""
import argparse
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import apply_change, control, read, tool, wait_for
from test_plugin_compatibility import Session, prepare_song, read_wav

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


def chain_of(project, insert_id):
    insert = tool(project, "inspect_insert", {"insert": insert_id})["result"]
    return [e["name"] for e in insert.get("effects", [])]


def check_a_chain_changes_only_where_it_was_offered(exe, folder, report):
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        inserts = state["mixer"]["inserts"]
        mine = inserts[0]["id"]
        report.expect("there is an insert to work on", bool(mine))

        capabilities = tool(project, "get_capabilities")["result"]
        for kind in ("effect.add", "effect.remove", "effect.move", "effect.bypass",
                     "send.add", "send.remove"):
            report.expect("the contract announces " + kind, kind in capabilities["writes"])

        revision = read(folder / "sync-status.json")["revision"]

        nothing_attached = tool(project, "create_proposal", {
            "description": "AI: a bit of room", "base_revision": revision,
            "chain": [{"what": "add", "insert": mine, "type": "reverb"}]})
        report.expect("with no insert attached, no chain may be changed",
                      nothing_attached["status"] == "error"
                      and nothing_attached["error"]["code"] == "OUT_OF_SCOPE",
                      nothing_attached.get("error", {}))

        scoped = {"description": "AI: a bit of room", "base_revision": revision,
                  "allowed_inserts": [mine]}

        not_installed = tool(project, "create_proposal",
                             dict(scoped, chain=[{"what": "add", "insert": mine,
                                                  "type": "a-plugin-nobody-has"}]))
        report.expect("an effect this machine does not have is refused",
                      not_installed["status"] == "error"
                      and not_installed["error"]["code"] == "NOT_FOUND",
                      not_installed.get("error", {}))

        invented = tool(project, "create_proposal",
                        dict(scoped, chain=[{"what": "sideways", "insert": mine,
                                             "type": "reverb"}]))
        report.expect("a verb the service does not know is refused",
                      invented["status"] == "error"
                      and invented["error"]["code"] == "INVALID_ARGUMENT",
                      invented.get("error", {}))

        itself = tool(project, "create_proposal",
                      dict(scoped, chain=[{"what": "send", "insert": mine, "target": mine}]))
        report.expect("an insert cannot be told to send to itself",
                      itself["status"] == "error"
                      and itself["error"]["code"] == "INVALID_ARGUMENT",
                      itself.get("error", {}))

        report.expect("and none of those refusals changed anything",
                      read(folder / "sync-status.json")["revision"] == revision)

        # Adding one that is installed.
        made = tool(project, "create_proposal",
                    dict(scoped, chain=[{"what": "add", "insert": mine, "type": "reverb"}]))
        report.expect("an effect this machine has can be proposed", made["status"] == "ok",
                      made.get("error", {}).get("message", ""))
        if made["status"] != "ok":
            return

        report.expect("the proposal counts it as a chain change",
                      made["result"]["proposal"]["chain_changes"] == 1,
                      made["result"]["proposal"])
        report.expect("and the diff says what kind it would add",
                      made["result"]["diff"]["chain"][0]["type"] == "reverb",
                      made["result"]["diff"]["chain"][0])

        # Heard before taken: the plugins are derived from the tree, so the copy builds
        # the chain the proposal describes.
        (folder / "preview-status.json").unlink(missing_ok=True)
        session.run([{"preview": [made["result"]["proposal"]["id"], 0.0, 8.0]}])
        status = wait_for(lambda: (read(folder / "preview-status.json")
                                   if (folder / "preview-status.json").exists()
                                   and read(folder / "preview-status.json").get("running") is False
                                   else None),
                          timeout=240, what="the comparison to finish")

        report.expect("both halves were rendered",
                      status["before"]["exists"] and status["after"]["exists"])
        report.expect("the notes are the same on both sides, so any difference is the chain",
                      sorted(status["before"]["notes"]) == sorted(status["after"]["notes"]))

        a, b = read_wav(folder / "preview-before.wav"), read_wav(folder / "preview-after.wav")
        report.expect("and adding a reverb is audible - further than two renders drift apart",
                      abs(a["rms"] - b["rms"]) > abs(a["rms"]) * 0.02 + 1.0e-6,
                      (a["rms"], b["rms"]))
        # The notes are identical and the sound is not, which is exactly why the
        # field says "notes" and not "music": the app can compare what the engine
        # played, and cannot compare how it sounded.
        report.expect("the app reports the notes as unchanged, which is all it can know",
                      status["same_notes_both_halves"] is True,
                      status.get("same_notes_both_halves"))
        report.expect("rendering it changed no music",
                      read(folder / "sync-status.json")["revision"] == revision)
        report.expect("and the chain in the song is still empty",
                      chain_of(project, mine) == [], chain_of(project, mine))

        tool(project, "apply_proposal", {"proposal": made["result"]["proposal"]["id"]})
        session.settled()
        report.expect("applying it puts the effect on the chain",
                      len(chain_of(project, mine)) == 1, chain_of(project, mine))

        control(project, "undo")
        time.sleep(1.0)
        session.settled()
        report.expect("one undo takes it back off", chain_of(project, mine) == [],
                      chain_of(project, mine))

        report.for_a_person("whether that is the right amount of reverb",
                            "a comparison was rendered; how it sounds is listening")
    finally:
        session.close()


def check_the_order_of_a_chain_can_be_changed(exe, folder, report):
    """Order matters in a chain, so moving one is a real change - and a position that
    is not on the chain is refused rather than clamped to an end somebody did not ask
    for."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        mine = state["mixer"]["inserts"][0]["id"]

        def two_effects(live):
            live["mixer"]["inserts"][0]["effects"] = [
                {"id": "first-one", "type": "delay", "bypass": False, "wet": 1.0},
                {"id": "second-one", "type": "reverb", "bypass": False, "wet": 1.0}]

        apply_change(project, two_effects)
        time.sleep(1.0)
        session.settled()
        report.expect("there are two effects to reorder", len(chain_of(project, mine)) == 2,
                      chain_of(project, mine))

        revision = read(folder / "sync-status.json")["revision"]
        scoped = {"description": "AI: other way round", "base_revision": revision,
                  "allowed_inserts": [mine]}

        off_the_end = tool(project, "create_proposal",
                           dict(scoped, chain=[{"what": "move", "insert": mine,
                                                "effect": "second-one", "to": 7}]))
        report.expect("a position that is not on the chain is refused, not clamped",
                      off_the_end["status"] == "error"
                      and off_the_end["error"]["code"] == "INVALID_ARGUMENT",
                      off_the_end.get("error", {}))

        missing = tool(project, "create_proposal",
                       dict(scoped, chain=[{"what": "bypass", "insert": mine,
                                            "effect": "not-on-this-chain", "on": True}]))
        report.expect("an effect that is not on that insert is NOT_FOUND",
                      missing["status"] == "error"
                      and missing["error"]["code"] == "NOT_FOUND",
                      missing.get("error", {}))

        made = tool(project, "create_proposal",
                    dict(scoped, chain=[{"what": "move", "insert": mine,
                                         "effect": "second-one", "to": 0}]))
        report.expect("a move inside the chain is offered", made["status"] == "ok",
                      made.get("error", {}).get("message", ""))
        if made["status"] != "ok":
            return

        report.expect("the diff says where it was and where it would go",
                      made["result"]["diff"]["chain"][0]["position"] == {"was": 1, "now": 0},
                      made["result"]["diff"]["chain"][0])

        before = chain_of(project, mine)
        tool(project, "apply_proposal", {"proposal": made["result"]["proposal"]["id"]})
        session.settled()
        report.expect("the chain is in the new order",
                      chain_of(project, mine) == list(reversed(before)),
                      (before, chain_of(project, mine)))

        control(project, "undo")
        time.sleep(1.0)
        session.settled()
        report.expect("one undo puts the order back", chain_of(project, mine) == before,
                      chain_of(project, mine))
    finally:
        session.close()


def check_a_send_cannot_close_a_loop(exe, folder, report):
    """A signal that reaches its own input does not get quieter. The app refuses this
    from its own menu; a proposal is refused by the same test, because a rule enforced
    in one place and not the other is not a rule."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        source = state["mixer"]["inserts"][0]["id"]
        target = "insert-2"

        # A second insert, pointed at the first. A send from the first to the second
        # would then close the loop.
        def a_second_insert(live):
            live["mixer"]["inserts"].append(
                {"id": target, "index": 2, "name": "A bus", "gain_db": 0.0, "pan": 0.0,
                 "mute": False, "output": source, "effects": [], "sends": []})

        apply_change(project, a_second_insert)
        time.sleep(1.0)
        session.settled()
        report.expect("there are two inserts to route between",
                      len(session.settled()["mixer"]["inserts"]) == 2,
                      [i["id"] for i in session.settled()["mixer"]["inserts"]])

        def route(live):
            for one in live["mixer"]["inserts"]:
                if one["id"] == target:
                    one["output"] = source

        apply_change(project, route)
        time.sleep(1.0)
        session.settled()

        revision = read(folder / "sync-status.json")["revision"]
        looping = tool(project, "create_proposal", {
            "description": "AI: feed it back", "base_revision": revision,
            "allowed_inserts": [source],
            "chain": [{"what": "send", "insert": source, "target": target}]})
        report.expect("a send that would close a loop is refused",
                      looping["status"] == "error"
                      and looping["error"]["code"] == "INVALID_ARGUMENT",
                      looping.get("error", {}))
        report.expect("and the refusal says why, rather than just no",
                      "back into itself" in looping.get("error", {}).get("message", ""),
                      looping.get("error", {}).get("message", ""))
        report.expect("nothing was routed", read(folder / "sync-status.json")["revision"] == revision)
    finally:
        session.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("a chain changes only where it was offered, and you hear it first")
    check_a_chain_changes_only_where_it_was_offered(exe, output / "add", report)
    print()
    print("the order of a chain can be changed")
    check_the_order_of_a_chain_can_be_changed(exe, output / "order", report)
    print()
    print("a send cannot close a loop")
    check_a_send_cannot_close_a_loop(exe, output / "loop", report)

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
                        default=ROOT / "build-cocompose" / ("chain-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
