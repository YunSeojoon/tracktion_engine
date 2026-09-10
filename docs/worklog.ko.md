# 공동 작곡 편집기 작업 기록

## 2026-09-07 — CoCompose 첫 구현

대상은 기존 DAW·VST 사용자다. 목표는 외부 AI와 함께 작곡하는 것이며 앱 내 채팅은 뒤로 미뤘다. 필수 조건인 라이브 싱크는 열린 `Edit`를 유지하면서 데이터와 UI를 갱신하는 방식으로 구현한다.

### 작업 단위

| 작업 | 상태 | 결과 |
|---|---|---|
| fl-mcp 조사 | 코드·기록 조사 완료 | [조사 문서](fl-mcp-lessons.ko.md). FL 앱을 이번에 실행해 재현한 결과는 아님 |
| Windows 앱 | 구현·통합 검사 완료 | `examples/CoCompose`: 전용 창, 타임라인, 재생, 템포, 트랙 추가, 선택 클립 transpose, 플러그인 스캔 |
| JSON 라이브 싱크 | 구현·통합 검사 완료 | 250ms 감시, 두 번 동일 내용 확인, revision 충돌 검출, 전체 사전 검증, 기존 Edit 직접 갱신 |
| 실제 상태 내보내기 | 구현·통합 검사 완료 | `state.json` 엔진 readback, `sync-status.json` 진단, `session.tracktionedit` 저장 |
| Windows 사용 문서 | 인터페이스 반영 완료 | [실행 가이드](windows-guide.ko.md), 루트 `README.ai-editor.md` |
| 외부 제어 helper | 구현·통합 검사 완료 | `tools/cocompose.py`: 상태 읽기, 템포·노트·gain·파라미터 수정, 재생·정지·undo·redo·종료 |
| 통합 검사 | 10개 통과 | `tools/test_live_sync.py`, 종료 코드 0 |

### 구현 선택과 범위

별도 MCP 서버나 앱 내 모델 공급자 연결보다 JSON 파일 경로를 먼저 만들었다. 외부 도구가 최신 실제 상태를 읽고 수정한 뒤 결과를 확인할 수 있다. MIDI 트랙·클립·노트는 안정적인 ID로 찾고 갱신하며, 배열에서 빠진 항목은 삭제한다. 트랙 gain/mute/solo, BPM 및 이미 존재하는 플러그인의 공개 파라미터를 지원한다.

네이티브 프로젝트 로드는 앱 시작 시 한 번 수행한다. 라이브 변경 때 프로젝트를 다시 열거나 Edit 전체를 교체하지 않는다. 유효하지 않은 요청은 수정 전에 거절하고, 적용 중 예외는 현재 undo transaction으로 되돌린다. UI와 외부 도구의 경쟁 편집은 session ID와 revision으로 검출한다.

입력용 `project.json`은 초기 생성 후 앱이 덮어쓰지 않는다. 모든 후속 수정은 최신 `state.json`에서 시작한다. 요청 ID로 제출과 결과를 연결하며, 제어 요청은 `control.json`과 `control-status.json`을 사용한다. 파일 저장 실패는 `applied_unpersisted`로 구분한다. VST의 공개 파라미터 밖 내부 상태는 약 2초마다 flush와 서명 비교로 감지한다. 해당 경로의 타사 플러그인 호환성은 미검증이다.

현재 구현은 C++ 바이너리 자체의 핫 리로드, 완성형 피아노롤, 앱 내 채팅, WAV 렌더/A-B 평가를 포함하지 않는다. 서드파티 VST 호스팅 활성화와 실제 플러그인 호환성 검증도 구분한다.

### 검증 기록

CoCompose Windows Release 빌드가 MSVC 19.44.35223에서 성공했다. `tools/test_live_sync.py`를 실제 Release 실행 파일에 대해 실행하여 아래 10개 검사를 모두 통과했고 종료 코드는 0이었다. 테스트 결과 파일도 문서 담당 에이전트가 읽어 확인했다.

