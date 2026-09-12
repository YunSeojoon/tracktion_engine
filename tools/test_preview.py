"""A3, the half a machine can check: an A/B that is two real files and an unchanged song.

The half it cannot check is the important one. Whether the proposal sounds better is
not a thing this script, or the app, or a model with no ears can report - so nothing
here claims it. What it holds to is everything around that judgement: that both files
were rendered over the same range through the same mix path, that they differ, that
they say which revision they came from, and that making them changed nothing about the
project they came from.

    python tools/test_preview.py
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


def check_an_ab_is_two_files_and_an_untouched_song(exe, folder, report):
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        pattern_id = state["patterns"][0]["id"]
        channel_id = state["channels"][0]["id"]

        part = tool(project, "inspect_pattern",
                    {"pattern": pattern_id, "channel": channel_id})["result"]
        notes = [n for p in part["parts"] for n in p["notes"]]
        report.expect("there is something to hear", len(notes) >= 4, len(notes))

        revision = read(folder / "sync-status.json")["revision"]
        made = tool(project, "create_proposal", {
            "description": "AI: a fifth up",
            "pattern": pattern_id, "channel": channel_id,
            "allowed_notes": [n["id"] for n in notes[:4]],
            "base_revision": revision,
            "keeps": {"rhythm": True, "velocity": True},
            "notes": [{"what": "change", "id": n["id"], "pitch": min(127, n["pitch"] + 7)}
                      for n in notes[:4]]})
        report.expect("a proposal to listen to", made["status"] == "ok",
                      made.get("error", {}).get("message", ""))
        if made["status"] != "ok":
            return

        proposal = made["result"]["proposal"]["id"]

        (folder / "preview-status.json").unlink(missing_ok=True)
        session.run([{"preview": [proposal, 0.0, 8.0]}])

        def finished():
            status = read(folder / "preview-status.json") if (folder / "preview-status.json").exists() else {}
            return status if status and status.get("running") is False else None

        status = wait_for(finished, timeout=240)

        report.expect("both halves were rendered",
                      status["before"]["exists"] and status["after"]["exists"],
                      (status["before"]["exists"], status["after"]["exists"]))
        # "They differ" on its own proved nothing: a review of this check found it passing
        # on render noise while the preview was rendering the old notes twice. And the
        # obvious repair - render twice, expect identical bytes - fails too: renders are
        # not bit-stable on this machine. So the proof is at the level that broke. The
        # app reports which notes the engine was playing in each half; A has to be the
        # original pitches, B has to be the proposed ones, and B must not be A.
        def played(half):
            return sorted(status[half].get("notes", []))

        # What the engine plays over [0, 8) is every note of every clip that overlaps
        # it, on every channel, at song positions - not the one pattern this check
        # happened to inspect. The first version of this expectation modelled a single
        # part and failed on its own arithmetic (36 notes played, 32 expected). The
        # region tool is the app's own account of the arrangement, and the engine
        # derives from the same tree, so the expectation is built from that.
        region = tool(project, "inspect_region", {"start_beat": 0.0, "end_beat": 8.0})["result"]
        lifted = {n["id"]: min(127, n["pitch"] + 7) for n in notes[:4]}

        def expectation(with_proposal):
            out = []
            for clip in region["clips"]:
                for part in clip.get("parts", []):
                    for n in part["notes"]:
                        pitch = lifted.get(n["id"], n["pitch"]) if with_proposal else n["pitch"]
                        out.append("%d@%.3f" % (pitch, clip["start_beat"] + n["start_beat"]))
            return sorted(out)

        original, proposed = expectation(False), expectation(True)
        report.expect("the expectation covers more than one clip",
                      len(original) > len(notes), (len(original), len(notes)))

        # When these disagree the useful thing is which notes, not how many: the first
        # failure of this check reported a count and cost a whole round trip to read.
        def difference(played_half, expected):
            from collections import Counter
            extra = Counter(played_half) - Counter(expected)
            missing = Counter(expected) - Counter(played_half)
            return "played-not-expected %s / expected-not-played %s" % (
                sorted(extra.elements())[:8], sorted(missing.elements())[:8])

        report.expect("A is the music as it is",
                      played("before") == original,
                      difference(played("before"), original))
        report.expect("B is the music as proposed",
                      played("after") == proposed,
                      difference(played("after"), proposed))
        report.expect("and B is not A - the change reached the render, not just the label",
                      played("after") != played("before"))
        report.expect("the fingerprints still tell the two files apart",
                      status["before"]["fingerprint"] != status["after"]["fingerprint"])
        report.expect("neither is empty",
                      status["before"]["bytes"] > 1000 and status["after"]["bytes"] > 1000,
                      (status["before"]["bytes"], status["after"]["bytes"]))
        report.expect("each says which music it came from",
                      status["source_revision"] == revision, (status["source_revision"], revision))
        report.expect("and over which stretch",
                      status["start_beat"] == 0.0 and status["end_beat"] == 8.0,
                      (status["start_beat"], status["end_beat"]))

        # The point of a preview: it costs the project nothing.
        report.expect("rendering a comparison changed no music",
                      read(folder / "sync-status.json")["revision"] == revision,
                      read(folder / "sync-status.json")["revision"])

        still = tool(project, "get_proposal", {"proposal": proposal})
        report.expect("and the proposal is still only a proposal",
                      still["status"] == "ok" and still["result"]["proposal"]["applied"] is False,
                      still.get("result", {}).get("proposal", {}).get("applied"))

        after = tool(project, "inspect_pattern",
                     {"pattern": pattern_id, "channel": channel_id})["result"]
        heard = {n["id"]: n["pitch"] for p in after["parts"] for n in p["notes"]}
        report.expect("the notes in the song are where they were",
                      all(heard[n["id"]] == n["pitch"] for n in notes), "some note moved")

        report.expect("the app does not claim to have heard anything",
                      status.get("heard") is False and "listening" in status.get("note", "").lower(),
                      status.get("note", "")[:80])

        report.for_a_person("whether the proposal actually sounds better",
                            "two files were made; comparing them is listening, "
                            "and nothing here can do that")

        # A stale preview must be distinguishable from a fresh one.
        first = status["after"]["fingerprint"]
        from cocompose import apply_change

        def transpose(live):
            pattern = next(p for p in live["patterns"] if p["id"] == pattern_id)
            for sequence in pattern["sequences"]:
                for note in sequence["notes"]:
                    note["pitch"] = max(0, note["pitch"] - 12)

        apply_change(project, transpose)
        time.sleep(1.0)
        moved_revision = read(folder / "sync-status.json")["revision"]
        report.expect("the music moved on", moved_revision != revision)
        report.expect("the preview on disk still says which revision it was of",
                      read(folder / "preview-status.json")["source_revision"] == revision,
                      read(folder / "preview-status.json")["source_revision"])
        report.expect("so a stale comparison can be told from a fresh one",
                      read(folder / "preview-status.json")["after"]["fingerprint"] == first)
    finally:
        session.close()


def check_a_mixer_change_is_in_what_you_hear(exe, folder, report):
    """B2: a proposal that moves a knob has to be in the comparison, not just in Apply.

    Notes live in the project tree and a preview could apply them to its copy. A
    parameter lives inside the plugin, so the copy's tree knew nothing about it and the
    preview rendered the mixer as it stands while Apply changed it. Half a proposal,
    presented as the proposal - which is worse than no preview, because a person who
    listens and approves is approving something they did not hear.

    Renders here are not bit-stable, so "the audio differs" is measured with room:
    a reverb taken from a third to fully wet moves the level of the whole file well
    past the drift between two renders of the same thing."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        insert_id = state["mixer"]["inserts"][0]["id"]

        def add_a_reverb(live):
            live["mixer"]["inserts"][0].setdefault("effects", []).append(
                {"id": "one-to-turn-up", "type": "reverb", "bypass": False, "wet": 1.0})

        apply_change(project, add_a_reverb)
        time.sleep(1.0)
        session.settled()

        insert = tool(project, "inspect_insert", {"insert": insert_id})["result"]
        # The effect's own id is what create_proposal takes for "plugin" - the service
        # matches either the engine's plugin id or the effect uid, and the effect uid is
        # the one a reader of inspect_insert actually has.
        wet, effect_id = None, None
        for effect in insert.get("effects", []):
            for p in effect.get("parameters", []):
                if p["name"].lower().startswith("wet"):
                    wet, effect_id = p, effect["id"]
                    break
        report.expect("there is a mixer parameter to move", wet is not None,
                      [p["name"] for e in insert.get("effects", []) for p in e.get("parameters", [])])
        if wet is None:
            return

        was = wet["value"]
        revision = read(folder / "sync-status.json")["revision"]
        made = tool(project, "create_proposal", {
            "description": "AI: drown it in reverb",
            "allowed_inserts": [insert_id],
            "base_revision": revision,
            "parameters": [{"owner": insert_id, "plugin": effect_id,
                            "parameter": wet["id"], "value": 1.0}]})
        report.expect("a mixer proposal to listen to", made["status"] == "ok",
                      made.get("error", {}).get("message", ""))
        if made["status"] != "ok":
            return

        proposal = made["result"]["proposal"]["id"]

        (folder / "preview-status.json").unlink(missing_ok=True)
        session.run([{"preview": [proposal, 0.0, 8.0]}])

        def finished():
            path = folder / "preview-status.json"
            status = read(path) if path.exists() else {}
            return status if status and status.get("running") is False else None

        status = wait_for(finished, timeout=240)
        report.expect("both halves were rendered",
                      status["before"]["exists"] and status["after"]["exists"],
                      (status["before"]["exists"], status["after"]["exists"]))

        # The notes are identical on both sides - this proposal does not touch them -
        # so anything that differs between the two files is the mixer, which is the
        # point. A preview that ignored the parameter would produce two renders of the
        # same thing and could only differ by drift.
        report.expect("the notes are the same on both sides, so only the mixer differs",
                      sorted(status["before"].get("notes", [])) == sorted(status["after"].get("notes", [])))

        a, b = read_wav(folder / "preview-before.wav"), read_wav(folder / "preview-after.wav")
        drift = abs(a["rms"]) * 0.02 + 1.0e-6
        report.expect("and it is audible - the two halves do not measure the same",
                      abs(a["rms"] - b["rms"]) > drift, (a["rms"], b["rms"], drift))

        report.expect("the app does not say the mixer was left out",
                      "parameter" not in (status.get("limits") or "").lower(),
                      status.get("limits"))

        # And the song paid nothing for it.
        report.expect("rendering it changed no music",
                      read(folder / "sync-status.json")["revision"] == revision,
                      read(folder / "sync-status.json")["revision"])
        after = tool(project, "inspect_insert", {"insert": insert_id})["result"]
        still = next(p for effect in after.get("effects", []) for p in effect.get("parameters", [])
                     if p["id"] == wet["id"])
        report.expect("and the knob in the song has not moved", still["value"] == was,
                      (was, still["value"]))

        # Apply, and what was heard is what the song now has.
        applied = tool(project, "apply_proposal", {"proposal": proposal})
        report.expect("applying it works", applied["status"] == "ok",
                      applied.get("error", {}).get("message", ""))
        moved = tool(project, "inspect_insert", {"insert": insert_id})["result"]
        now = next(p for effect in moved.get("effects", []) for p in effect.get("parameters", [])
                   if p["id"] == wet["id"])
        report.expect("and the knob is where the comparison had it",
                      abs(now["value"] - 1.0) < 1.0e-3, now["value"])

        report.for_a_person("whether the previewed mixer and the applied mixer sound the same",
                            "two renders of the same music are not bit-identical here, so "
                            "the last word is a person's")
    finally:
        session.close()


