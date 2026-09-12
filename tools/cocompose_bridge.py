"""Answers CoCompose's questions by talking to a model.

The app never talks to a provider itself. It writes a question next to the project and
this program picks it up, asks whoever it is configured to ask, and writes the answer
back. Everything that makes that awkward - a key, a network, a slow reply, a provider's
own idea of how a request should look - lives here, outside the app.

Which means, in order of how much it matters:

  the key is never in the app, the project, the conversation or a log, because it was
  never in that process;

  changing provider is changing this program, and the app does not know the difference;

  and a reply that is slow or never arrives cannot hold up the music, because it is not
  happening in the app at all.

Run it beside a project while the app is open:

    set OPENAI_API_KEY=...
    python tools/cocompose_bridge.py --project "C:\\...\\project.json"

    python tools/cocompose_bridge.py --project ... --provider echo
        answers without a network, for checking the plumbing. It says so in every
        answer, and the app records which provider answered, so an echo run can never
        be mistaken for a real one.
"""
import argparse
import json
import os
import re
from pathlib import Path
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import atomic_write


def read(path):
    """Whatever is in the file, or nothing. A file that is not there yet, or is halfway
    through being written, is not an error here - it is the normal state of waiting."""
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (FileNotFoundError, json.JSONDecodeError, PermissionError, OSError):
        return {}


SYSTEM_PROMPT = """You are helping someone compose music in a DAW called CoCompose.

You are given exactly what they selected and attached, read out of the project: bars of
an arrangement, notes of one channel's part of a pattern, or one mixer insert. Beats are
quarter notes counted from the start; bars are what the person sees.

Answer about what was attached. You may use the surrounding context that came with it to
understand, but say clearly when you are talking about something outside what they
picked.

You cannot hear anything. No audio has been sent to you, so never say how something
sounds - say what the structure, the notes, the levels or the routing are, and say when
a question can only be settled by listening.

You cannot change the project yourself, but you may propose a change, and the person
decides. Never say you have made one - you have not, until they press Apply.

To propose, say what you would do in your own words as usual, and then put one fenced
block at the very end, exactly like this and nothing after it:

```cocompose-change
{"description": "Move the melody up a tone",
 "keeps": {"rhythm": true, "velocity": true},
 "notes": [{"what": "change", "id": "<the id given with the note>", "pitch": 62}]}
```

Rules for that block, all of them enforced by the app - a block that breaks one is
refused and the person sees nothing:

- Only notes that were attached. Every id must be one printed with the attachment.
- "what" is "change", "add" or "remove". For "change", give only the fields you are
  changing: "pitch" (0-127), "velocity" (1-127), "start_beat", "length_beats".
- "keeps" is what you promise not to disturb: "rhythm", "velocity", "pitch". The app
  measures the result and refuses the whole thing if a promise was broken, so promise
  only what you mean.
- To change a mixer parameter instead, use "parameters": [{"owner": "<insert id>",
  "plugin": "<effect id>", "parameter": "<parameter id>", "value": 0.0-1.0}], all three
  ids copied from the attached insert, and the value normalised from 0 to 1. Only an
  insert that was attached may be touched.
- Omit the block entirely if they asked a question rather than for a change, or if you
  are unsure. No block is a fine answer; a guessed one is not.

Refer to things the way the person sees them: bar numbers, channel names, note pitches.
Be brief and concrete. If the attachment is empty or no longer in the song, say so."""


def describe_clip(lines, clip, indent, context=False):
    """A clip, and the notes in it.

    Naming a clip and its length says nothing about the music: two clips both called
    "Pattern 2" and both sixteen beats can be a bass line and a cluster chord. Being
    asked what is wrong with a stretch of song and handed only the labels is being asked
    to guess, and a guess dressed as a diagnosis is the worst thing this can produce.
    """
    lines.append("%sclip '%s' at beat %.3f for %.3f beats%s"
                 % (indent, clip.get("pattern_name", ""), clip["start_beat"],
                    clip["length_beats"], "   (context, not what was asked about)" if context else ""))

    for part in clip.get("parts", []):
        lines.append("%s  channel '%s' playing %s"
                     % (indent, part.get("channel_name", ""), part.get("instrument", "")))
        for note in part.get("notes", []):
            lines.append("%s    pitch %d  beat %.3f  length %.3f  velocity %d"
                         % (indent, note["pitch"], note["start_beat"],
                            note["length_beats"], note["velocity"]))

    if clip.get("notes_omitted"):
        lines.append("%s  (%d more note(s) in this clip, not shown)"
                     % (indent, clip["notes_omitted"]))