1. Windows 앱 시작과 실제 Tracktion 재생 상태.
2. 동일 session/Edit 인스턴스에서 템포·트랙 이름·음높이·velocity 변경 및 실제 UI 라벨 변경.
3. 유효하지 않은 MIDI를 이전 상태 변경 없이 거절.
4. 저장 중인 불완전 JSON을 이전 상태 변경 없이 거절.
5. 오래된 revision과 다른 session 요청 거절.
6. 노트 전체 비우기, 한 번의 Undo 복원, Redo 재적용.
7. 내장 신스 필터 파라미터 실제 엔진 readback 및 Undo.
8. 트랙 추가·순서 변경·삭제 중 같은 Edit 유지.
9. state 출력 경로 차단 시 `applied_unpersisted` 구분 및 경로 복구 후 자동 저장.
10. 정상 종료 후 재실행 시 음악과 내장 플러그인 상태 보존. 부동소수점 비교 허용 오차는 `1e-8`.

검증 산출물은 `build-cocompose/live-test-a66d5569/`의 `test-report.json`, `before.png`, `after.png`, `ui-state.json`이다. 이 폴더는 로컬 빌드 산출물이므로 다른 checkout에는 없을 수 있다. 재현 명령은 저장소 루트에서 `python tools/test_live_sync.py`다. 기존 앱은 먼저 정상 종료한다.

라이브 검사에서는 session/Edit 유지 여부를 확인했고, 마지막 검사의 종료·재실행은 저장 복원 검증을 위해 별도로 수행했다. 음악 변경 때 프로젝트를 다시 열도록 요구하지 않는다.

실제 오디오 청취와 외부 VST3 호환성은 미검증이다. 재생 그래프 변경으로 엔진의 재생 플래그가 꺼지면 사용자의 재생 의도를 유지해 다음 UI tick에서 재개한다. 앱·프로젝트를 다시 열 필요는 없지만 무중단 음향을 보장하지 않는다.

### Windows ZIP 배포 작업

`tools/build-windows.ps1`에 Visual Studio 2022용 CMake Release 빌드와 CPack ZIP 생성을 연결했다. 출력은 `build-cocompose/dist/CoCompose-0.1.0-windows-x64.zip`이며 로컬 패키지 검증은 진행 중이다. 기존 Release 앱의 통합 검사 성공이 ZIP 배포 검사까지 의미하는 것은 아니다.

`.github/workflows/cocompose-windows.yml`에 Windows 빌드·artifact 업로드를 추가했다. 관련 경로의 `ai-editor` push, 해당 브랜치 대상 PR, 수동 실행을 지원한다. 원격 CI 실행 성공 여부는 아직 확인하지 않았다.

ZIP을 모두 풀어 `CoCompose.exe`를 더블클릭하는 사용법을 문서에 반영했다. 일반 실행에는 PowerShell·Python·Visual Studio가 필요 없다. 기본 프로젝트는 문서 폴더에 저장되므로 새 배포 폴더와 분리된다. `docs/windows-portable.ko.txt`가 배포용 안내다. 현재 코드 서명·설치 프로그램·자동 업데이트는 포함하지 않는다.

## 2026-09-10 — M0 인수·빌드·배포 기준선 확정

`docs/claude-handoff-milestones.ko.md`의 M0 완료 조건을 검증했다. 인수 시점의 미커밋 배포 작업은 덮어쓰지 않고 검토 후 `2e45b25`로 커밋했다.

### ZIP 배포 EXE 검증

로컬 `build-cocompose/dist/CoCompose-0.1.0-windows-x64.zip`의 내용은 `CoCompose.exe`, `README.txt`, `licenses/`, `tools/`이며 EXE가 압축 해제 폴더 바로 아래에 있다. ZIP 안의 EXE는 `build-cocompose/CoCompose_artefacts/Release/CoCompose.exe`와 SHA-256이 같다.

`python tools/test_live_sync.py --exe <압축 해제한 CoCompose.exe>`를 ZIP EXE에 대해 실행해 기존 10개 통합 검사를 모두 통과했고 종료 코드는 0이었다. 검사 항목은 위 2026-09-07 기록과 같다.

### Windows CI 검증

