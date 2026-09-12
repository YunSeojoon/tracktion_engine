"""A4's first piece: what is known about a project, kept apart by who said it.

Three things get called "the notes", and collapsing them is how an assistant starts
building confidently on something nobody agreed to. A person's condition is a rule. The
assistant's guess is a reading that may be right and is still a guess. The current
request is neither and expires with the answer.

The rule these checks exist for is the one that is easy to lose: a guess never becomes
a rule on its own. An assistant that reads "this is in D minor", is later shown its own
note, and treats it as an instruction, will insist on a key nobody chose.

    python tools/test_project_notes.py
"""
import argparse
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import apply_change, read, tool, wait_for
from test_plugin_compatibility import Session, prepare_song

ROOT = Path(__file__).resolve().parents[1]


class Report:
    def __init__(self):
        self.failures = []

    def expect(self, name, condition, detail=''):
        print(('  ok   ' if condition else '  FAIL ') + name + (('  ' + str(detail)) if detail else ''))
        if not condition:
            self.failures.append(name)


def notes_of(folder):
    return read(folder / "chat-inspector.json").get("notes", {}).get("notes", [])


def of_kind(folder, kind):
    return [n for n in notes_of(folder) if n["kind"] == kind]


def check_a_guess_never_becomes_a_rule_on_its_own(exe, folder, report):
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        session.settled()
        revision = read(folder / "sync-status.json")["revision"]

        session.run([{"project_note": ["condition", "keep the drums as they are"]},
                     {"project_note": ["guess", "this sounds like D minor"]},
                     {"project_note": ["request", "make the bass busier"]}])
        time.sleep(0.6)

        report.expect("a person's decision is recorded as decided",
                      [n["text"] for n in of_kind(folder, "condition")]
                      == ["keep the drums as they are"],
                      of_kind(folder, "condition"))
        report.expect("the assistant's reading is recorded as a guess",
                      [n["text"] for n in of_kind(folder, "guess")] == ["this sounds like D minor"])
        report.expect("a guess says which music it was read from",
                      of_kind(folder, "guess")[0]["about_revision"] == revision,
                      (of_kind(folder, "guess")[0]["about_revision"], revision))
        report.expect("and it is not a condition",
                      len(of_kind(folder, "condition")) == 1)

        # Only a person turns a guess into a rule, and the guess stays where it was.
        guess_id = of_kind(folder, "guess")[0]["id"]
        session.run([{"project_note": ["accept", guess_id]}])
        time.sleep(0.5)

        agreed = of_kind(folder, "condition")
        report.expect("accepting a guess makes a condition",
                      any(n["text"] == "this sounds like D minor" for n in agreed), agreed)
        report.expect("and says which guess it came from",
                      any(n["promoted_from"] == guess_id for n in agreed))
        report.expect("the guess is still there, as a guess",
                      len(of_kind(folder, "guess")) == 1)

        report.expect("writing notes changed no music",
                      read(folder / "sync-status.json")["revision"] == revision,
                      read(folder / "sync-status.json")["revision"])
    finally:
        session.close()