def describe_attachments(attachments):
    """Turns the packet into something a model reads as music rather than as JSON."""
    if not attachments:
        return "Nothing was attached to this question."

    lines = []
    for a in attachments:
        lines.append("--- attached: %s (%s)" % (a.get("summary", ""), a.get("kind", "")))
        if not a.get("still_there", True):
            lines.append("    NOTE: what this referred to is no longer in the song.")

        detail = (a.get("detail") or {}).get("result")
        if not detail:
            error = (a.get("detail") or {}).get("error", {})
            lines.append("    could not be read: %s" % error.get("message", "unknown"))
            continue

        if a["kind"] == "region":
            lines.append("    beats %.3f to %.3f (%s), tempo %.2f"
                         % (detail["start_beat"], detail["end_beat"],
                            detail.get("bars", ""), detail.get("tempo", 0.0)))

            for clip in detail.get("clips", []):
                describe_clip(lines, clip, "    ")

            for clip in detail.get("context_clips", []):
                describe_clip(lines, clip, "    ", context=True)

            if detail.get("notes_left_out"):
                lines.append("    NOTE: this region holds more notes than fit in one"
                             " question. Ask about a shorter stretch to see the rest.")

        elif a["kind"] == "notes":
            lines.append("    pattern '%s', %d beats long, placed %d time(s)%s"
                         % (detail.get("name", ""), detail.get("length_beats", 0),
                            detail.get("placement_count", 0),
                            " - changing it changes all of them" if detail.get("shared") else ""))
            for part in detail.get("parts", []):
                lines.append("    channel '%s' playing %s"
                             % (part.get("channel_name", ""), part.get("instrument", "")))
                allowed = set(a.get("notes_allowed") or [])
                for note in part.get("notes", []):
                    # The id is here because a proposal has to name the note it moves,
                    # and the app only accepts ids it handed out. The whole part is shown
                    # for context; the mark says which of it may actually be changed.
                    lines.append("      pitch %d  beat %.3f  length %.3f  velocity %d  id %s%s"
                                 % (note["pitch"], note["start_beat"],
                                    note["length_beats"], note["velocity"], note["id"],
                                    "  <- may be changed" if note["id"] in allowed else ""))
                if allowed:
                    shown = [n["id"] for n in part.get("notes", []) if n["id"] in allowed]
                    lines.append("    CHANGEABLE NOTE IDS - a change may name these and"
                                 " nothing else: " + ", ".join(shown))
                    lines.append("    every other note above is context. Naming one gets"
                                 " the whole change refused and the person sees nothing.")

        elif a["kind"] == "insert":
            lines.append("    insert %s '%s', gain %.2f dB, pan %.2f, out to %s"
                         % (detail.get("index"), detail.get("name", ""),
                            detail.get("gain_db", 0.0), detail.get("pan", 0.0),
                            detail.get("output", "")))
            if not detail.get("effects"):
                lines.append("    no effects on this insert")
            for effect in detail.get("effects", []):
                lines.append("      %s%s, wet %.2f, %d public parameter(s)"
                             % (effect.get("name", effect.get("type", "")),
                                " (bypassed)" if effect.get("bypass") else "",
                                effect.get("wet", 1.0), len(effect.get("parameters", []))))
                shown = effect.get("parameters", [])[:12]
                for parameter in shown:
                    lines.append("        %s = %.3f" % (parameter.get("name", parameter["id"]),
                                                        parameter["value"]))
                rest = len(effect.get("parameters", [])) - len(shown)
                if rest > 0:
                    lines.append("        (%d more parameter(s) on this effect, not shown"
                                 " - ask about this insert again to see them named)" % rest)
            # A send is why something is audible in a place it was never put, so an
            # account of an insert that leaves them out can be confidently wrong.
            for send in detail.get("sends", []):
                lines.append("    sends a copy to %s at %.2f dB"
                             % (send.get("target", "?"), send.get("level_db", 0.0)))
            if not detail.get("sends"):
                lines.append("    no sends from this insert")

            lines.append("    " + detail.get("opaque_state", ""))
            for channel in detail.get("fed_by", []):
                lines.append("    fed by channel '%s'" % channel.get("name", ""))

    return "\n".join(lines)


