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
from pathlib import Path
import sys
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

You cannot change the project. If they ask for a change, describe precisely what you
would change - which notes, which parameters, what values - and say that applying it is
not available yet. Do not pretend to have made a change.

Refer to things the way the person sees them: bar numbers, channel names, note pitches.
Be brief and concrete. If the attachment is empty or no longer in the song, say so."""


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
                lines.append("    clip '%s' at beat %.3f for %.3f beats"
                             % (clip.get("pattern_name", ""), clip["start_beat"], clip["length_beats"]))
            if detail.get("context_clips"):
                lines.append("    (context, not what was asked about: %d clip(s) either side)"
                             % len(detail["context_clips"]))

        elif a["kind"] == "notes":
            lines.append("    pattern '%s', %d beats long, placed %d time(s)%s"
                         % (detail.get("name", ""), detail.get("length_beats", 0),
                            detail.get("placement_count", 0),
                            " - changing it changes all of them" if detail.get("shared") else ""))
            for part in detail.get("parts", []):
                lines.append("    channel '%s' playing %s"
                             % (part.get("channel_name", ""), part.get("instrument", "")))
                for note in part.get("notes", []):
                    lines.append("      pitch %d  beat %.3f  length %.3f  velocity %d"
                                 % (note["pitch"], note["start_beat"],
                                    note["length_beats"], note["velocity"]))

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
                for parameter in effect.get("parameters", [])[:12]:
                    lines.append("        %s = %.3f" % (parameter.get("name", parameter["id"]),
                                                        parameter["value"]))
            lines.append("    " + detail.get("opaque_state", ""))
            for channel in detail.get("fed_by", []):
                lines.append("    fed by channel '%s'" % channel.get("name", ""))

    return "\n".join(lines)


def build_prompt(request):
    history = request.get("history", {}).get("messages", [])
    parts = []

    if len(history) > 1:
        parts.append("Earlier in this conversation about the same project:")
        for message in history[:-1]:
            text = (message.get("text") or "").strip()
            if text:
                parts.append("  %s: %s" % (message["from"], text[:600]))
        parts.append("")

    parts.append(describe_attachments(request.get("attachments", [])))
    parts.append("")
    parts.append("Their question: " + request.get("message", ""))
    return "\n".join(parts)


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


def make_provider(name, model, key_name):
    if name == "echo":
        return Echo()

    key = os.environ.get(key_name, "")
    if not key:
        raise SystemExit("No %s in the environment. Set it, or use --provider echo." % key_name)

    if name == "openai":
        return OpenAIChat(model or "gpt-4o-mini", key)

    raise SystemExit("Unknown provider: " + name)


def serve(project, provider, once=False):
    folder = Path(project).parent
    folder.mkdir(parents=True, exist_ok=True)

    request_file = folder / "chat-request.json"
    reply_file = folder / "chat-reply.json"
    cancel_file = folder / "chat-cancel.json"
    state_file = folder / "chat-bridge.json"

    # The app reads this to know whether anything is listening, so it can say "nothing is
    # connected" instead of leaving a question waiting forever.
    atomic_write(state_file, {"ready": True, "name": provider.name,
                              "started_ms": int(time.time() * 1000)})
    print("bridge ready:", provider.name)
    print("watching", request_file)

    answered = set()
    try:
        while True:
            request = read(request_file)
            request_id = request.get("request_id")

            if not request_id or request_id in answered:
                if once and answered:
                    return
                time.sleep(0.2)
                continue

            answered.add(request_id)
            print("question:", (request.get("message") or "")[:70])

            def stream(so_far, _id=request_id):
                if read(cancel_file).get("request_id") == _id:
                    raise KeyboardInterrupt
                atomic_write(reply_file, {"request_id": _id, "status": "streaming",
                                          "text": so_far, "provider": provider.name})

            try:
                text = provider.answer(request, stream)
                atomic_write(reply_file, {"request_id": request_id, "status": "ok",
                                          "text": text, "provider": provider.name})
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

            if once:
                return
    finally:
        atomic_write(state_file, {"ready": False, "name": provider.name})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--provider", default="echo", choices=["echo", "openai"])
    parser.add_argument("--model", default=None)
    parser.add_argument("--key-name", default="OPENAI_API_KEY",
                        help="environment variable holding the key; never read from the project")
    parser.add_argument("--once", action="store_true", help="answer one question and stop")
    args = parser.parse_args()

    serve(args.project, make_provider(args.provider, args.model, args.key_name), once=args.once)