`.github/workflows/cocompose-windows.yml`이 `ai-editor` push에서 실행되어 성공했다. run 34420462358, 약 9분, 커밋 `2e45b25`. `CoCompose-windows-x64` artifact를 다운로드해 압축을 풀었고 ZIP 구조와 EXE 버전 정보(0.1.0)가 로컬 패키지와 일치했다.

CI artifact의 EXE에 대해서도 `tools/test_live_sync.py`를 실행해 10개 검사를 모두 통과했다. 원격 빌드 산출물이 로컬 빌드와 동일하게 동작함을 확인했다.

### 개발 도구 없는 환경 시작 검사

`tools/test_portable_start.ps1`을 추가했다. PATH를 Windows 기본 디렉터리만 남기고 `cmake`, `python`, `cl`, `git`, `msbuild`가 실제로 도달 불가능한지 확인한 뒤, 인자 없이 EXE를 실행한다. 더블클릭과 같은 경로다.

로컬 ZIP EXE와 CI artifact EXE 모두 창(`CoCompose`)이 뜨고, 문서 폴더의 기존 프로젝트가 그대로 복원되었다. 트랙 1개, 노트 32개, BPM 120, 트랙 이름 `CoCompose Synth`가 실행 전후로 동일했고 `state.json`이 갱신되었다. 새 예제 프로젝트로 덮어쓰지 않는다.

### 남은 제한

실제 청음과 타사 VST3 호환성은 여전히 미검증이다. 코드 서명·설치 프로그램·자동 업데이트는 없다. 검사는 개발 도구가 설치된 장비에서 PATH를 제한해 수행한 것이며, 완전히 새로 설치한 Windows 장비에서의 검사는 아니다.

## 2026-09-10 — M1 작업 공간과 패턴 중심 데이터 모델

`docs/claude-handoff-milestones.ko.md`의 M1을 구현했다. 화면을 흉내 내는 대신 각 패널의 컨트롤이 모델을 거쳐 실제 엔진에 도달하도록 만들었다.

### 데이터 모델

Edit 안의 `COCOMPOSE` 트리에 Channel, Pattern, Playlist lane, Clip instance, Mixer insert를 서로 다른 객체로 둔다. 하나의 engine audio track에 모두 묶지 않는다. Channel 하나가 engine audio track 하나를 소유하고, Pattern은 채널별 `SEQUENCE`로 노트를 담는다. Clip instance는 패턴을 레인의 특정 박에 배치한 참조다.

엔진 MIDI 클립은 이 모델에서 파생시킨다. (배치, 그 패턴에서 연주하는 채널) 쌍마다 클립 하나를 만들고 클립 상태에 두 ID를 기록해 다시 찾는다. 그래서 패턴을 고치면 그 패턴의 모든 배치가 함께 바뀐다. 파생 클립 자체는 Undo 대상이 아니다. Undo는 모델을 되돌리고 다음 tick에서 엔진을 다시 파생시키므로 한 편집이 한 Undo로 유지된다.

Mixer insert는 모델과 저장까지만 구현했다. 실제 라우팅은 M5다. 현재 오디오는 각 채널에서 Master로 직결된다.

### 변환

`COCOMPOSE` 트리가 없는 세션을 열면 트랙마다 채널과 전용 레인을, MIDI 클립마다 패턴과 배치 하나를 만든다. 기존 클립을 지우고 다시 만들지 않고 배치 ID를 붙여 그대로 인수하므로 편곡과 플러그인 상태가 그대로 남는다. 트랙·클립·노트의 `coComposeId`를 각각 채널·패턴·노트 ID로 재사용한다.

schema 1 JSON 입력도 같은 규칙으로 schema 2로 올린 뒤 적용한다. 두 경로 모두 ID가 같으므로 기존 외부 스크립트의 참조가 유지된다.

`tests/cocompose/legacy-schema1.tracktionedit`는 패턴 도입 이전 빌드가 저장한 실제 세션이며 변환 검사에 사용한다.

### 화면