def check_a_mixed_proposal_is_heard_whole(exe, folder, report):
    """B2: notes and a mixer move in one proposal, and the comparison has both.

    These were the halves that came apart: notes went into the copy's tree and were
    heard, the parameter stayed in the plugin and was not, and Apply did both. A person
    who listened to that and approved it approved something they had not heard."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        pattern_id = state["patterns"][0]["id"]
        channel_id = state["channels"][0]["id"]
        insert_id = state["mixer"]["inserts"][0]["id"]

        def add_a_reverb(live):
            live["mixer"]["inserts"][0].setdefault("effects", []).append(
                {"id": "one-to-turn-up", "type": "reverb", "bypass": False, "wet": 1.0})

        apply_change(project, add_a_reverb)
        time.sleep(1.0)
        session.settled()

        insert = tool(project, "inspect_insert", {"insert": insert_id})["result"]
        wet, effect_id = None, None
        for effect in insert.get("effects", []):
            for p in effect.get("parameters", []):
                if p["name"].lower().startswith("wet"):
                    wet, effect_id = p, effect["id"]
                    break

        part = tool(project, "inspect_pattern",
                    {"pattern": pattern_id, "channel": channel_id})["result"]
        notes = [n for p in part["parts"] for n in p["notes"]]
        report.expect("there are notes and a knob to change together",
                      wet is not None and len(notes) >= 4, (wet is not None, len(notes)))
        if wet is None or len(notes) < 4:
            return

        revision = read(folder / "sync-status.json")["revision"]
        made = tool(project, "create_proposal", {
            "description": "AI: a fifth up, and wetter",
            "pattern": pattern_id, "channel": channel_id,
            "allowed_notes": [n["id"] for n in notes[:4]],
            "allowed_inserts": [insert_id],
            "base_revision": revision,
            "notes": [{"what": "change", "id": n["id"], "pitch": min(127, n["pitch"] + 7)}
                      for n in notes[:4]],
            "parameters": [{"owner": insert_id, "plugin": effect_id,
                            "parameter": wet["id"], "value": 1.0}]})
        report.expect("a proposal that does both", made["status"] == "ok",
                      made.get("error", {}).get("message", ""))
        if made["status"] != "ok":
            return

        proposal = made["result"]["proposal"]["id"]
        summary = made["result"]["proposal"]
        report.expect("and it says it does both",
                      summary["notes_changed"] == 4 and summary["parameters_changed"] == 1,
                      (summary["notes_changed"], summary["parameters_changed"]))

        (folder / "preview-status.json").unlink(missing_ok=True)
        session.run([{"preview": [proposal, 0.0, 8.0]}])

        def finished():
            path = folder / "preview-status.json"
            status = read(path) if path.exists() else {}
            return status if status and status.get("running") is False else None

        status = wait_for(finished, timeout=240)

        lifted = {min(127, n["pitch"] + 7) for n in notes[:4]}
        played_after = {int(entry.split("@")[0]) for entry in status["after"].get("notes", [])}
        report.expect("the notes moved in the half you listen to",
                      lifted.issubset(played_after), (sorted(lifted), sorted(played_after)[:8]))
        report.expect("and the notes are not the same on both sides",
                      sorted(status["before"].get("notes", [])) != sorted(status["after"].get("notes", [])))

        a, b = read_wav(folder / "preview-before.wav"), read_wav(folder / "preview-after.wav")
        report.expect("and the audio moved further than two renders drift apart",
                      abs(a["rms"] - b["rms"]) > abs(a["rms"]) * 0.02 + 1.0e-6,
                      (a["rms"], b["rms"]))
        report.expect("nothing is reported as left out of the comparison",
                      not (status.get("limits") or ""), status.get("limits"))

        report.expect("and the song still has neither change",
                      read(folder / "sync-status.json")["revision"] == revision)

        # Apply, then read both halves back out of the song.
        applied = tool(project, "apply_proposal", {"proposal": proposal})
        report.expect("applying it works", applied["status"] == "ok",
                      applied.get("error", {}).get("message", ""))

        heard = tool(project, "inspect_pattern",
                     {"pattern": pattern_id, "channel": channel_id})["result"]
        pitches = {n["id"]: n["pitch"] for p in heard["parts"] for n in p["notes"]}
        report.expect("the notes in the song are where the comparison had them",
                      all(pitches[n["id"]] == min(127, n["pitch"] + 7) for n in notes[:4]))

        moved = tool(project, "inspect_insert", {"insert": insert_id})["result"]
        now = next(p for effect in moved.get("effects", []) for p in effect.get("parameters", [])
                   if p["id"] == wet["id"])
        report.expect("and so is the knob", abs(now["value"] - 1.0) < 1.0e-3, now["value"])

        # One apply is one undo, and that has to hold for a proposal made of two
        # different kinds of change. An earlier draft of this line called a read tool
        # and asserted it answered, which proves nothing about undo at all.
        control(project, "undo")
        time.sleep(1.0)
        session.settled()

        back = tool(project, "inspect_pattern",
                    {"pattern": pattern_id, "channel": channel_id})["result"]
        where = {n["id"]: n["pitch"] for p in back["parts"] for n in p["notes"]}
        report.expect("one undo puts the notes back",
                      all(where[n["id"]] == n["pitch"] for n in notes[:4]),
                      [(n["pitch"], where[n["id"]]) for n in notes[:4]])

        returned = tool(project, "inspect_insert", {"insert": insert_id})["result"]
        knob = next(p for effect in returned.get("effects", []) for p in effect.get("parameters", [])
                    if p["id"] == wet["id"])
        report.expect("and the same undo puts the knob back",
                      abs(knob["value"] - wet["value"]) < 1.0e-3, (wet["value"], knob["value"]))
    finally:
        session.close()


def check_a_shared_pattern_says_how_much_you_heard(exe, folder, report):
    """B2: one pattern placed twice, a comparison that covers one of them.

    A note change edits the pattern, so it is heard everywhere the pattern is placed -
    and a preview renders one stretch of the song. Those are different sizes, and a
    person who listens to eight bars and presses Apply is changing more than they
    heard. The proposal says how many places share the pattern; the preview has to say
    how many of them this particular comparison covered, or the number is advice
    without a scale."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        pattern_id = state["patterns"][0]["id"]
        channel_id = state["channels"][0]["id"]

        # A second placement of the same pattern, far enough along that a comparison of
        # the first eight beats cannot reach it.
        first = next(c for c in state["playlist"]["clips"] if c["pattern"] == pattern_id)
        lane = first["lane"]
        session.run([{"select_pattern": 0}, {"select_lane": 0}, {"place": [0, 64.0]}])
        time.sleep(0.8)
        state = session.settled()

        sharing = [c for c in state["playlist"]["clips"] if c["pattern"] == pattern_id]
        report.expect("the pattern is placed more than once", len(sharing) >= 2,
                      [(c["start"], c["lane"]) for c in sharing])
        if len(sharing) < 2:
            return

        part = tool(project, "inspect_pattern",
                    {"pattern": pattern_id, "channel": channel_id})["result"]
        notes = [n for p in part["parts"] for n in p["notes"]]
        revision = read(folder / "sync-status.json")["revision"]

        made = tool(project, "create_proposal", {
            "description": "AI: a fifth up, everywhere this plays",
            "pattern": pattern_id, "channel": channel_id,
            "allowed_notes": [n["id"] for n in notes[:4]],
            "base_revision": revision,
            "notes": [{"what": "change", "id": n["id"], "pitch": min(127, n["pitch"] + 7)}
                      for n in notes[:4]]})
        report.expect("a proposal against a shared pattern", made["status"] == "ok",
                      made.get("error", {}).get("message", ""))
        if made["status"] != "ok":
            return

        proposal = made["result"]["proposal"]["id"]
        report.expect("the proposal says the pattern is shared",
                      made["result"]["proposal"]["placements"] == len(sharing),
                      (made["result"]["proposal"]["placements"], len(sharing)))

        (folder / "preview-status.json").unlink(missing_ok=True)
        session.run([{"preview": [proposal, 0.0, 8.0]}])

        def finished():
            path = folder / "preview-status.json"
            status = read(path) if path.exists() else {}
            return status if status and status.get("running") is False else None

        status = wait_for(finished, timeout=240)

        covered = len([c for c in sharing
                       if c["start"] < 8.0 and c["start"] + c["length"] > 0.0])
        report.expect("the comparison says how many places the change reaches",
                      status.get("changes_places") == len(sharing),
                      (status.get("changes_places"), len(sharing)))
        report.expect("and how many of them it actually covered",
                      status.get("places_in_this_stretch") == covered,
                      (status.get("places_in_this_stretch"), covered))
        report.expect("and those two are not the same number here - which is the point",
                      status.get("changes_places") > status.get("places_in_this_stretch"),
                      (status.get("changes_places"), status.get("places_in_this_stretch")))

        # Apply, and the far placement moved too - which is what the numbers were for.
        tool(project, "apply_proposal", {"proposal": proposal})
        region = tool(project, "inspect_region", {"start_beat": 64.0, "end_beat": 72.0})["result"]
        far = [n["pitch"] for clip in region["clips"] for p in clip.get("parts", [])
               for n in p["notes"]]
        report.expect("applying it changed the placement nobody listened to",
                      any(pitch in {min(127, n["pitch"] + 7) for n in notes[:4]} for pitch in far),
                      sorted(set(far))[:8])
    finally:
        session.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("an A/B, and a song that did not change")
    check_an_ab_is_two_files_and_an_untouched_song(exe, output / "ab", report)
    print()
    print("a mixer change is in what you hear, not only in what you apply")
    check_a_mixer_change_is_in_what_you_hear(exe, output / "mixer", report)
    print()
    print("notes and a mixer move in one proposal, heard whole")
    check_a_mixed_proposal_is_heard_whole(exe, output / "mixed", report)
    print()
    print("a shared pattern, and how much of it you actually heard")
    check_a_shared_pattern_says_how_much_you_heard(exe, output / "shared", report)

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
                        default=ROOT / "build-cocompose" / ("preview-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