def describe_earlier_attachment(a, now_revision):
    """One line for something an earlier question was about.

    A follow-up like "make that part less busy" means the thing attached two messages
    ago, and a history that mentioned only the words left the model guessing. What it
    gets is what was frozen then - and, when the music has moved since, that it has:
    the target is described as it was, never asserted to be that way still.
    """
    where = {"region": "bars around beats %.3f to %.3f" % (a.get("start_beat", 0.0),
                                                           a.get("end_beat", 0.0)),
             "notes": "%d note(s) of pattern %s on channel %s" % (a.get("note_count", 0),
                                                                  a.get("pattern", "?"),
                                                                  a.get("channel", "?")),
             "insert": "mixer insert %s" % a.get("insert", "?")}.get(a.get("kind"), a.get("kind", "?"))

    taken = a.get("taken_at_revision")
    moved = ("" if taken is None or now_revision is None or taken == now_revision
             else "  (read at revision %s; the project is now at %s, so it may have changed)"
                  % (taken, now_revision))
    return "    was about: " + where + moved


def build_prompt(request):
    story = request.get("history", {}) or {}
    history = story.get("messages", [])
    now_revision = request.get("revision")
    parts = []

    if len(history) > 1:
        parts.append("Earlier in this conversation about the same project:")
        for message in history[:-1]:
            text = (message.get("text") or "").strip()
            if text:
                parts.append("  %s: %s%s" % (message["from"], text,
                                             " [...cut]" if message.get("text_was_cut") else ""))
            for a in message.get("attachments") or []:
                parts.append(describe_earlier_attachment(a, now_revision))

        if story.get("trimmed"):
            parts.append("  (older messages than these exist and are not shown)")
        if story.get("any_text_cut"):
            parts.append("  (a message above was cut; ask rather than assume what it said)")
        parts.append("")

    known = request.get("notes") or {}

    if known.get("conditions") or known.get("guesses"):
        parts.append("What is known about this project:")

        for condition in known.get("conditions", []):
            parts.append("  DECIDED by the person: " + condition["text"])

        for guess in known.get("guesses", []):
            parts.append("  guessed by you earlier, not agreed: %s  (read at revision %s)"
                         % (guess["text"], guess.get("about_revision")))

        if known.get("guesses"):
            parts.append("  A guess is not a rule. Do not treat one as something you were"
                         " told, and say so if you are relying on it.")
        parts.append("")

    parts.append(describe_attachments(request.get("attachments", [])))
    parts.append("")
    parts.append("Their question: " + request.get("message", ""))
    return "\n".join(parts)


def suggested_change(request):
    """A change the app can check and offer, worked out without a model.

    Its only job is to exercise the path a real answer takes: the app puts whatever a
    bridge sends through the same scope and keeps checks either way, so a suggestion
    from here is treated exactly as suspiciously as one from a model.

    It moves the attached notes up a tone and leaves the rhythm alone - a change that is
    obviously not musical judgement, which is the point.
    """
    for attachment in request.get("attachments", []):
        if attachment.get("kind") != "notes":
            continue

        detail = (attachment.get("detail") or {}).get("result")
        if not detail:
            continue

        allowed = set(attachment.get("notes_allowed") or [])

        for part in detail.get("parts", []):
            notes = [n for n in part.get("notes", []) if not allowed or n["id"] in allowed][:4]
            if not notes:
                continue

            return {
                "description": "Move the attached notes up a tone",
                "keeps": {"rhythm": True, "velocity": True},
                "notes": [{"what": "change", "id": note["id"],
                           "pitch": min(127, note["pitch"] + 2)} for note in notes],
            }

    return None