Browser(왼쪽), Channel Rack(가운데 위), Mixer(가운데 아래), Pattern picker와 Playlist(오른쪽)를 배치했다. 패널 사이 막대로 크기를 조절하고 View 메뉴 또는 `Alt+1`~`Alt+5`로 숨기거나 되살린다. 포커스된 패널은 테두리로 표시하며 `F6`으로 순환한다. 패널 크기·표시 여부·선택은 Edit의 `COCOMPOSELAYOUT`에 저장되어 다시 열 때 복원된다. 선택은 편집이 아니므로 Undo 대상이 아니다.

각 패널은 실제 데이터를 보여준다. Browser는 프로젝트의 채널·패턴·레인·인서트와 스캔된 플러그인을, Channel Rack은 채널별 이름·mute/solo·볼륨·인서트 번호·악기 창과 선택 패턴에서의 노트 수를, Mixer는 Master와 인서트별 볼륨·팬·mute 및 배정된 채널 수를, Pattern picker는 패턴과 배치 횟수를, Playlist는 레인과 배치 개수 및 파생된 클립이 보이는 타임라인을 보여준다.

상단에 File/Edit/View/Tools/Help 메뉴와 재생·Song/Pattern·BPM·마디:박·메트로놈·CPU·포커스 표시를 붙였다. 모든 메뉴 항목은 `ApplicationCommandManager`에 연결되어 있고 동작하지 않는 항목은 두지 않았다. Song 모드는 편곡 전체를, Pattern 모드는 선택한 패턴의 첫 배치를 반복한다.

### 검증

`tools/test_live_sync.py`가 15개로 늘었고 전부 통과했다. 기존 10개 항목을 새 모델로 옮기면서 각 항목에 `state.json`의 읽기 전용 `engine` 결과 확인을 추가해, 모델만 바뀌고 엔진에는 도달하지 않는 통과를 막았다.

추가된 검사는 다음 다섯 가지다.

11. 한 패턴을 두 곳에 배치하고 패턴을 수정하면 두 배치의 엔진 클립이 함께 바뀌며 Undo 한 번으로 둘 다 복원된다.
12. 배치 하나에 패턴 복제본을 연결하면 그 배치만 바뀌고 원래 패턴은 그대로다.
13. 패턴 도입 이전 세션이 채널·패턴·플레이리스트로 변환되며 ID와 노트가 유지된다.
14. schema 1 `project.json` 입력이 패턴 모델로 올라간다.
15. 패널 크기·표시 여부·선택이 세션에 저장되고, 수정한 값이 다음 실행에서 그대로 복원된다.

화면은 실행 중인 앱의 스크린샷으로도 확인했다. 외부에서 채널·시퀀스·배치를 추가하면 Browser, Channel Rack, Mixer, Pattern picker, Playlist가 앱을 다시 열지 않고 갱신된다.

### 남은 제한

패널 크기 조절 막대를 마우스로 끄는 동작 자체는 자동 검사가 아니다. 저장된 크기를 앱이 존중하는지까지만 검사한다. 스텝 시퀀서·피아노롤(M2), 마우스 플레이리스트 편집(M3), 샘플 브라우저(M4), 믹서 라우팅과 효과(M5)는 아직 없다. Playlist 타임라인은 Tracktion 예제 컴포넌트이며 레인이 아니라 채널 단위로 클립을 보여준다. 실제 청음과 타사 VST3 호환성은 여전히 미검증이다.

## 2026-09-10 — M2 Channel Rack과 피아노롤

`docs/claude-handoff-milestones.ko.md`의 M2를 구현했다.

### 채널

채널마다 악기를 고른다. 내장 4OSC, 내장 샘플러, 스캔된 VST3 악기가 선택지이며 모델의 `instrument` 값이 곧 트랙 첫 플러그인의 종류다. 값이 바뀌면 `render()`가 기존 악기를 교체하고, 만들 수 없는 플러그인이면 요청을 모델에 남긴 채 교체하지 않는다. 나중에 스캔하면 그때 붙는다.

샘플러는 `sample` 경로의 파일 하나를 채널의 `step_pitch`를 기준음으로 건반 전체에 배치한다. 채널 행의 `WAV` 버튼이 파일을 고르고 악기를 샘플러로 바꾼다.

