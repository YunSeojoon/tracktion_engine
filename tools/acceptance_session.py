"""Sets up and records a real acceptance session on this machine.

P2 of the second review cannot be finished by a script. Choosing a sound inside a
plugin's window, playing a keyboard, unplugging an interface and listening to the
result are all things a person does. What a script can do is everything around them:
write down exactly what the machine was, drive the parts that are automatable, and
leave a record with the human steps still open rather than quietly passed.

Run it with the app closed. It leaves a folder holding the project, the render and
acceptance-record.md, which is the sheet the person fills in.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cocompose import apply_change, atomic_write, control, read
from test_plugin_compatibility import Session, read_wav, render, scan


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest().upper()


def commit_of(root):
    try:
        return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                              capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def build_the_skeleton(session):
    """Three named channels and an arrangement, so the person starts from a song rather
    than an empty page. The sounds are left as the built-in synth on purpose: choosing
    real ones is the part being accepted."""
    session.run([{"command": "Add channel"}, {"command": "Add channel"}])
    names = ["Drums", "Bass", "Lead"]

    def rename(live):
        for channel, label in zip(live["channels"], names):
            channel["name"] = label

    apply_change(session.folder / "project.json", rename)
    time.sleep(1.5)

    figures = {0: [(36, 0), (42, 4), (38, 8), (42, 12)],
               1: [(36, 0), (36, 6), (43, 8), (41, 12)],
               2: [(72, 0), (76, 2), (79, 6), (76, 10), (72, 14)]}
    for channel, notes in figures.items():
        session.run([{"select_channel": channel}]
                    + [{"note": [pitch, float(start), 2.0, 100]} for pitch, start in notes])

    session.run([{"select_lane": 0}] + [{"place": [0, beat]} for beat in (0.0, 32.0, 64.0, 96.0)])
    return session.settled()


def outside_edit_while_playing(session):
    """The one thing this app is for: an outside tool changing a project that is open
    and playing, with no reopening anywhere in the loop."""
    project = session.folder / "project.json"
    control(project, "play")
    time.sleep(2.0)
    playing = bool(read(session.folder / "sync-status.json").get("playing"))
    session_id = read(session.folder / "sync-status.json")["session_id"]

    def louder(live):
        live["channels"][2]["gain_db"] = -4.5
        live["bpm"] = 128.0

    after, _ = apply_change(project, louder)
    time.sleep(1.5)
    still = read(session.folder / "sync-status.json")

    control(project, "stop")
    return {"was_playing": playing,
            "bpm_reached_the_open_project": after["bpm"] == 128.0,
            "same_session_throughout": still["session_id"] == session_id,
            "still_playing_after_the_edit": bool(still.get("playing")) if playing else None}


def save_and_reopen(session):
    before = session.settled()
    session.close()
    session.open()
    after = session.settled()
    same = all(len(after[part]) == len(before[part]) for part in ("channels", "patterns"))
    return {"channels": len(after["channels"]), "patterns": len(after["patterns"]),
            "bpm": after["bpm"], "unchanged": same and after["bpm"] == before["bpm"]}


def record_sheet(report, folder):
    device = report["device"]
    inputs = device.get("inputs_available") or []
    midi = [i for i in inputs if i["kind"] == "midi"]
    audio_in = [i for i in inputs if i["kind"] == "audio"]

    def row(name, value):
        return "| %s | %s |" % (name, value)

    def listed(devices):
        return ", ".join("%s (%s)" % (i["name"], "켜짐" if i["enabled"] else "꺼짐")
                         for i in devices) or "없음"

    lines = [
        "# CoCompose 수용 검사 기록",
        "",
        "P2. 이 문서의 표는 스크립트가 기계에서 읽어 채웠고, 아래 체크리스트는 사람이 채운다.",
        "빈 항목은 통과가 아니라 미검증이다.",
        "",
        "## 이 기계",
        "",
        "| 항목 | 값 |",
        "|---|---|",
        row("앱", "CoCompose " + report["version"]),
        row("커밋", report["commit"][:12]),
        row("오디오 장치", "%s (%s)" % (device.get("name", "-"), device.get("type", "-"))),
        row("샘플레이트 / 버퍼", "%s Hz / %s samples"
            % (device.get("sample_rate", "-"), device.get("buffer_size", "-"))),
        row("출력 지연", "%s samples" % device.get("output_latency", "-")),
        row("MIDI 입력", listed(midi)),
        row("오디오 입력", listed(audio_in)),
        row("건반 검사 가능", "가능" if any(i["enabled"] for i in midi) else "MIDI 입력이 없어 불가"),
        row("스캔된 플러그인", "악기 %d, 효과 %d"
            % (report["instruments"], report["effects"])),
        row("프로젝트 폴더", "`%s`" % folder),
        "",
        "## 스크립트가 확인한 것",
        "",
        "| 검사 | 결과 |",
        "|---|---|",
        row("재생 중 외부 편집이 열린 프로젝트에 닿음",
            "O" if report["outside_edit"]["bpm_reached_the_open_project"] else "X"),
        row("그동안 세션이 바뀌지 않음",
            "O" if report["outside_edit"]["same_session_throughout"] else "X"),
        row("재생이 시작됨",
            "O" if report["outside_edit"]["was_playing"] else "장치 없음"),
        row("저장 후 다시 열기", "O" if report["reopen"]["unchanged"] else "X"),
        row("믹스 렌더", "%s초, peak %s"
            % (round(report["render"].get("seconds", 0), 1),
               round(report["render"].get("peak", 0), 4))),
        row("렌더 파일 SHA-256", "`%s`" % report.get("render_sha256", "-")),
        "",
        "## 사람이 채우는 항목",
        "",
        "각 줄에 실제로 한 것과 결과를 적는다. 하지 않았으면 `미검증`으로 남긴다.",
        "",
        "| 항목 | 어떻게 | 결과 |",
        "|---|---|---|",
        "| 악기 음색 선택 | 채널의 `...` 버튼으로 플러그인 창을 열고 실제로 쓸 음색을 고른다 | |",
        "| 음색 바꾸고 바로 렌더 | 음색을 바꾸자마자 Export WAV. 들리는 것과 파일이 같은가 | |",
        "| MIDI 건반 녹음 | 채널을 Arm하고 Record. 친 것이 패턴으로 들어왔는가 | |",
        "| 오디오 입력 녹음 | 오디오 입력을 Arm하고 Record. 파형이 들어왔는가 | |",
        "| 재생 중 장치 전환 | 재생 중 Tools > Audio settings에서 다른 장치로 바꾼다. 앱이 살아 있고 소리가 이어지는가 | |",
        "| 인터페이스 분리 | 재생 중 USB 인터페이스를 뽑는다. 앱이 죽지 않고 상태를 알리는가 | |",
        "| 저장 / 복원 | 닫았다 다시 연다. 고른 음색과 설정이 그대로인가 | |",
        "| 청음 | 렌더된 WAV를 듣는다. 무음·클리핑·끊김·타이밍 | |",
        "",
        "### AI와 함께 쓸 때",
        "",
        "여기부터는 검사가 대신할 수 없다고 기록해 둔 항목들이다. 각각 왜 기계가",
        "못 하는지도 적어 둔다 — 빈 칸이 \"아직 안 했다\"인지 \"할 수 없다\"인지",
        "구별되지 않으면 미검증 목록은 쓸모가 없다.",
        "",
        "| 항목 | 어떻게 | 왜 사람이 | 결과 |",
        "|---|---|---|---|",
        "| A/B 청음 | 제안을 하나 받아 Preview를 누르고 preview-before.wav와 "
        "preview-after.wav를 듣는다 | 제안이 더 나은지는 듣는 일이다. 앱도 모델도 "
        "보고할 수 있는 것이 아니다 | |",
        "| 후보 셋 중에 고르기 | 같은 구간을 세 번 물어 셋을 받고, 각각 Preview로 듣고 "
        "두 번째를 채택한다 | 셋이 고를 만한 음악인지는 판단이다 | |",
        "| 들리지 않는 변경 | 공유된 클립 하나를 Make unique 제안으로 바꿔 A/B를 "
        "듣는다. 같게 들려야 맞다 | 앱은 \"양쪽이 같은 노트를 연주했다\"까지만 말할 수 "
        "있고 어떻게 들렸는지는 말할 수 없다 | |",
        "| 채팅 중 Space / Enter / Home | 채팅 입력창에 글을 쓰다가 각각 눌러 본다. "
        "재생·피아노롤·플레이헤드가 움직이면 안 된다 | 헤드리스 창은 OS 키보드 포커스를 "
        "받지 못해 스크립트가 그 상태를 만들 수 없다 | |",
        "| 메뉴가 포인터 아래 | 패턴 클립·노트·빈 격자·레인 헤더·채널 헤더·이펙트 "
        "슬롯·오디오 클립·노브를 우클릭한다. 메뉴가 가리킨 자리에 뜨고 문구가 읽히는가 | "
        "메뉴가 어디 뜨는지와 어떻게 읽히는지는 보는 일이다 | |",
        "| 100 / 150 / 200% 가독성 | Windows 표시 배율을 바꿔 가며 네 패널을 본다. "
        "잘리거나 겹치거나 읽을 수 없는 곳 | 배율에서 글자가 읽히는지는 보는 일이다 | |",
        "| 서드파티 VST 상태 | VST 악기 하나의 음색을 바꾼 뒤 A/B를 만든다. A가 바꾼 "
        "뒤의 소리인가 | 같은 스냅샷 경로를 내장 플러그인으로는 확인했지만 스캔된 VST로는 "
        "한 번도 지나가지 않았다 | |",
        "| hosted provider | 키를 환경 변수에 넣고 `--provider openai`로 브리지를 "
        "띄워 한 번 묻는다 | 이 기계에서 호스티드 경로는 한 번도 호출된 적이 없다. "
        "검증된 것은 로컬 ollama뿐이다 | |",
        "| 두 모델 교체 | 서로 다른 모델로 브리지를 번갈아 띄우며 대화를 잇는다. 이전 "
        "답이 새 모델 것으로 표시되지 않는가 | 픽스처 둘로는 앱의 장부만 증명된다 | |",
        "",
        "### 한 곡을 끝까지",
        "",
        "B6의 완료 조건이다. 한 프로젝트에서 순서대로 하고, 중간에 막히면 어디서",
        "막혔는지 적는다.",
        "",
        "| 단계 | 결과 |",
        "|---|---|",
        "| 구간을 골라 AI에게 진단을 묻는다 | |",
        "| 노트를 첨부해 다시 써 달라고 한다 | |",
        "| Insert를 첨부해 믹서를 확인하고 고쳐 달라고 한다 | |",
        "| 제안을 Preview로 듣는다 | |",
        "| 채택한다 | |",
        "| Undo로 되돌린다 | |",
        "| 앱을 닫고 다음 날 열어 같은 대화를 잇는다 | |",
        "",
        "## 무음이었던 플러그인",
        "",
        "`docs/compatibility-2026-09-10.ko.md`에서 렌더 peak가 0이었던 악기들은 자기 라이브러리를",
        "불러와야 소리가 난다고 적었다. 그것은 아직 추정이다. 위에서 음색을 실제로 고른 뒤에도",
        "무음이면 원인이 다른 것이므로 여기에 적는다.",
        "",
        "| 플러그인 | 음색을 고른 뒤 | 비고 |",
        "|---|---|---|",
    ]
    for name in report.get("silent_plugins", []):
        lines.append("| %s | | |" % name)

    lines += ["", "## 판정", "",
              "위 사람 항목이 모두 채워지기 전까지 P2는 완료가 아니다. B6도 마찬가지다 -",
        "\"한 곡을 끝까지\"의 일곱 줄이 비어 있으면 그 단계는 시작되지 않은 것이고,",
        "기계 검사가 몇 건 통과했는지는 그것을 대신하지 않는다.", ""]
    return "\n".join(lines)


def run(exe, output):
    root = Path(__file__).resolve().parents[1]
    output.mkdir(parents=True, exist_ok=True)
    folder = output / "session"
    folder.mkdir(exist_ok=True)

    session = Session(exe, folder).open()
    report = {"version": "0.1.0", "commit": commit_of(root)}
    try:
        catalogue = (read(folder / "plugin-scan.json") if (folder / "plugin-scan.json").exists()
                     else scan(session))
        report["instruments"] = len([p for p in catalogue["found"] if p["instrument"]])
        report["effects"] = len([p for p in catalogue["found"] if not p["instrument"]])

        build_the_skeleton(session)
        report["device"] = session.settled()["engine"].get("device") or {}
        report["outside_edit"] = outside_edit_while_playing(session)
        report["render"] = render(session)
        if (folder / "mix.wav").exists():
            report["render_sha256"] = digest(folder / "mix.wav")
            report["render"].update(read_wav(folder / "mix.wav"))
        report["reopen"] = save_and_reopen(session)
    finally:
        session.close()

    compat = root / "docs" / "compatibility-2026-09-10.ko.md"
    if compat.exists():
        marker = "기본 상태에서는 무음"
        report["silent_plugins"] = [line.split("|")[1].strip()
                                    for line in compat.read_text(encoding="utf-8").splitlines()
                                    if line.startswith("|") and marker in line]

    atomic_write(output / "acceptance.json", report)
    (output / "acceptance-record.md").write_text(record_sheet(report, folder), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "silent_plugins"},
                     indent=2, ensure_ascii=False))
    print("Sheet to fill in: " + str(output / "acceptance-record.md"))
    return report


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=root / "build-cocompose/CoCompose_artefacts/Release/CoCompose.exe")
    parser.add_argument("--output", type=Path,
                        default=root / "build-cocompose" / ("acceptance-" + uuid.uuid4().hex[:8]))
    args = parser.parse_args()
    run(args.exe.resolve(), args.output.resolve())