CHANGE_OPENS = re.compile(r"```[ \t]*cocompose-change[ \t]*\r?\n", re.IGNORECASE)


def first_object(text):
    """Reads one JSON object off the front of `text`, returning it and what follows.

    Braces are counted rather than fences, and a string is skipped over so that a brace
    inside one does not end the object early. Counting braces is what makes a block that
    was never closed still readable - see parse_change.
    """
    depth = 0
    inside_string = False
    escaped = False

    for i, character in enumerate(text):
        if inside_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                inside_string = False
        elif character == '"':
            inside_string = True
        elif character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return text[:i + 1], text[i + 1:]

    return None, text


def parse_change(text):
    """Pulls a proposed change out of an answer, and says what is left to read.

    A model writes prose and, if it wants to change something, one fenced block. The
    block is lifted out so the person reads the sentence rather than the JSON, and
    passed on untouched - the app checks it against the attachment, the promises it
    makes and the current revision, exactly as it checks the echo bridge's. Nothing
    here decides whether a change is allowed; it only decides whether one was asked for.

    The closing fence is not required. Models open the block, write a complete object,
    and run out of breath before the last three backticks; insisting on them would throw
    away a perfectly good change over punctuation. What has to be complete is the object.

    Anything malformed is still no change at all. A half-understood edit to someone's
    music is worse than an answer with no button on it.
    """
    opened = CHANGE_OPENS.search(text or "")
    if not opened:
        return text, None

    body = text[opened.end():].lstrip()
    raw, rest = first_object(body)
    if raw is None:
        return text, None

    try:
        change = json.loads(raw)
    except ValueError:
        return text, None

    if not isinstance(change, dict) or not (change.get("notes") or change.get("parameters")):
        return text, None

    rest = rest.lstrip()
    if rest.startswith("```"):
        rest = rest[3:]

    return (text[:opened.start()] + rest).strip(), change


class Echo:
    """No network. Says what it was given, and says that it is not a model."""

    name = "echo (no model, plumbing only)"

    def answer(self, request, on_text):
        # The whole prompt, not a summary of it: the point of this bridge is to show
        # exactly what would have gone to a model, the earlier conversation included.
        reply = ("This is the echo bridge, not a model - no question was sent anywhere.\n\n"
                 "What the app would have sent:\n\n" + build_prompt(request))

        for i in range(0, len(reply), 180):
            on_text(reply[:i + 180])
            time.sleep(0.05)
        return reply


class OpenAIChat:
    """One provider, over plain HTTPS. No SDK, so nothing is hidden behind a wrapper."""

    def __init__(self, model, key):
        self.model = model
        self.key = key
        self.name = "openai " + model

    def answer(self, request, on_text):
        body = json.dumps({
            "model": self.model,
            "messages": [{"role": "system", "content": SYSTEM_PROMPT},
                         {"role": "user", "content": build_prompt(request)}],
        }).encode("utf-8")

        call = urllib.request.Request(
            "https://api.openai.com/v1/chat/completions",
            data=body,
            headers={"Authorization": "Bearer " + self.key,
                     "Content-Type": "application/json"},
            method="POST")

        with urllib.request.urlopen(call, timeout=120) as response:
            payload = json.loads(response.read().decode("utf-8"))

        text = payload["choices"][0]["message"]["content"]
        on_text(text)
        return text