def check_notes_outlive_the_app_but_not_the_request(exe, folder, report):
    """A condition is a decision and survives; a request is about a moment and does not."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        session.settled()
        session.run([{"project_note": ["condition", "stay in 3/4"]},
                     {"project_note": ["guess", "the chorus starts at bar 17"]},
                     {"project_note": ["request", "try something for the bridge"]}])
        time.sleep(0.6)
        report.expect("there is a request while it is being asked",
                      len(of_kind(folder, "request")) == 1)
    finally:
        session.close()

    session = Session(exe, folder).open()
    try:
        session.settled()
        time.sleep(0.8)
        report.expect("a decision survives closing the app",
                      any(n["text"] == "stay in 3/4" for n in of_kind(folder, "condition")),
                      of_kind(folder, "condition"))
        report.expect("so does a guess, still marked as one",
                      any(n["text"] == "the chorus starts at bar 17" for n in of_kind(folder, "guess")))
        report.expect("a finished request does not come back",
                      not of_kind(folder, "request"), of_kind(folder, "request"))
    finally:
        session.close()


def check_notes_do_not_move_between_projects(exe, folder, report):
    """Notes belong to a project id. A copied folder is a different project and must not
    inherit decisions made about the original."""
    folder.mkdir(parents=True, exist_ok=True)
    session = Session(exe, folder).open()
    try:
        prepare_song(session)
        session.settled()
        session.run([{"project_note": ["condition", "the outro fades"]}])
        time.sleep(0.5)
        report.expect("the note is here", len(of_kind(folder, "condition")) == 1)
    finally:
        session.close()

    # The same notes file, claimed by a project that is not the one it was written for.
    copied = folder.parent / (folder.name + "-copy")
    copied.mkdir(parents=True, exist_ok=True)
    import shutil
    for name in ("project.json", "notes.json"):
        if (folder / name).exists():
            shutil.copyfile(folder / name, copied / name)

    session = Session(exe, copied).open()
    try:
        session.settled()
        time.sleep(0.8)
        # A fresh project id means the notes file is not this project's.
        report.expect("a copied project does not inherit the original's decisions",
                      not of_kind(copied, "condition"), of_kind(copied, "condition"))
    finally:
        session.close()


def check_the_model_is_told_which_is_which(report):
    """The prompt has to keep them apart, or a model reads its own guess as an
    instruction it was given."""
    from cocompose_bridge import build_prompt

    prompt = build_prompt({
        "revision": 5,
        "message": "make the bridge less busy",
        "attachments": [],
        "history": {"messages": []},
        "notes": {"conditions": [{"id": "c1", "text": "keep the drums as they are"}],
                  "guesses": [{"id": "g1", "text": "this sounds like D minor",
                               "about_revision": 4}],
                  "request": "make the bridge less busy"}})

    report.expect("what the person decided is marked as decided",
                  "DECIDED by the person: keep the drums as they are" in prompt)
    report.expect("what the assistant guessed is marked as a guess",
                  "guessed by you earlier, not agreed" in prompt)
    report.expect("and the model is told a guess is not a rule",
                  "A guess is not a rule" in prompt)
    report.expect("a guess carries the music it was read from", "revision 4" in prompt)


def check_asking_for_more_does_not_grant_more(exe, folder, report):
    """A4: how far to go is a request, not a permission.

    Asking for something bolder says what would be welcome. It says nothing about what
    may be touched, and the app must not read it as though it did - a person who picks
    eight notes and asks for something adventurous is asking for an adventurous eight
    notes. This is the check that would catch a future change wiring strength into
    scope, which is the mistake the spec names."""
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
        notes = [n["id"] for p in part["parts"] for n in p["notes"]]
        inside, outside = notes[:4], notes[4]

        for strength in ("tidy", "rework", "fresh"):
            session.run([{"project_note": ["strength", strength]}])
            time.sleep(0.4)
            report.expect("the app holds the strength it was set to: " + strength,
                          read(folder / "chat-inspector.json")["notes"]["strength"] == strength,
                          read(folder / "chat-inspector.json")["notes"]["strength"])

            revision = read(folder / "sync-status.json")["revision"]

            reaching = tool(project, "create_proposal", {
                "description": "beyond what was attached",
                "pattern": pattern_id, "channel": channel_id,
                "allowed_notes": inside, "base_revision": revision, "keeps": {},
                "notes": [{"what": "change", "id": outside, "pitch": 64}]})
            report.expect("at " + strength + ", a note outside the selection is still refused",
                          reaching["status"] == "error"
                          and reaching["error"]["code"] == "OUT_OF_SCOPE",
                          reaching.get("error", {}).get("code", reaching["status"]))

            breaking = tool(project, "create_proposal", {
                "description": "breaks a promise",
                "pattern": pattern_id, "channel": channel_id,
                "allowed_notes": inside, "base_revision": revision,
                "keeps": {"pitch": True},
                "notes": [{"what": "change", "id": inside[0], "pitch": 70}]})
            report.expect("at " + strength + ", a kept condition is still kept",
                          breaking["status"] == "error"
                          and breaking["error"]["code"] == "LOCKED",
                          breaking.get("error", {}).get("code", breaking["status"]))

            report.expect("and none of that touched the music",
                          read(folder / "sync-status.json")["revision"] == revision)
    finally:
        session.close()


def check_the_model_is_told_taste_not_reach(report):
    """The sentence the model gets has to be about taste, or a model reads a bolder
    setting as a wider licence."""
    from cocompose_bridge import build_prompt

    prompt = build_prompt({
        "revision": 5, "message": "surprise me", "attachments": [],
        "history": {"messages": []},
        "notes": {"conditions": [], "guesses": [], "request": "surprise me",
                  "strength": "fresh",
                  "strength_means": "They want a new idea rather than a variation of this "
                                    "one. Start from what the music is doing rather than "
                                    "from these exact notes - still only within what they "
                                    "attached and whatever they asked you to keep."}})

    report.expect("the model is told how far they want it to go",
                  "How far they want you to go" in prompt)
    report.expect("and told plainly that this is not permission",
                  "not about what you may touch" in prompt)


def check_a_note_about_a_place_keeps_its_place(exe, folder, report):
    """B3: a note pinned to a stretch, and a note tied to a clip, when the music moves.

    Both answers are right for different notes. "The drop is too early" is about a
    moment in the arrangement and stays there when a clip is dragged past it. "Rewrite
    this fill" is about a clip and follows it. Guessing which is meant is how a note
    ends up pointing at the wrong bar.

    And a clip can be deleted. A note that followed it then points at nothing, and the
    honest thing is to say so - not to re-aim it at whatever is nearest, and not to go
    on showing the old place as though the clip were still in it."""
    folder.mkdir(parents=True, exist_ok=True)
    project = folder / "project.json"
    session = Session(exe, folder).open()

    try:
        prepare_song(session)
        state = session.settled()
        clip = state["playlist"]["clips"][0]
        report.expect("there is a clip to tie a note to", clip is not None)

        session.run([{"project_note": ["todo_at", "the drop is too early", 16.0, 24.0]},
                     {"project_note": ["todo_on", "rewrite this fill", clip["id"]]}])
        time.sleep(0.6)

        todos = of_kind(folder, "todo")
        report.expect("both notes were taken", len(todos) == 2, [t["text"] for t in todos])
        if len(todos) != 2:
            return

        pinned = next(t for t in todos if t["anchor"] == "time")
        tied = next(t for t in todos if t["anchor"] == "clip")
        report.expect("the pinned one is where it was put",
                      (pinned["from_beat"], pinned["to_beat"]) == (16.0, 24.0),
                      (pinned["from_beat"], pinned["to_beat"]))
        report.expect("and the tied one starts where its clip is",
                      tied["from_beat"] == clip["start"], (tied["from_beat"], clip["start"]))

        def shift(live):
            for placement in live["playlist"]["clips"]:
                if placement["id"] == clip["id"]:
                    placement["start"] = clip["start"] + 32.0

        apply_change(project, shift)
        time.sleep(1.5)
        session.settled()

        moved = of_kind(folder, "todo")
        after_tied = next(t for t in moved if t["id"] == tied["id"])
        after_pinned = next(t for t in moved if t["id"] == pinned["id"])
        report.expect("the tied note went with its clip",
                      after_tied["from_beat"] == clip["start"] + 32.0,
                      (after_tied["from_beat"], clip["start"] + 32.0))
        report.expect("and the pinned one stayed where it was put",
                      (after_pinned["from_beat"], after_pinned["to_beat"]) == (16.0, 24.0),
                      (after_pinned["from_beat"], after_pinned["to_beat"]))

        def remove(live):
            live["playlist"]["clips"] = [p for p in live["playlist"]["clips"]
                                         if p["id"] != clip["id"]]

        apply_change(project, remove)
        time.sleep(1.5)
        session.settled()

        orphaned = next(t for t in of_kind(folder, "todo") if t["id"] == tied["id"])
        report.expect("a note whose clip was deleted says so",
                      orphaned["clip"].startswith("gone:"), orphaned["clip"])
        report.expect("and keeps where the clip used to be, rather than moving somewhere "
                      "nobody put it",
                      orphaned["from_beat"] == clip["start"] + 32.0, orphaned["from_beat"])
        report.expect("the pinned note is untouched by any of it",
                      next(t for t in of_kind(folder, "todo")
                           if t["id"] == pinned["id"])["from_beat"] == 16.0)

        session.run([{"project_note": ["done", pinned["id"]]}])
        time.sleep(0.6)
        finished = next(t for t in of_kind(folder, "todo") if t["id"] == pinned["id"])
        report.expect("a todo marked done stays on the list", finished["done"] is True)
    finally:
        session.close()


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    report = Report()

    print("a guess is not a rule")
    check_a_guess_never_becomes_a_rule_on_its_own(exe, output / "kinds", report)
    print()
    print("what survives a restart")
    check_notes_outlive_the_app_but_not_the_request(exe, output / "restart", report)
    print()
    print("notes belong to one project")
    check_notes_do_not_move_between_projects(exe, output / "project", report)
    print()
    print("asking for more does not grant more")
    check_asking_for_more_does_not_grant_more(exe, output / "strength", report)
    print()
    print("the model is told taste, not reach")
    check_the_model_is_told_taste_not_reach(report)
    print()
    print("the model is told which is which")
    check_the_model_is_told_which_is_which(report)
    print()
    print("a note about a place keeps its place")
    check_a_note_about_a_place_keeps_its_place(exe, output / "places", report)

    print()
    print("FAILURES:", report.failures if report.failures else "none")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path,
                        default=ROOT / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build-cocompose" / ("notes-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    sys.exit(1 if run(args.exe.resolve(), args.output.resolve()).failures else 0)
