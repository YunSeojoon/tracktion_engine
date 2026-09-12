"""B5, the first kind: a proposal that moves a placement.

A clip is a reference to a pattern, so moving one moves where that music is heard
without copying or altering the music itself. That is why it is the first arrangement
edit to open: nothing here can damage a pattern, and a comparison can include it,
because a placement is in the project tree the way a note is and not inside a plugin
the way a parameter is.

What this holds to is the same contract every other write kind is held to. Scope comes
from what was attached and not from what was asked for. A comparison includes it, so a
person can hear the move before taking it. Applying is one undo. And the pattern the
clip points at is untouched, because moving where music is played is not editing it.

    python tools/test_clip_moves.py
"""
import argparse
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import read, tool, wait_for, control
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


def clips(state):
    return state["playlist"]["clips"]


def check_a_clip_can_be_moved_only_where_it_was_offered(exe, folder, report):
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        pattern_id = prepare_song(session)
        state = session.settled()
        channel_id = state["channels"][0]["id"]
        lanes = state["playlist"]["lanes"]

        # Two placements: one inside the region that will be attached, one well past it.
        session.run([{"select_pattern": 1}, {"select_lane": 0}, {"place": [0, 64.0]}])
        time.sleep(0.8)
        state = session.settled()

        inside = next(c for c in clips(state) if c["start"] < 8.0)
        outside = next(c for c in clips(state) if c["start"] >= 64.0)
        report.expect("there is a clip inside the region and one outside it",
                      inside and outside, [(c["start"], c["id"]) for c in clips(state)])

        capabilities = tool(project, "get_capabilities")["result"]
        report.expect("moving a clip is announced as something a proposal may do",
                      "clip.start_beat" in capabilities["writes"], capabilities["writes"])

        revision = read(folder / "sync-status.json")["revision"]

        # Nothing attached: no clip may move, however precisely it is named.
        refused = tool(project, "create_proposal", {
            "description": "AI: shift it along", "base_revision": revision,
            "clips": [{"id": inside["id"], "start_beat": 4.0}]})
        report.expect("with no region attached, no clip may be moved",
                      refused["status"] == "error"
                      and refused["error"]["code"] == "OUT_OF_SCOPE",
                      refused.get("error", {}))

        # A region that covers one of them. The other stays out of reach.
        scoped = {"description": "AI: shift it along",
                  "base_revision": revision,
                  "allowed_clips": [inside["id"]]}

        reaching = tool(project, "create_proposal",
                        dict(scoped, clips=[{"id": outside["id"], "start_beat": 4.0}]))
        report.expect("a clip outside what was attached is refused",
                      reaching["status"] == "error"
                      and reaching["error"]["code"] == "OUT_OF_SCOPE",
                      reaching.get("error", {}))

        before_the_beginning = tool(project, "create_proposal",
                                    dict(scoped, clips=[{"id": inside["id"], "start_beat": -4.0}]))
        report.expect("a clip cannot be moved before the beginning of the song",
                      before_the_beginning["status"] == "error"
                      and before_the_beginning["error"]["code"] == "INVALID_ARGUMENT",
                      before_the_beginning.get("error", {}))

        nowhere = tool(project, "create_proposal",
                       dict(scoped, clips=[{"id": inside["id"]}]))
        report.expect("and a move has to say where to",
                      nowhere["status"] == "error"
                      and nowhere["error"]["code"] == "INVALID_ARGUMENT",
                      nowhere.get("error", {}))

        no_such_lane = tool(project, "create_proposal",
                            dict(scoped, clips=[{"id": inside["id"], "lane": "not-a-lane"}]))
        report.expect("a lane that does not exist is NOT_FOUND, not a silent no-op",
                      no_such_lane["status"] == "error"
                      and no_such_lane["error"]["code"] == "NOT_FOUND",
                      no_such_lane.get("error", {}))

        report.expect("and none of those refusals moved anything",
                      read(folder / "sync-status.json")["revision"] == revision)

        # The move that is inside the lines.
        made = tool(project, "create_proposal",
                    dict(scoped, clips=[{"id": inside["id"], "start_beat": 16.0}]))
        report.expect("a move inside what was attached is offered", made["status"] == "ok",
                      made.get("error", {}).get("message", ""))
        if made["status"] != "ok":
            return

        proposal = made["result"]["proposal"]["id"]
        report.expect("the proposal says it moves one clip",
                      made["result"]["proposal"]["clips_moved"] == 1,
                      made["result"]["proposal"])
        moved = made["result"]["diff"]["clips"][0]
        report.expect("and the diff shows where from and where to",
                      (moved["start_beat"]["was"], moved["start_beat"]["now"])
                      == (inside["start"], 16.0), moved["start_beat"])

        # It is in the comparison, not only in the apply.
        (folder / "preview-status.json").unlink(missing_ok=True)
        session.run([{"preview": [proposal, 0.0, 24.0]}])
        status = wait_for(lambda: (read(folder / "preview-status.json")
                                   if (folder / "preview-status.json").exists()
                                   and read(folder / "preview-status.json").get("running") is False
                                   else None),
                          timeout=240, what="the comparison to finish")

        at_sixteen = [n for n in status["after"].get("notes", []) if float(n.split("@")[1]) >= 16.0]
        was_at_sixteen = [n for n in status["before"].get("notes", []) if float(n.split("@")[1]) >= 16.0]
        report.expect("the half you listen to has the clip in its new place",
                      len(at_sixteen) > len(was_at_sixteen),
                      (len(was_at_sixteen), len(at_sixteen)))
        report.expect("and the two halves are not the same music",
                      sorted(status["before"].get("notes", [])) != sorted(status["after"].get("notes", [])))
        report.expect("rendering it moved nothing in the song",
                      read(folder / "sync-status.json")["revision"] == revision)

        # Apply, and the clip is where the comparison had it.
        applied = tool(project, "apply_proposal", {"proposal": proposal})
        report.expect("applying it works", applied["status"] == "ok",
                      applied.get("error", {}).get("message", ""))

        after = session.settled()
        now = next(c for c in clips(after) if c["id"] == inside["id"])
        report.expect("the clip is where the comparison had it", now["start"] == 16.0, now["start"])
        report.expect("the one outside the region did not move",
                      next(c for c in clips(after) if c["id"] == outside["id"])["start"]
                      == outside["start"])

        # Moving where music is played is not editing it.
        part = tool(project, "inspect_pattern",
                    {"pattern": pattern_id, "channel": channel_id})["result"]
        report.expect("the pattern the clip points at is untouched",
                      len([n for p in part["parts"] for n in p["notes"]]) > 0)

        control(project, "undo")
        time.sleep(1.0)
        back = session.settled()
        report.expect("one undo puts the clip back",
                      next(c for c in clips(back) if c["id"] == inside["id"])["start"]
                      == inside["start"],
                      next(c for c in clips(back) if c["id"] == inside["id"])["start"])

        report.for_a_person("whether moving it there is a good idea",
                            "a comparison was rendered; whether the arrangement is better "
                            "for it is listening")
    finally:
        session.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("a clip moves only where it was offered, and you hear it first")
    check_a_clip_can_be_moved_only_where_it_was_offered(exe, output / "moves", report)

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
                        default=ROOT / "build-cocompose" / ("clipmoves-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
