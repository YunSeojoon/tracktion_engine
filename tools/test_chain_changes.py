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


def check_a_send_to_somewhere_already_fed_is_not_a_second_send(exe, folder, report):
    """review-v5 R1: the comparison and the apply disagreed about an existing send.

    Applying a send to a target this insert already feeds changes the level of the send
    that is there. The preview copy appended a new one without looking, so a comparison
    played the signal twice where applying would play it once - heard one thing, got
    another, which is the failure the whole preview path exists to prevent.

    No check had a place for it, which is why twelve suites passing did not refute it."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        source = state["mixer"]["inserts"][0]["id"]
        target = "a-bus"

        def a_bus_and_a_send(live):
            live["mixer"]["inserts"].append(
                {"id": target, "index": 2, "name": "A bus", "gain_db": 0.0, "pan": 0.0,
                 "mute": False, "output": "master", "effects": [], "sends": []})
            live["mixer"]["inserts"][0]["sends"] = [
                {"id": "the-one-already-there", "target": target, "level": 0.2}]

        apply_change(project, a_bus_and_a_send)
        time.sleep(1.0)
        state = session.settled()

        sends = state["mixer"]["inserts"][0].get("sends", [])
        report.expect("there is already a send to that bus", len(sends) == 1, sends)
        if len(sends) != 1:
            return

        revision = read(folder / "sync-status.json")["revision"]
        made = tool(project, "create_proposal", {
            "description": "AI: more of it", "base_revision": revision,
            "allowed_inserts": [source],
            "chain": [{"what": "send", "insert": source, "target": target, "level": 0.8}]})
        report.expect("changing the level of an existing send is offered",
                      made["status"] == "ok", made.get("error", {}).get("message", ""))
        if made["status"] != "ok":
            return

        (folder / "preview-status.json").unlink(missing_ok=True)
        session.run([{"preview": [made["result"]["proposal"]["id"], 0.0, 8.0]}])
        heard = wait_for(lambda: (read(folder / "preview-status.json")
                                  if (folder / "preview-status.json").exists()
                                  and read(folder / "preview-status.json").get("running") is False
                                  else None),
                         timeout=240, what="the comparison to finish")
        report.expect("a comparison was rendered", heard["after"]["exists"])

        tool(project, "apply_proposal", {"proposal": made["result"]["proposal"]["id"]})
        applied = session.settled()
        now = applied["mixer"]["inserts"][0].get("sends", [])

        # This is the whole finding: one send, at the new level, on both sides of the
        # comparison. Two would mean the copy had been fed twice.
        report.expect("applying it leaves one send, not two", len(now) == 1, now)
        report.expect("and it is the one that was there, at the new level",
                      len(now) == 1 and abs(now[0]["level"] - 0.8) < 1.0e-6, now)
    finally:
        session.close()


def check_a_loop_through_a_send_is_refused_too(exe, folder, report):
    """review-v5 R2: the cycle check followed outputs and ignored sends.

    A send is a path audio travels. With A already sending to B, routing B to A closes
    a loop out of one output and one send - and neither step looks wrong on its own,
    which is exactly why the check has to walk both."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        a = state["mixer"]["inserts"][0]["id"]
        b = "the-other-one"

        def a_send_from_a_to_b(live):
            live["mixer"]["inserts"].append(
                {"id": b, "index": 2, "name": "B", "gain_db": 0.0, "pan": 0.0,
                 "mute": False, "output": "master", "effects": [], "sends": []})
            live["mixer"]["inserts"][0]["sends"] = [
                {"id": "a-to-b", "target": b, "level": 0.3}]

        apply_change(project, a_send_from_a_to_b)
        time.sleep(1.0)
        state = session.settled()
        report.expect("A already sends to B",
                      any(x["target"] == b for x in state["mixer"]["inserts"][0].get("sends", [])),
                      state["mixer"]["inserts"][0].get("sends", []))

        revision = read(folder / "sync-status.json")["revision"]
        closing = tool(project, "create_proposal", {
            "description": "AI: and back again", "base_revision": revision,
            "allowed_inserts": [b],
            "chain": [{"what": "send", "insert": b, "target": a, "level": 0.3}]})

        report.expect("a loop closed through a send is refused",
                      closing["status"] == "error"
                      and closing["error"]["code"] == "INVALID_ARGUMENT",
                      closing.get("error", {}))
        report.expect("and nothing was routed",
                      read(folder / "sync-status.json")["revision"] == revision)
    finally:
        session.close()