class Ollama:
    """A model running on this machine, over Ollama's HTTP API.

    Same contract as the hosted provider and the same prompt; the differences are that
    there is no key to hold and nothing leaves the machine. That last part is why it is
    here. A person can have the whole thing working - attach, ask, read, propose, apply -
    and decide afterwards whether they want their music sent to anybody at all.

    It streams, because a local model on ordinary hardware is slow enough that watching
    the answer appear is the difference between working and hung.
    """

    def __init__(self, model, host):
        self.model = model
        self.host = host.rstrip("/")
        self.name = "ollama " + model

    def answer(self, request, on_text):
        body = json.dumps({
            "model": self.model,
            "stream": True,
            "messages": [{"role": "system", "content": SYSTEM_PROMPT},
                         {"role": "user", "content": build_prompt(request)}],
            # A suggested change is a fenced block of JSON, and a model left to be
            # inventive gets the brackets wrong. This is not about taste.
            "options": {"temperature": 0.2}}).encode("utf-8")

        call = urllib.request.Request(self.host + "/api/chat", data=body,
                                      headers={"Content-Type": "application/json"},
                                      method="POST")

        whole = ""
        with urllib.request.urlopen(call, timeout=900) as response:
            for line in response:
                line = line.strip()
                if not line:
                    continue

                # One JSON object per line. Anything unreadable is a line that has not
                # finished arriving, not a failure; the next read brings the rest.
                try:
                    piece = json.loads(line.decode("utf-8"))
                except ValueError:
                    continue

                if piece.get("error"):
                    raise RuntimeError(piece["error"])

                whole += piece.get("message", {}).get("content", "")
                on_text(whole)

                if piece.get("done"):
                    break

        return whole


def make_provider(name, model, key_name, host="http://localhost:11434"):
    if name == "echo":
        return Echo()

    # No key: the model is on this machine. Asking for one would be theatre.
    if name == "ollama":
        return Ollama(model or "llama3.1:8b", host)

    key = os.environ.get(key_name, "")
    if not key:
        raise SystemExit("No %s in the environment. Set it, or use --provider echo." % key_name)

    if name == "openai":
        return OpenAIChat(model or "gpt-4o-mini", key)

    raise SystemExit("Unknown provider: " + name)


HEARTBEAT_SECONDS = 2.0


class Liveness:
    """Says, continuously, that this bridge is still here.

    Writing "ready" once at startup is a promise about the past. A bridge killed with
    the window close button, or with the machine, never gets to take it back - the file
    stays, the app believes it, and a question sent afterwards waits for something that
    is not there any more.

    So it is written again every couple of seconds, including while a provider call is
    in flight, which is exactly when a bridge looks most like a dead one. The app treats
    a heartbeat that has stopped as a bridge that has stopped.
    """

    def __init__(self, state_file, name):
        self.state_file = state_file
        self.name = name
        self.instance = str(uuid.uuid4())
        self.busy_with = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._beat, daemon=True)

    def _write(self, ready=True):
        atomic_write(self.state_file, {
            "ready": ready,
            "name": self.name,
            "instance": self.instance,
            "heartbeat_ms": int(time.time() * 1000),
            "heartbeat_interval_ms": int(HEARTBEAT_SECONDS * 1000),
            "busy_with": self.busy_with})

    def _beat(self):
        while not self.stop.wait(HEARTBEAT_SECONDS):
            self._write()

    def __enter__(self):
        self._write()
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        # A clean exit says so outright, so the app does not have to wait out a timeout
        # to learn what it could have been told.
        self._write(ready=False)
        return False


def already_dealt_with(reply_file, request_id):
    """Whether this question has already been answered, and what to do if it was cut off.

    The bridge's memory of what it has answered used to live in a set, which is to say
    it lived until the process did. Restarting it in a folder with a question still
    sitting there sent that question to the provider a second time - a second bill, a
    second answer replacing the first, and with write tools a second edit.

    The reply beside the request is the record, and it outlives the process. Returns
    "done" when the question has a final answer, "interrupted" when an answer had begun
    and never finished, and None when it is genuinely new.
    """
    reply = read(reply_file)
    if reply.get("request_id") != request_id:
        return None
    if reply.get("status") in ("ok", "error"):
        return "done"
    return "interrupted"


