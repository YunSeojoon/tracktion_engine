"""Focused runtime regressions for comparison restoration and bus ranges."""
import argparse
from pathlib import Path
import time
import uuid

from cocompose import atomic_write, read, tool, wait_for
from test_plugin_compatibility import Session, prepare_song
from test_chain_changes import Report, check_a_chain_comparison_covers_where_that_insert_plays


def restoration(exe, folder, report):
    folder.mkdir(parents=True)
    session = Session(exe, folder).open()
    project = folder / "project.json"
    try:
        prepare_song(session)
        state = session.settled()
        mine = state["mixer"]["inserts"][0]["id"]
        made = tool(project, "create_proposal", {
            "base_revision": read(folder / "sync-status.json")["revision"],
            "allowed_inserts": [mine],
            "chain": [{"what": "add", "insert": mine, "type": "reverb"}]})
        session.run([{"preview": [made["result"]["proposal"]["id"], 0, 8]}])
        status_file = folder / "preview-status.json"
        record = wait_for(lambda: read(status_file) if status_file.exists()
                          and not read(status_file).get("running") else None,
                          timeout=300, what="comparison")
        report.expect("successful pair is persisted", record.get("paired") is True)
    finally:
        session.close()

    # Exercise the actual startup reader with each durable completion state.
    # Failure fixture matches a failed second render: files survive, pair is false.
    for label, patch, expected in [
        ("success", {}, True),
        ("failed second half", {"paired": False, "limits": "render failed"}, False),
        ("legacy status", {"paired": None}, False),
        ("interrupted render", {"running": True}, False),
        ("mismatched file", {"after": dict(record["after"], fingerprint="wrong")}, False),
    ]:
        atomic_write(status_file, dict(record, **patch))
        session.open()
        try:
            time.sleep(1)
            inspector = read(folder / "chat-inspector.json")
            report.expect("restore: " + label,
                          bool(inspector.get("listening", {}).get("offered")) == expected)
        finally:
            session.close()


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=root / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path, default=root / "build-cocompose" / ("review-v7-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    report = Report()
    restoration(args.exe, args.output / "restore", report)
    for route in (None, "send", "output"):
        check_a_chain_comparison_covers_where_that_insert_plays(
            args.exe, args.output / (route or "direct"), report, route)
    print("FAILURES:", report.failures)
    raise SystemExit(bool(report.failures))
