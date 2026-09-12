"""Checks the AI tool contract against the app that implements it.

Three things are worth checking and none of them is "does the model behave":

  - what the app answers matches the published schema, so a caller written against
    docs/ai-tool-contract.schema.json is not surprised;
  - a bad request is refused with the word the contract promises, and refusing it
    changes nothing;
  - the chat panel inside the app and a script outside it get the same answer to the
    same question, because they are the same service and not two implementations.

Run with the app closed:
    python tools/test_tool_contract.py --output <folder>
"""
import argparse
import copy
import json
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

import jsonschema

from cocompose import atomic_write, read, tool, wait_for
from test_plugin_compatibility import Session, prepare_song

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = json.loads((ROOT / "docs" / "ai-tool-contract.schema.json").read_text(encoding="utf-8"))


class Report:
    def __init__(self):
        self.failures = []

    def expect(self, name, condition, detail=""):
        print(("  ok   " if condition else "  FAIL ") + name + (("  " + str(detail)) if detail else ""))
        if not condition:
            self.failures.append(name)


def valid_against(fragment, definition):
    """True when this fragment matches one named shape in the schema."""
    schema = dict(SCHEMA)
    schema["$ref"] = "#/$defs/" + definition
    schema.pop("oneOf", None)
    try:
        jsonschema.validate(fragment, schema)
        return True, ""
    except jsonschema.ValidationError as error:
        return False, error.message