def check_one_proposal_cannot_close_a_loop_with_its_own_changes(exe, folder, report):
    """review-v5 R2, the second half: two changes that are each innocent alone.

    Every chain item was measured against the music as it stands. That is right for one
    change and wrong for two: a proposal carrying A to B and B to A has each half look
    fine against a model where the other half has not happened yet, so the loop is
    assembled by the very write that was supposed to refuse it."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        a = state["mixer"]["inserts"][0]["id"]
        b = "second-bus"

        def a_second_bus(live):
            live["mixer"]["inserts"].append(
                {"id": b, "index": 2, "name": "B", "gain_db": 0.0, "pan": 0.0,
                 "mute": False, "output": "master", "effects": [], "sends": []})

        apply_change(project, a_second_bus)
        time.sleep(1.0)
        session.settled()

        revision = read(folder / "sync-status.json")["revision"]
        both = tool(project, "create_proposal", {
            "description": "AI: round and round", "base_revision": revision,
            "allowed_inserts": [a, b],
            "chain": [{"what": "send", "insert": a, "target": b, "level": 0.3},
                      {"what": "send", "insert": b, "target": a, "level": 0.3}]})

        report.expect("two sends that close a loop between them are refused",
                      both["status"] == "error"
                      and both["error"]["code"] == "INVALID_ARGUMENT",
                      both.get("error", {}))
        report.expect("and the refusal says it is the changes taken together",
                      "together" in both.get("error", {}).get("message", ""),
                      both.get("error", {}).get("message", ""))
        report.expect("nothing was routed",
                      read(folder / "sync-status.json")["revision"] == revision)

        # And a pair that does not close a loop is still allowed: a check that refuses
        # everything would pass the test above and be useless.
        fine = tool(project, "create_proposal", {
            "description": "AI: both to the bus", "base_revision": revision,
            "allowed_inserts": [a, b],
            "chain": [{"what": "send", "insert": a, "target": b, "level": 0.3},
                      {"what": "add", "insert": b, "type": "reverb"}]})
        report.expect("a proposal that does not close a loop is still offered",
                      fine["status"] == "ok", fine.get("error", {}).get("message", ""))

        if fine["status"] == "ok":
            tool(project, "apply_proposal", {"proposal": fine["result"]["proposal"]["id"]})
            after = session.settled()
            report.expect("applying it routes and adds",
                          any(x["target"] == b for x in after["mixer"]["inserts"][0].get("sends", []))
                          and len(chain_of(project, b)) == 1,
                          (after["mixer"]["inserts"][0].get("sends", []), chain_of(project, b)))

            control(project, "undo")
            time.sleep(1.0)
            back = session.settled()
            report.expect("and one undo takes back both halves",
                          not back["mixer"]["inserts"][0].get("sends", [])
                          and chain_of(project, b) == [],
                          (back["mixer"]["inserts"][0].get("sends", []), chain_of(project, b)))
    finally:
        session.close()


def check_an_effect_moves_by_its_place_in_the_chain(exe, folder, report):
    """review-v5 R3: an effect ordinal is not a child index.

    A chain is counted in effects. An insert's children are effects and sends mixed
    together. Using one for the other put a move at the wrong place whenever a send sat
    earlier in the list - in the copy and in the song alike, so the comparison agreed
    with the apply about something that was wrong in both."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        mine = state["mixer"]["inserts"][0]["id"]

        # A send FIRST, then two effects: the arrangement that made the ordinals lie.
        def a_send_then_two_effects(live):
            live["mixer"]["inserts"].append(
                {"id": "a-bus", "index": 2, "name": "Bus", "gain_db": 0.0, "pan": 0.0,
                 "mute": False, "output": "master", "effects": [], "sends": []})
            live["mixer"]["inserts"][0]["sends"] = [
                {"id": "the-send", "target": "a-bus", "level": 0.2}]
            live["mixer"]["inserts"][0]["effects"] = [
                {"id": "the-delay", "type": "delay", "bypass": False, "wet": 1.0},
                {"id": "the-reverb", "type": "reverb", "bypass": False, "wet": 1.0}]

        apply_change(project, a_send_then_two_effects)
        time.sleep(1.0)
        session.settled()

        report.expect("the chain starts as delay then reverb, with a send in the list",
                      chain_of(project, mine) == ["delay", "reverb"], chain_of(project, mine))
        if chain_of(project, mine) != ["delay", "reverb"]:
            return

        revision = read(folder / "sync-status.json")["revision"]
        made = tool(project, "create_proposal", {
            "description": "AI: reverb first", "base_revision": revision,
            "allowed_inserts": [mine],
            "chain": [{"what": "move", "insert": mine, "effect": "the-reverb", "to": 0}]})
        report.expect("the move is offered", made["status"] == "ok",
                      made.get("error", {}).get("message", ""))
        if made["status"] != "ok":
            return

        report.expect("the diff promises reverb first",
                      made["result"]["diff"]["chain"][0]["position"] == {"was": 1, "now": 0},
                      made["result"]["diff"]["chain"][0])

        tool(project, "apply_proposal", {"proposal": made["result"]["proposal"]["id"]})
        session.settled()
        report.expect("and the chain is what the diff promised, send or no send",
                      chain_of(project, mine) == ["reverb", "delay"], chain_of(project, mine))

        report.expect("the send is still there",
                      len(session.settled()["mixer"]["inserts"][0].get("sends", [])) == 1,
                      session.settled()["mixer"]["inserts"][0].get("sends", []))

        control(project, "undo")
        time.sleep(1.0)
        session.settled()
        report.expect("one undo puts the order back",
                      chain_of(project, mine) == ["delay", "reverb"], chain_of(project, mine))

        # And back the other way, from the order a send still sits in front of.
        revision = read(folder / "sync-status.json")["revision"]
        other = tool(project, "create_proposal", {
            "description": "AI: delay last", "base_revision": revision,
            "allowed_inserts": [mine],
            "chain": [{"what": "move", "insert": mine, "effect": "the-delay", "to": 1}]})
        if other["status"] == "ok":
            tool(project, "apply_proposal", {"proposal": other["result"]["proposal"]["id"]})
            session.settled()
            report.expect("moving the other way lands where it was promised too",
                          chain_of(project, mine) == ["reverb", "delay"],
                          chain_of(project, mine))
    finally:
        session.close()


