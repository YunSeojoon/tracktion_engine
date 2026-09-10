# Windows에서 CoCompose 실행하기

CoCompose는 DAW·VST 사용자가 외부 AI와 함께 작곡하기 위한 Windows 편집기다. 앱 안의 채팅 대신 외부 스크립트나 AI가 JSON을 수정하고, 현재 열린 프로젝트의 UI와 엔진에 그 결과를 반영한다.

## ZIP으로 실행하기

Windows x64용 `CoCompose-0.1.0-windows-x64.zip`을 폴더에 모두 압축 해제하고 `CoCompose.exe`를 더블클릭한다. 앱 실행에는 PowerShell, Python, Visual Studio가 필요 없다. Python은 외부 AI용 helper를 사용할 때만 필요하다.

기본 프로젝트는 Windows 문서 폴더의 `CoCompose`에 저장된다. 앱을 업데이트할 때는 종료 후 새 ZIP을 별도 폴더에 풀어 실행한다. 기본 프로젝트 데이터는 배포 폴더 밖에 있어 유지된다. 별도 `--project` 경로를 사용했다면 그 폴더를 계속 지정한다.

현재 ZIP은 코드 서명이 없으며 설치 프로그램과 자동 업데이트는 포함하지 않는다. 아래 개발자 스크립트가 ZIP을 생성한다. GitHub Actions workflow의 원격 실행 성공과 artifact 다운로드를 확인했고, 로컬 ZIP과 CI artifact 모두 압축 해제 후 통합 검사 10개를 통과했다.

## 개발자 빌드와 실행

Visual Studio 2022의 **C++를 사용한 데스크톱 개발**, Windows SDK, CMake 3.22 이상, Git이 필요하다. PowerShell에서 실행한다.

```powershell
Set-Location 'C:\project_private\tracktion_engine'
git submodule update --init --recursive
cmake -S examples/CoCompose -B build-cocompose -G 'Visual Studio 17 2022' -A x64
cmake --build build-cocompose --config Release --target CoCompose --parallel 4
& '.\build-cocompose\CoCompose_artefacts\Release\CoCompose.exe'
```

이미 빌드했다면 마지막 실행 명령만 쓰거나 탐색기에서 `build-cocompose\CoCompose_artefacts\Release\CoCompose.exe`를 더블 클릭한다. 설치 프로그램은 아직 없다.

개발 도구와 서브모듈 준비 후 ZIP까지 생성하려면 다음 스크립트를 사용한다. 내부에서 CMake Release 빌드와 CPack 패키징을 수행한다.

```powershell
Set-Location 'C:\project_private\tracktion_engine'
git submodule update --init --recursive
& '.\tools\build-windows.ps1'
```

결과 경로는 `build-cocompose\dist\CoCompose-0.1.0-windows-x64.zip`이다. 병렬 빌드 수는 `-Jobs 4`처럼 조절할 수 있다. 로컬 ZIP 생성 검증은 진행 중이며, 위의 Release 실행 파일 검증과 구분한다.

`.github/workflows/cocompose-windows.yml`은 `ai-editor`의 관련 경로 push, 해당 브랜치 대상 PR 또는 수동 실행에서 Windows 빌드 후 `CoCompose-windows-x64` artifact로 ZIP을 업로드한다. 원격 실행 성공과 artifact 실행을 확인했다.

배포본 자체를 검사하려면 압축을 푼 뒤 `.\tools\test_portable_start.ps1 -Exe '<압축 해제 경로>\CoCompose.exe'`를 실행한다. PATH에서 개발 도구를 제거한 상태로 앱을 띄우고 기존 기본 프로젝트가 복원되는지 확인한다.

기본 작업 파일은 Windows 문서 폴더의 `CoCompose\project.json`이다. 문서 폴더가 OneDrive 등으로 이동된 경우 화면 상단의 실제 경로를 확인한다. 폴더에 기존 작업이 없으면 내장 FourOsc 신스로 8마디 예제를 만든다. 별도 작업 폴더를 쓰려면 다음처럼 실행한다.

```powershell
& '.\build-cocompose\CoCompose_artefacts\Release\CoCompose.exe' --project 'C:\project_private\my-song\project.json'
```

동시에 실행하는 앱은 하나다. 각 곡은 별도 폴더에 보관한다. `session.tracktionedit` 등 보조 파일 이름이 폴더 단위로 고정되어 있으므로 같은 폴더에 여러 프로젝트 JSON을 두지 않는다.

C++ 소스를 수정해 다시 빌드할 때는 실행 파일 잠금을 풀기 위해 앱을 종료해야 할 수 있다. 음악 데이터 라이브 싱크는 재빌드나 앱·프로젝트 재시작 없이 동작한다. 실행 중 C++ 바이너리를 교체하는 기능은 아니다.

