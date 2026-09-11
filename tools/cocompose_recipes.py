"""The five starting recipes from the tool contract, as calls rather than prose.

A recipe is an order of tool calls, the conditions it promises to keep, and what it
does when it is refused. It is deliberately not an engine: nothing here knows what a
note is, nothing here edits a ValueTree, and nothing here can get past a check the app
makes. Every one of them is a thin wrapper over the same tool service the chat panel
inside the app talks to, so a script and the chat cannot end up disagreeing.

That is the point of them. The contract asks for recipes instead of a pile of separate
edit scripts, because a separate script is a second implementation of the rules, and a
second implementation is a second set of bugs.

Run one:
    python tools/cocompose_recipes.py <recipe> --project <path to project.json> [options]

    diagnose-region   --start-beat 0 --end-beat 16
    rewrite-melody    --pattern <id> --channel <id> --notes <id,id> --semitones 2
    tidy-velocity     --pattern <id> --channel <id> --notes <id,id> --velocity 96
    review-insert     --insert <id>
    move-clip         --clip <id> --to-beat 32     (not available in this build)

Nothing applies itself. A recipe that proposes a change prints the proposal and stops;
pass --apply to go through with it, which is one undo.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import read, tool


class Refused(RuntimeError):
    """The app said no. The recipe stops rather than trying to get round it."""

    def __init__(self, answer):
        self.code = answer.get("error", {}).get("code", "UNKNOWN")
        self.message = answer.get("error", {}).get("message", "")
        super().__init__("%s: %s" % (self.code, self.message))


def call(project, name, arguments=None):
    answer = tool(project, name, arguments or {})
    if answer.get("status") != "ok":
        raise Refused(answer)
    return answer["result"]


def revision(project):
    return read(Path(project).parent / "sync-status.json")["revision"]


# --- 1. diagnose a region ------------------------------------------------------------

def diagnose_region(project, start_beat, end_beat, **_):
    """Reads a stretch of the arrangement and its immediate surroundings. Changes
    nothing, and says plainly that it has not heard any of it - the difference between
    reading structure and listening is the one thing this recipe must not blur."""
    region = call(project, "inspect_region", {"start_beat": start_beat, "end_beat": end_beat})
    audio = call(project, "get_capabilities", {})["audio"]

    return {"recipe": "diagnose-region",
            "read": region,
            "heard": False,
            "why_not_heard": audio.get("reason", "no audio was sent"),
            "changed": False}


# --- 2. rewrite a melody -------------------------------------------------------------

def rewrite_melody(project, pattern, channel, notes, semitones, apply=False, **_):
    """Moves the chosen notes by a fixed interval and promises to leave the rhythm and
    the velocities where they are. The promise is not taken on trust: the app measures
    the result and refuses the whole proposal if it was broken."""
    part = call(project, "inspect_pattern", {"pattern": pattern, "channel": channel})
    wanted = set(notes)
    chosen = [n for p in part["parts"] for n in p["notes"] if n["id"] in wanted]

    if not chosen:
        raise SystemExit("None of those notes are in that part")

    return propose(project, {
        "description": "Move %d note(s) by %+d semitones" % (len(chosen), semitones),
        "pattern": pattern, "channel": channel,
        "allowed_notes": sorted(wanted),
        "keeps": {"rhythm": True, "velocity": True},
        "notes": [{"what": "change", "id": n["id"],
                   "pitch": max(0, min(127, n["pitch"] + semitones))} for n in chosen]},
        apply, "rewrite-melody")


# --- 3. tidy velocities --------------------------------------------------------------

def tidy_velocity(project, pattern, channel, notes, velocity, apply=False, **_):
    """Levels the chosen notes to one velocity, keeping pitch and time. Same shape as
    the melody recipe, different promise - which is the whole idea: the recipes differ
    in what they keep, not in how they edit."""
    part = call(project, "inspect_pattern", {"pattern": pattern, "channel": channel})
    wanted = set(notes)
    chosen = [n for p in part["parts"] for n in p["notes"] if n["id"] in wanted]

    if not chosen:
        raise SystemExit("None of those notes are in that part")

    return propose(project, {
        "description": "Set %d note(s) to velocity %d" % (len(chosen), velocity),
        "pattern": pattern, "channel": channel,
        "allowed_notes": sorted(wanted),
        "keeps": {"rhythm": True, "pitch": True},
        "notes": [{"what": "change", "id": n["id"], "velocity": velocity} for n in chosen]},
        apply, "tidy-velocity")


# --- 4. review an insert -------------------------------------------------------------

def review_insert(project, insert, **_):
    """Reads a mixer insert and says which of it could not be read. A plugin's own
    saved state is opaque from out here, and a review that quietly omitted that would
    read as a complete account of something it had only half seen."""
    detail = call(project, "inspect_insert", {"insert": insert})

    return {"recipe": "review-insert",
            "read": detail,
            "opaque": detail.get("opaque_state", ""),
            "changed": False}


# --- 5. move a clip ------------------------------------------------------------------

def move_clip(project, **_):
    """Not available. The contract lists it, this build cannot propose it, and saying
    so is the whole behaviour - a recipe that pretended would be worse than none."""
    capabilities = call(project, "get_capabilities", {})
    return {"recipe": "move-clip",
            "status": "UNSUPPORTED",
            "message": "Moving a clip is not a kind of change this build can propose",
            "writes_this_build_supports": capabilities["writes"],
            "changed": False}


# --- the shared tail: propose, show, and only then perhaps apply ---------------------

def propose(project, arguments, apply, name):
    arguments["base_revision"] = revision(project)
    made = call(project, "create_proposal", arguments)

    result = {"recipe": name, "proposal": made["proposal"], "diff": made["diff"],
              "changed": False}

    if not apply:
        return result

    applied = call(project, "apply_proposal", {"proposal": made["proposal"]["id"]})
    result["changed"] = not applied["already_applied"]
    result["undo_description"] = applied.get("undo_description", "")
    return result


RECIPES = {"diagnose-region": diagnose_region,
           "rewrite-melody": rewrite_melody,
           "tidy-velocity": tidy_velocity,
           "review-insert": review_insert,
           "move-clip": move_clip}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("recipe", choices=sorted(RECIPES))
    parser.add_argument("--project", required=True)
    parser.add_argument("--start-beat", type=float, default=0.0)
    parser.add_argument("--end-beat", type=float, default=16.0)
    parser.add_argument("--pattern")
    parser.add_argument("--channel")
    parser.add_argument("--insert")
    parser.add_argument("--clip")
    parser.add_argument("--to-beat", type=float)
    parser.add_argument("--notes", default="",
                        help="comma separated note ids, from inspect_pattern")
    parser.add_argument("--semitones", type=int, default=2)
    parser.add_argument("--velocity", type=int, default=96)
    parser.add_argument("--apply", action="store_true",
                        help="go through with the change; without it, nothing is edited")
    args = parser.parse_args()

    try:
        answer = RECIPES[args.recipe](args.project,
                                      start_beat=args.start_beat, end_beat=args.end_beat,
                                      pattern=args.pattern, channel=args.channel,
                                      insert=args.insert, clip=args.clip,
                                      notes=[n for n in args.notes.split(",") if n],
                                      semitones=args.semitones, velocity=args.velocity,
                                      apply=args.apply)
    except Refused as refusal:
        print(json.dumps({"recipe": args.recipe, "refused": refusal.code,
                          "message": refusal.message, "changed": False}, indent=2))
        return 1

    print(json.dumps(answer, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