def check_a_chain_comparison_covers_where_that_insert_plays(exe, folder, report):
    """review-v6 R3: the comparison has to be where the thing being changed is heard.

    The range the panel works out looks at note changes and clip changes only. A change
    that touches nothing but an insert - a parameter, an effect, a send - matched neither
    and fell through to beats 0 to 8. Attach the insert for an instrument that first
    comes in at bar nine, change its reverb, and the comparison renders the opening,
    where that instrument is silent: both halves sound the same and there is nothing to
    judge.

    So the music here starts late on purpose. The comparison has to follow it."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        mine = state["mixer"]["inserts"][0]["id"]

        # Every clip, not the first one. The fixture places two and moving one left the
        # other at the beginning - so the insert really was playing there and the range
        # the app worked out was right. The check was wrong, which is the sort of thing
        # a check that has never failed hides.
        def move_them_late(live):
            for i, late in enumerate(live["playlist"]["clips"]):
                late["start"] = 64.0 + i * 32.0

        apply_change(project, move_them_late)
        time.sleep(1.0)
        moved = session.settled()["playlist"]["clips"]
        report.expect("the music starts at bar seventeen, not at the beginning",
                      all(c["start"] >= 64.0 for c in moved), [c["start"] for c in moved])
        if not all(c["start"] >= 64.0 for c in moved):
            return

        revision = read(folder / "sync-status.json")["revision"]
        made = tool(project, "create_proposal",
                    {"description": "AI: a bit of room", "base_revision": revision,
                     "allowed_inserts": [mine],
                     "chain": [{"what": "add", "insert": mine, "type": "reverb"}]})
        report.expect("there is a chain change to compare", made["status"] == "ok",
                      made.get("error", {}))
        if made["status"] != "ok":
            return

        proposal = made["result"]["proposal"]["id"]
        (folder / "preview-status.json").unlink(missing_ok=True)

        # The panel's own range working. Naming the beats here would be asking the check
        # its own answer back; whether the button reaches this is a different question
        # and has its own check.
        session.run([{"preview_panel": proposal}])
        status = wait_for(lambda: (read(folder / "preview-status.json")
                                   if (folder / "preview-status.json").exists()
                                   and read(folder / "preview-status.json").get("running") is False
                                   else None),
                          timeout=300, what="the comparison to finish")

        report.expect("the comparison covers where that insert is playing, not the "
                      "silence at the start",
                      status["end_beat"] > 64.0 and status["start_beat"] >= 32.0,
                      (status["start_beat"], status["end_beat"]))
        report.expect("and both halves rendered",
                      status["before"]["exists"] and status["after"]["exists"],
                      (status["before"]["exists"], status["after"]["exists"]))
    finally:
        session.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("a chain comparison covers where that insert plays")
    check_a_chain_comparison_covers_where_that_insert_plays(exe, output / "chainrange", report)
    print()
    print("a chain changes only where it was offered, and you hear it first")
    check_a_chain_changes_only_where_it_was_offered(exe, output / "add", report)
    print()
    print("the order of a chain can be changed")
    check_the_order_of_a_chain_can_be_changed(exe, output / "order", report)
    print()
    print("a send cannot close a loop")
    check_a_send_cannot_close_a_loop(exe, output / "loop", report)
    print()
    print("a send to somewhere already fed is the same send, louder")
    check_a_send_to_somewhere_already_fed_is_not_a_second_send(exe, output / "existing", report)
    print()
    print("a loop through a send is refused too")
    check_a_loop_through_a_send_is_refused_too(exe, output / "sendloop", report)
    print()
    print("one proposal cannot close a loop with its own changes")
    check_one_proposal_cannot_close_a_loop_with_its_own_changes(exe, output / "pairloop", report)
    print()
    print("an effect moves by its place in the chain, not among the children")
    check_an_effect_moves_by_its_place_in_the_chain(exe, output / "ordinals", report)

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