## 화면 구성

왼쪽에 Browser, 가운데 위아래로 Channel Rack과 Mixer, 오른쪽 위아래로 Pattern picker와 Playlist가 놓인다. 패널 사이 막대를 끌어 크기를 조절하고, View 메뉴 또는 `Alt+1`~`Alt+5`로 각 패널을 숨기거나 되살린다. 포커스가 있는 패널은 테두리가 밝게 표시되며 `F6`으로 다음 패널로 이동한다. 패널 크기·표시 여부·현재 선택은 세션에 저장되어 다시 열 때 복원된다.

| 영역 | 하는 일 |
|---|---|
| Browser | 프로젝트의 채널·패턴·플레이리스트 레인·믹서 인서트와 스캔된 플러그인 목록. 항목을 누르면 선택이 바뀐다 |
| Channel Rack | 채널 추가/삭제, 이름 변경, mute/solo, 볼륨, 믹서 인서트 번호, 악기 창 열기. 선택한 패턴에서 그 채널이 연주하는 노트 수를 함께 보여준다 |
| Mixer | Master와 인서트별 볼륨·팬·mute, 각 인서트에 배정된 채널 수 |
| Pattern picker | 패턴 목록과 배치 횟수, 새로 만들기·복제·삭제 |
| Playlist | 레인 목록과 배치 개수, `Place pattern`으로 선택한 패턴을 레인 끝에 붙이기, 그리고 모델이 만든 클립과 재생 커서를 보여주는 타임라인 |

상단 툴바는 재생/정지, Song·Pattern 루프 전환, BPM, 마디:박 위치, 메트로놈, CPU 사용률, 포커스된 패널을 표시한다. 메뉴는 File(저장·복사본 저장·폴더 열기·종료), Edit(undo/redo, 채널·패턴 추가, 패턴 배치, 배치 독립화, 반음 이동), View(패널 표시), Tools(플러그인 스캔·오디오 설정), Help로 구성된다.

단축키는 `Space` 재생/정지, `Ctrl+L` Song·Pattern 전환, `Ctrl+M` 메트로놈, `Ctrl+S` 저장, `Ctrl+Z`/`Ctrl+Shift+Z` undo/redo, `Ctrl+T` 채널 추가, `Ctrl+P` 패턴 추가, `Ctrl+B` 패턴 배치, `Ctrl+U` 선택 배치 독립화, `Ctrl+↑`/`Ctrl+↓` 패턴 반음 이동이다.

스텝 시퀀서와 피아노롤, 마우스로 하는 플레이리스트 편집, 샘플 브라우저, 믹서 라우팅과 효과는 아직 없다. 타임라인은 Tracktion 예제 컴포넌트를 사용한다.

## 외부 AI와 협업하기

앱이 생성한 `state.json`을 읽고 그 전체 구조를 유지한 채 필요한 부분만 바꿔 `project.json`으로 저장한다. 기본 흐름은 **읽기 → 수정 → 저장 → 엔진에서 다시 읽어 검증**이다.

| 파일 | 역할 |
|---|---|
| `project.json` | 외부 변경 입력 전용. 초기 샘플 생성 후 앱이 덮어쓰지 않음 |
| `state.json` | 실제 열린 엔진 상태를 읽어 내보낸 결과. 외부 도구는 여기서 수정의 출발점을 얻음 |
| `sync-status.json` | 적용 상태, 오류, revision, 세션 및 Edit 식별자, 재생 상태 |
| `session.tracktionedit` | 네이티브 프로젝트와 플러그인 상태 저장. 앱 시작 때 복원 |
| `control.json` | 재생·정지·undo·redo·종료 제어 요청 |
| `control-status.json` | 요청 ID에 대응하는 제어 결과 |

`state.json`, `sync-status.json`, `session.tracktionedit`은 외부 명령 입력용 파일이 아니다. 작업을 복사하거나 백업할 때는 폴더 전체를 보관한다.

JSON의 `session_id`와 `revision`은 최신 `state.json`에서 읽은 값을 그대로 보낸다. 두 값 모두 현재 앱과 일치해야 한다. 앱이 성공적으로 적용하면 revision이 증가한다. 사람의 UI 조작으로 revision이 바뀌거나 앱 재실행으로 세션이 바뀌면 외부 변경은 거절된다. 최신 `state.json`을 다시 읽어 의도한 수정만 재적용한다. 식별자나 revision만 바꿔 오래된 전체 상태를 덮어쓰지 않는다. `project.json`은 최신 상태 사본이 아니므로 다음 수정의 출발점으로 쓰지 않는다.