행에는 이름, mute/solo, 악기 선택, 악기 창 열기, 샘플 선택, 볼륨·팬 노브, 믹서 인서트 번호가 들어간다. 나머지 공간은 스텝 그리드가 차지한다.

### 스텝 그리드

선택한 패턴을 16분음표 단위로 나눈 격자다. 스텝 하나는 그 구간에서 시작하는 노트가 있는지로 결정한다. 누르면 채널의 `step_pitch`·`step_length`로 노트를 쓰고, 켜진 스텝을 누르면 지운다. 두 값은 채널 행의 스텝 설정 버튼에서 1/16~1마디 길이와 기준음으로 고른다. 끌면 여러 스텝을 연속으로 바꾼다. 새 패턴의 기본 길이는 한 마디(4박, 16스텝)로 맞췄다.

### 피아노롤

`Piano roll` 버튼이 선택한 채널·패턴의 노트 편집기를 별도 창으로 연다. 추가·삭제·이동·길이 조절·velocity 조절, 사각형 다중 선택, 복제, 퀀타이즈, 스냅(1/1~1/16, off), 확대를 제공한다. 왼쪽 건반을 누르면 `playGuideNote`로 채널의 실제 악기를 통해 미리듣는다. 아래쪽 VELOCITY 칸은 그리드와 가로 스크롤을 공유하며 노트별 세기를 보여준다. 열 때 노트가 있는 음역으로 스크롤한다.

노트는 모델의 `NOTE`이므로 스텝이든 피아노롤이든 편집은 그 패턴의 모든 배치에 반영되고 Undo 한 번으로 함께 되돌아간다. 외부 편집과 같은 경로다.

### 화면 조작 재생 (`--ui-script`)

화면이 실제로 동작하는지 외부에서 검사하기 위해 `--ui-script <파일>`을 추가했다. JSON 배열의 조작을 250ms마다 실제 패널에 재생하고 `ui-script-status.json`에 진행 상황을 남긴다. 지원 항목은 메뉴 명령 이름, 채널·패턴·레인 선택, 스텝 토글, 피아노롤 노트 추가다. 모델을 직접 건드리지 않고 화면과 같은 코드 경로를 지나간다. `--screenshots`와 같은 진단 기능이며 라이브 싱크 규약의 일부가 아니다.

`--screenshots`는 피아노롤이 열려 있으면 `piano-roll.png`도 남긴다.

### schema

채널에 `instrument`, `sample`, `step_pitch`, `step_length`를 추가했다. 기존 schema 2 문서에 없어도 되는 선택 필드이므로 버전은 올리지 않았다. 값이 비어 있으면 4OSC로 정규화한다.

### 검증

`tools/test_live_sync.py`가 16개로 늘었고 전부 통과했다. 새 검사는 M2의 완료 조건이다. `--ui-script`로 메뉴 명령·스텝 그리드·피아노롤만 사용해 드럼 6스텝, 베이스 4스텝, 멜로디 5노트의 3채널 패턴을 만들고, 배치·재생·저장까지 진행한 뒤 다음을 확인한다.

- 세 채널의 노트가 모델과 엔진 클립 3개에 정확히 들어간다.
- 재생이 실제로 시작된다.
- 외부에서 멜로디를 반음 3개 올리면 열려 있는 편집기의 모델과 엔진 클립에 반영된다.
- 종료 후 다시 열면 채널·패턴·엔진 클립이 그대로 복원된다.
- 화면 라벨에 세 채널 이름이 모두 나타난다.

피아노롤 화면은 `piano-roll.png` 스냅샷으로도 확인했다.

### 남은 제한

샘플러는 사운드 하나만 다루며 다중 존·루프·스트레치는 없다. 마우스 드래그 자체는 자동 검사 대상이 아니고 `--ui-script`가 같은 코드 경로를 호출하는 것까지 검사한다. 마우스 플레이리스트 편집(M3), 샘플 브라우저(M4), 믹서 라우팅과 효과(M5)는 아직 없다. 실제 청음과 타사 VST3 호환성은 여전히 미검증이다.
