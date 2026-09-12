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


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("an A/B, and a song that did not change")
    check_an_ab_is_two_files_and_an_untouched_song(exe, output / "ab", report)

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