UI 변경도 감지되면 네이티브 프로젝트와 `state.json`에 저장된다. 공개 파라미터 밖의 VST 내부 상태는 약 2초마다 flush 후 서명 변화를 감지해 저장한다. 이 경로의 타사 VST 호환성은 아직 검증하지 않았다.

파일은 임시 파일에 완성본을 쓴 뒤 교체하는 방식으로 저장한다. 앱은 250ms 간격으로 확인하고 두 번 연속 동일한 내용을 읽은 뒤 적용하므로 보통 수백 ms의 지연이 있다. 처리량과 시스템 상태에 따른 추가 지연은 가능하다.

### 문서 구조 (schema 2)

문서는 다섯 가지를 구분한다.

| 키 | 내용 |
|---|---|
| `channels` | 악기 채널. `gain_db`, `pan`, `mute`, `solo`, 믹서 `insert` 번호, 플러그인 공개 `parameters` |
| `patterns` | 이름이 붙은 노트 묶음. 채널마다 `sequences` 항목 하나를 가지며 각 항목에 `notes`가 들어간다 |
| `playlist.lanes`, `playlist.clips` | 클립은 패턴을 레인의 특정 박 위치에 **배치**한 것이다. 여러 클립이 한 패턴을 참조할 수 있다 |
| `mixer.inserts` | 번호가 붙은 인서트. 채널이 배정되며 저장되지만, 실제 오디오 라우팅은 아직 채널→Master 직결이다 |
| `engine` | 모델이 만들어낸 실제 엔진 클립의 읽기 전용 결과. 요청에 넣어도 무시되며, 편집이 재생에 도달했는지 확인하는 용도다 |

클립은 패턴을 참조만 하므로, 패턴을 수정하면 그 패턴의 모든 배치가 함께 바뀌고 Undo 한 번으로 모두 되돌아간다. 한 배치만 따로 바꾸려면 패턴을 새 `id`와 새 노트 `id`로 복제해 그 클립이 복제본을 가리키게 한다. `tools/cocompose.py make-unique`가 같은 일을 한다.

이전의 평평한 `tracks` 모델로 쓴 문서와 그 빌드가 저장한 세션은 열 때 자동으로 변환된다. 트랙·클립·노트의 `id`는 그대로 유지되므로 기존 스크립트의 참조가 깨지지 않는다.

JSON은 전체 원하는 상태를 나타낸다. 기존 항목을 배열에서 빼면 삭제로 해석하므로 의도하지 않은 항목은 보존한다.

- `bpm`: 30–300.
- `gain_db`: -60–6, `pan`: -1–1. 채널 페이더는 이 값이 우선한다.
- 클립 `start` / `length`: 박 단위. 노트 `start`는 패턴 내부의 상대 박 위치다.
- 노트 `pitch`: 0–127, `velocity`: 1–127. 노트는 패턴 길이를 벗어나지 않아야 한다.
- 파라미터 `value`: 0–1로 정규화한 값. `plugin_id`와 파라미터 `id`는 최신 state에서 가져온다.

현재 상한은 파일 8MB, 채널 64개, 패턴 512개, 패턴당 sequence 64개, 레인 128개, 배치 4,096개, 인서트 256개, 전체 노트 20,000개다. JSON으로 임의 코드를 실행하거나 새로운 VST를 로드하는 인터페이스는 없다. 플러그인 삽입은 UI에서 하고, 외부 AI는 노출된 파라미터를 읽고 수정한다.

## Python helper로 바로 조작하기

Python 3의 표준 라이브러리만 사용한다. 앱을 실행한 상태에서 저장소 루트의 PowerShell에서 실행한다. `--project`는 하위 명령보다 앞에 놓는다. 아래 경로는 앱에 전달한 경로와 같아야 한다. Windows 문서 폴더를 이동한 환경에서는 helper의 기본 경로에 의존하지 말고 화면 상단 경로를 명시한다.

```powershell
python tools/cocompose.py --project 'C:\project_private\my-song\project.json' inspect
python tools/cocompose.py --project 'C:\project_private\my-song\project.json' tempo 108
python tools/cocompose.py --project 'C:\project_private\my-song\project.json' play
python tools/cocompose.py --project 'C:\project_private\my-song\project.json' stop
python tools/cocompose.py --project 'C:\project_private\my-song\project.json' undo
python tools/cocompose.py --project 'C:\project_private\my-song\project.json' redo
```

추가 명령은 다음과 같다. ID는 `inspect` 결과에서 가져오며, 아래 꺾쇠 표기는 실제 값으로 치환한다.