def serve(project, provider, once=False):
    folder = Path(project).parent
    folder.mkdir(parents=True, exist_ok=True)

    request_file = folder / "chat-request.json"
    reply_file = folder / "chat-reply.json"
    cancel_file = folder / "chat-cancel.json"
    state_file = folder / "chat-bridge.json"

    answered = set()
    alive = Liveness(state_file, provider.name)

    with alive:
        print("bridge ready:", provider.name)
        print("watching", request_file)

        while True:
            request = read(request_file)
            request_id = request.get("request_id")

            if not request_id or request_id in answered:
                if once and answered:
                    return
                time.sleep(0.2)
                continue

            standing = already_dealt_with(reply_file, request_id)

            if standing == "done":
                # Answered by a previous run of this bridge. Left alone.
                answered.add(request_id)
                print("already answered:", request_id[:8])
                if once:
                    return
                continue

            if standing == "interrupted":
                # An answer had started and the bridge went away mid-sentence. Whether
                # the provider did the work - and charged for it - is unknowable from
                # here, so it is not quietly asked again. The person is told, and asking
                # again is their decision.
                answered.add(request_id)
                atomic_write(reply_file, {
                    "request_id": request_id, "status": "error", "code": "IO_ERROR",
                    "message": "The bridge stopped while this answer was arriving. "
                               "Nothing was changed. Ask again if you want to.",
                    "retryable": True, "provider": provider.name})
                print("interrupted earlier, not resent:", request_id[:8])
                if once:
                    return
                continue

            answered.add(request_id)
            alive.busy_with = request_id
            print("question:", (request.get("message") or "")[:70])

            def stream(so_far, _id=request_id):
                if read(cancel_file).get("request_id") == _id:
                    raise KeyboardInterrupt
                atomic_write(reply_file, {"request_id": _id, "status": "streaming",
                                          "text": so_far, "provider": provider.name})

            try:
                text = provider.answer(request, stream)

                # The echo bridge has no model to ask, so it works one out mechanically;
                # a real provider's comes from what it actually wrote. Either way the
                # app is the one that decides whether it is allowed.
                if isinstance(provider, Echo):
                    change = suggested_change(request)
                else:
                    text, change = parse_change(text)

                reply = {"request_id": request_id, "status": "ok",
                         "text": text, "provider": provider.name}
                if change:
                    reply["change"] = change

                atomic_write(reply_file, reply)
                print("answered", len(text), "characters")
            except KeyboardInterrupt:
                atomic_write(reply_file, {"request_id": request_id, "status": "error",
                                          "code": "CANCELLED", "message": "cancelled",
                                          "retryable": True, "provider": provider.name})
                print("cancelled")
            except urllib.error.HTTPError as error:
                detail = error.read().decode("utf-8", "replace")[:300]
                code = ("LOCKED" if error.code in (401, 403)
                        else "IO_ERROR" if error.code == 429 else "IO_ERROR")
                atomic_write(reply_file, {"request_id": request_id, "status": "error",
                                          "code": code,
                                          "message": "HTTP %d: %s" % (error.code, detail),
                                          "retryable": error.code in (429, 500, 502, 503),
                                          "provider": provider.name})
                print("failed: HTTP", error.code)
            except (urllib.error.URLError, TimeoutError, OSError) as error:
                atomic_write(reply_file, {"request_id": request_id, "status": "error",
                                          "code": "IO_ERROR", "message": str(error),
                                          "retryable": True, "provider": provider.name})
                print("failed:", error)

            alive.busy_with = None

            if once:
                return


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--provider", default="echo", choices=["echo", "openai", "ollama"])
    parser.add_argument("--model", default=None)
    parser.add_argument("--key-name", default="OPENAI_API_KEY",
                        help="environment variable holding the key; never read from the project")
    parser.add_argument("--host", default="http://localhost:11434",
                        help="where ollama is listening")
    parser.add_argument("--once", action="store_true", help="answer one question and stop")
    args = parser.parse_args()

    serve(args.project, make_provider(args.provider, args.model, args.key_name, args.host),
          once=args.once)