def in_app(session, folder, request):
    """Asks the same question through the app's own panel path."""
    (folder / "tool-response-inapp.json").unlink(missing_ok=True)
    session.run([{"tool": request}])
    wait_for(lambda: read(folder / "tool-response-inapp.json").get("request_id") == request["request_id"],
             timeout=30)
    return read(folder / "tool-response-inapp.json")


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    folder = output / "session"
    folder.mkdir(exist_ok=True)
    project = folder / "project.json"
    report = Report()

    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        state = session.settled()
        pattern_id = state["patterns"][0]["id"]
        insert_id = state["mixer"]["inserts"][0]["id"]
        revision_before = read(folder / "sync-status.json")["revision"]

        print("answers match the published schema")
        caps = tool(project, "get_capabilities")
        ok, why = valid_against(caps, "answer")
        report.expect("an answer matches the answer shape", ok, why)
        ok, why = valid_against(caps["result"], "capabilities")
        report.expect("capabilities match the capabilities shape", ok, why)
        # Rather than listing the expected tools here - which only proves the list
        # matches another list - every tool that is offered is called, and none of them
        # may answer "no such tool". That makes the promise check itself.
        offered = caps["result"]["tools"]
        report.expect("some tools are offered", len(offered) >= 5, offered)

        for name in offered:
            answer = tool(project, name, {})
            unknown = (answer["status"] == "error"
                       and ("No such tool" in answer["error"]["message"]
                            or answer["error"]["code"] == "UNSUPPORTED"))
            report.expect("an offered tool exists: " + name, not unknown,
                          answer.get("error", {}).get("message", ""))

        report.expect("writes are described as kinds of change, not as prose",
                      all("." in kind for kind in caps["result"]["writes"]),
                      caps["result"]["writes"])
        # A closed list, not a prefix test that would wave through anything starting
        # with a word it likes. What a proposal may change is the promise this whole
        # contract rests on, so adding to it is a thing somebody does on purpose and
        # this line is where they have to say so.
        #
        # Clip moves joined it once a comparison could include them: announcing a kind
        # a model cannot hear before it is taken would be offering a change nobody can
        # judge. Adding, removing and duplicating clips, and anything to do with
        # effects or routing, are deliberately still absent.
        report.expect("exactly the kinds of change this build means to allow are offered",
                      sorted(caps["result"]["writes"]) == sorted([
                          "note.pitch", "note.start_beat", "note.length_beats",
                          "note.velocity", "note.add", "note.remove",
                          "parameter.value",
                          "clip.start_beat", "clip.lane"]),
                      caps["result"]["writes"])
        report.expect("audio is declared unavailable rather than left unsaid",
                      caps["result"]["audio"]["can_send_audio"] is False)

        region = tool(project, "inspect_region", {"start_beat": 0.0, "end_beat": 16.0})
        ok, why = valid_against(region["result"], "region")
        report.expect("a region matches the region shape", ok, why)
        report.expect("what was asked about is kept apart from its context",
                      "clips" in region["result"] and "context_clips" in region["result"])

        pattern = tool(project, "inspect_pattern", {"pattern": pattern_id})
        ok, why = valid_against(pattern["result"], "pattern")
        report.expect("a pattern matches the pattern shape", ok, why)
        report.expect("a pattern says how many placements share it",
                      pattern["result"]["placement_count"] >= 1
                      and isinstance(pattern["result"]["shared"], bool))
        report.expect("notes carry stable ids, not positions",
                      all(note["id"] for note in pattern["result"]["parts"][0]["notes"]))

        insert = tool(project, "inspect_insert", {"insert": insert_id})
        ok, why = valid_against(insert["result"], "insert")
        report.expect("an insert matches the insert shape", ok, why)
        report.expect("an insert admits what it cannot read",
                      "not readable" in insert["result"]["opaque_state"])

        print()
        print("bad requests are refused by name")
        cases = [
            ("a backwards range", "inspect_region", {"start_beat": 8.0, "end_beat": 4.0},
             "INVALID_ARGUMENT"),
            ("a range that is not a number", "inspect_region",
             {"start_beat": "bar one", "end_beat": 4.0}, "INVALID_ARGUMENT"),
            ("a missing start", "inspect_region", {"end_beat": 4.0}, "INVALID_ARGUMENT"),
            ("an insert that is not there", "inspect_insert", {"insert": "nope"}, "NOT_FOUND"),
            ("a pattern that is not there", "inspect_pattern", {"pattern": "nope"}, "NOT_FOUND"),
            ("a lane that is not there", "inspect_region",
             {"start_beat": 0.0, "end_beat": 4.0, "lanes": ["nope"]}, "NOT_FOUND"),
            ("a tool that does not exist", "fly_to_the_moon", {}, "INVALID_ARGUMENT"),
            ("a tool that is not built yet", "preview_proposal", {}, "UNSUPPORTED"),
        ]

        for name, which, arguments, expected in cases:
            answer = tool(project, which, arguments)
            ok, why = valid_against(answer, "answer")
            report.expect(name + " is refused as " + expected,
                          answer["status"] == "error" and answer["error"]["code"] == expected and ok,
                          answer.get("error", {}).get("code", answer["status"]) + (" " + why if not ok else ""))

        for name in ["preview_proposal", "get_operation", "cancel_operation"]:
            report.expect("a tool that is not built is not offered: " + name,
                          name not in offered)

        wrong_version = tool(project, "get_selection", {}, contract_version=99)
        report.expect("an unknown contract version is refused",
                      wrong_version["error"]["code"] == "UNSUPPORTED")

        report.expect("none of that changed the music",
                      read(folder / "sync-status.json")["revision"] == revision_before,
                      read(folder / "sync-status.json")["revision"])

        print()
        print("asking twice")
        once = tool(project, "inspect_pattern", {"pattern": pattern_id}, request_id="same-question")
        twice = tool(project, "inspect_pattern", {"pattern": pattern_id}, request_id="same-question")
        report.expect("the same request id is answered once", once == twice)

        print()
        print("the chat panel and a script agree")
        for name, which, arguments in [("capabilities", "get_capabilities", {}),
                                       ("a region", "inspect_region", {"start_beat": 0.0, "end_beat": 16.0}),
                                       ("a pattern", "inspect_pattern", {"pattern": pattern_id}),
                                       ("an insert", "inspect_insert", {"insert": insert_id}),
                                       ("a refusal", "inspect_insert", {"insert": "nope"})]:
            request = {"contract_version": 1, "request_id": str(uuid.uuid4()),
                       "tool": which, "arguments": arguments}
            inside = in_app(session, folder, request)
            outside = tool(project, which, arguments, request_id=str(uuid.uuid4()))

            # Everything but the identifiers, which are per request by definition.
            def comparable(answer):
                trimmed = copy.deepcopy(answer)
                trimmed.pop("request_id", None)
                if "result" in trimmed and "session_id" in trimmed["result"]:
                    trimmed["result"].pop("session_id")
                return trimmed

            report.expect(name + ": the same answer either way",
                          comparable(inside) == comparable(outside),
                          json.dumps(comparable(inside))[:120])
        print()
        print("the recipes run, and stop when they are told no")
        # The contract asks for the starting recipes to be exercised by the real
        # validator rather than only written down, with a refusal branch each. A recipe
        # is only a call order over these tools, so what is being checked here is that
        # the order is right and that a refusal stops it - not that editing works, which
        # is checked where editing lives.
        import cocompose_recipes as recipes

        looked = recipes.diagnose_region(project, start_beat=0.0, end_beat=16.0)
        report.expect("diagnose-region reads without changing anything",
                      looked["changed"] is False
                      and read(folder / "sync-status.json")["revision"] == revision_before)
        report.expect("diagnose-region does not claim to have heard it",
                      looked["heard"] is False and bool(looked["why_not_heard"]))

        channel_id = state["channels"][0]["id"]
        part = tool(project, "inspect_pattern",
                    {"pattern": pattern_id, "channel": channel_id})["result"]
        notes = [n["id"] for p in part["parts"] for n in p["notes"]]
        report.expect("there are notes for the recipes to work on", len(notes) >= 4, len(notes))

        pitched = recipes.rewrite_melody(project, pattern=pattern_id, channel=channel_id,
                                         notes=notes[:2], semitones=2)
        report.expect("rewrite-melody proposes rather than edits",
                      pitched["changed"] is False
                      and pitched["proposal"]["applied"] is False
                      and read(folder / "sync-status.json")["revision"] == revision_before)
        report.expect("rewrite-melody promises the rhythm and the velocities",
                      pitched["proposal"]["keeps"]["rhythm"] is True
                      and pitched["proposal"]["keeps"]["velocity"] is True)

        # The published schema described every read answer and nothing that writes, so
        # a kind of change could be added to a proposal without the contract noticing -
        # which is what happened when clip moves went in. A caller reads the summary to
        # decide and the diff to see both sides, so both are part of the promise.
        ok, why = valid_against(pitched["proposal"], "proposalSummary")
        report.expect("a proposal matches the published proposal shape", ok, why)

        made = tool(project, "get_proposal", {"proposal": pitched["proposal"]["id"]})
        ok, why = valid_against(made["result"]["diff"], "proposalDiff")
        report.expect("and its diff matches the published diff shape", ok, why)
        report.expect("the diff always names all three kinds, empty or not",
                      set(made["result"]["diff"]) == {"notes", "parameters", "clips"},
                      sorted(made["result"]["diff"]))

        levelled = recipes.tidy_velocity(project, pattern=pattern_id, channel=channel_id,
                                         notes=notes[:2], velocity=96)
        report.expect("tidy-velocity keeps pitch and time instead",
                      levelled["proposal"]["keeps"]["pitch"] is True
                      and levelled["proposal"]["keeps"]["rhythm"] is True)

        reviewed = recipes.review_insert(project, insert=insert_id)
        report.expect("review-insert says which part it could not read",
                      reviewed["changed"] is False and "opaque" in reviewed)

        unavailable = recipes.move_clip(project)
        report.expect("move-clip says it is not available rather than pretending",
                      unavailable["status"] == "UNSUPPORTED" and unavailable["changed"] is False)

        # --- and the three ways a recipe is told no ---------------------------------
        def refused(call_it):
            try:
                call_it()
            except recipes.Refused as no:
                return no.code
            return "not refused"

        report.expect("a note outside the selection stops the recipe",
                      refused(lambda: recipes.propose(project, {
                          "description": "outside", "pattern": pattern_id,
                          "channel": channel_id, "allowed_notes": [notes[0]],
                          "keeps": {}, "notes": [{"what": "change", "id": notes[1],
                                                  "pitch": 64}]}, False, "x"))
                      == "OUT_OF_SCOPE")

        report.expect("a broken promise stops the recipe",
                      refused(lambda: recipes.propose(project, {
                          "description": "breaks its word", "pattern": pattern_id,
                          "channel": channel_id, "allowed_notes": notes[:2],
                          "keeps": {"pitch": True},
                          "notes": [{"what": "change", "id": notes[0], "pitch": 70}]},
                          False, "x")) == "LOCKED")

        stale = {"description": "out of date", "pattern": pattern_id,
                 "channel": channel_id, "allowed_notes": notes[:2], "keeps": {},
                 "notes": [{"what": "change", "id": notes[0], "pitch": 64}],
                 "base_revision": revision_before - 1}
        made = tool(project, "create_proposal", stale)
        report.expect("music that has moved on stops the recipe",
                      made["status"] == "error"
                      and made["error"]["code"] == "STALE_REVISION",
                      made.get("error", {}).get("code", made["status"]))

        report.expect("none of those refusals changed the music",
                      read(folder / "sync-status.json")["revision"] == revision_before,
                      read(folder / "sync-status.json")["revision"])

    finally:
        session.close()

    print()
    print("FAILURES:", report.failures if report.failures else "none")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=ROOT / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build-cocompose" / ("contract-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