```text
transpose <pattern_id> <semitones>
clear-notes <pattern_id>
place <lane_id> <pattern_id> <시작 박>
make-unique <clip_id>
gain <channel_id> <db>
parameter <channel_id> <plugin_id> <parameter_id> <0부터 1까지의 값>
quit
```

helper는 최신 상태를 읽고 완성된 파일을 교체한 뒤, 요청 ID에 대한 응답을 기다린다. 제어 요청도 요청 ID·session·revision으로 확인한다. `transpose`와 `clear-notes`는 패턴을 바꾸므로 그 패턴의 모든 배치에 반영된다. 한 배치만 떼어내려면 `make-unique`를 먼저 실행한다. `quit`는 앱을 종료한다.

## 반영과 오류 확인

정상 적용 시 화면 상태줄에 `Live sync`와 revision이 표시된다. `sync-status.json`의 `status`는 `synced`, `error`는 빈 문자열이어야 한다. 최종 값은 `state.json`에서 확인한다. 입력 파일에 쓴 값만 보고 성공으로 판단하지 않는다.

`applied_unpersisted`는 엔진에는 적용되었지만 파일 저장에 실패한 상태다. 변경 거절과 다르므로 같은 변경을 무작정 반복하지 않는다. 앱을 유지한 채 `error`에 나온 저장 경로·권한·디스크 상태를 확인한다. 저장 가능 상태가 복구되면 앱이 자동으로 다시 저장한다. 복구 전에는 디스크의 state가 최신이라고 가정해서는 안 된다.

잘못된 JSON, 값 범위 오류, 존재하지 않는 플러그인 파라미터, revision 충돌은 `Sync rejected`로 표시된다. 유효성 검사는 Edit 수정 전에 이루어지며, 적용 중 예외가 나면 현재 트랜잭션을 되돌린다. 오류가 있는 입력 파일은 진단할 수 있도록 보존한다. 최신 `state.json`을 바탕으로 수정한 내용을 다시 저장하면 앱을 재시작하지 않고 재시도할 수 있다.

라이브 갱신은 하나의 열린 `Edit`를 직접 수정한다. 기존 ID의 트랙·클립·노트를 찾아 갱신하며 매 저장마다 `loadEditFromFile`을 호출하거나 Edit 전체를 교체하지 않는다. 진단 파일의 `session_id`와 `edit_instance`를 비교해 동일 세션·Edit 유지 여부를 확인할 수 있다. 파일 읽기와 수정은 UI 메시지 스레드에서 실행하며 오디오 콜백에서 수행하지 않는다.

트랙 등 재생 그래프를 바꾸는 과정에서 엔진의 재생 플래그가 꺼지는 경우에는 사용자의 재생 의도를 유지해 다음 UI tick에서 재개한다. 프로젝트 재오픈은 필요 없지만, 소리가 전혀 끊기지 않는다는 보장은 아니다.

시작에 실패하면 작업 폴더의 `startup-error.txt`를 확인한다. 잘못된 저장 JSON은 네이티브 세션보다 오래된 revision 등을 포함할 수 있으므로 백업 후 실제 상태와 대조한다.

## 진단 옵션과 검증 상태

`--headless`는 창을 숨기고, `--play`는 시작과 함께 재생하며, `--screenshots`는 화면이 바뀔 때마다 UI 상태를 작업 폴더의 `ui.png`에 덮어쓰고 실제 UI 라벨을 `ui-state.json`에 기록한다. 일반 실행에는 필요 없다.

Windows MSVC 19.44.35223 Release 빌드와 실제 앱을 대상으로 한 통합 검사 15개를 통과했다. 같은 Edit에서 음악·UI 변경, 잘못된 입력 거절, undo/redo, 저장 실패 복구, 정상 종료 후 복원에 더해 한 패턴의 두 배치가 함께 바뀌고 함께 되돌아가는지, 복제한 배치가 독립하는지, 이전 모델의 세션과 JSON이 변환되는지, 패널 배치가 저장·복원되는지를 확인했다. 상세 결과는 [작업 기록](worklog.ko.md)에 있다. 실제 오디오 청취와 타사 VST3 호환성은 미검증이다.

통합 검사를 다시 실행하려면 현재 CoCompose를 먼저 정상 종료한 뒤 저장소 루트에서 다음을 실행한다. 검사는 새로운 테스트 폴더를 만들고 실제 앱을 숨긴 상태로 실행한다. 검사 중 정상 종료·재실행은 저장 복원 시험이며, 라이브 변경에 재실행이 필요한 것은 아니다.

```powershell
python tools/test_live_sync.py
```

결과 폴더는 기본적으로 `build-cocompose/live-test-<고유값>`이다. `test-report.json`, `before.png`, `after.png`, `ui-state.json`을 확인한다.
