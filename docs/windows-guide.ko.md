# Windows에서 CoCompose 실행하기

CoCompose는 DAW·VST 사용자가 외부 AI와 함께 작곡하기 위한 Windows 편집기다. 앱 안의 채팅 대신 외부 스크립트나 AI가 JSON을 수정하고, 현재 열린 프로젝트의 UI와 엔진에 그 결과를 반영한다.

## 빌드와 실행

Visual Studio 2022의 **C++를 사용한 데스크톱 개발**, Windows SDK, CMake 3.22 이상, Git이 필요하다. PowerShell에서 실행한다.

```powershell
Set-Location 'C:\project_private\tracktion_engine'
git submodule update --init --recursive
cmake -S examples/CoCompose -B build-cocompose -G 'Visual Studio 17 2022' -A x64
cmake --build build-cocompose --config Release --target CoCompose --parallel 4
& '.\build-cocompose\CoCompose_artefacts\Release\CoCompose.exe'
```

이미 빌드했다면 마지막 실행 명령만 쓰거나 탐색기에서 `build-cocompose\CoCompose_artefacts\Release\CoCompose.exe`를 더블 클릭한다. 설치 프로그램은 아직 없다.

기본 작업 파일은 Windows 문서 폴더의 `CoCompose\project.json`이다. 문서 폴더가 OneDrive 등으로 이동된 경우 화면 상단의 실제 경로를 확인한다. 폴더에 기존 작업이 없으면 내장 FourOsc 신스로 8마디 예제를 만든다. 별도 작업 폴더를 쓰려면 다음처럼 실행한다.

```powershell
& '.\build-cocompose\CoCompose_artefacts\Release\CoCompose.exe' --project 'C:\project_private\my-song\project.json'
```

동시에 실행하는 앱은 하나다. 각 곡은 별도 폴더에 보관한다. `session.tracktionedit` 등 보조 파일 이름이 폴더 단위로 고정되어 있으므로 같은 폴더에 여러 프로젝트 JSON을 두지 않는다.

C++ 소스를 수정해 다시 빌드할 때는 실행 파일 잠금을 풀기 위해 앱을 종료해야 할 수 있다. 음악 데이터 라이브 싱크는 재빌드나 앱·프로젝트 재시작 없이 동작한다. 실행 중 C++ 바이너리를 교체하는 기능은 아니다.

## 화면에서 하는 일

- `Play` / `Stop`: 현재 프로젝트 재생과 정지.
- `Undo`: 최근 편집 되돌리기. 외부 변경은 하나의 편집 트랜잭션으로 적용한다.
- `+ Track`: 내장 신스가 있는 트랙 추가.
- `Notes -1` / `Notes +1`: 선택한 MIDI 클립의 노트를 반음씩 이동.
- BPM: 현재 템포 변경.
- `Audio settings`: 출력 장치 설정.
- `Scan plugins`: 설치된 VST3 스캔. 스캔 후 트랙 하단의 플러그인 추가 UI에서 삽입한다.
- `Project folder`: 작업 파일 위치 표시.

현재 타임라인은 Tracktion 예제 컴포넌트를 사용한다. 완성형 DAW의 피아노롤·믹서·편곡 기능 전체를 제공하는 단계는 아니다.

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

지원하는 변경은 템포, MIDI 트랙·클립·노트의 추가/수정/삭제, 트랙 이름·gain/mute/solo, 이미 존재하는 플러그인의 공개 파라미터다. JSON은 전체 원하는 MIDI 상태를 나타낸다. 기존 트랙·클립·노트를 배열에서 빼면 삭제로 해석하므로 의도하지 않은 항목은 보존한다. 기존 객체의 `id`도 유지한다.

- `bpm`: 30–300.
- `gain_db`: -60–6. 트랙 페이더는 이 값이 우선한다.
- 클립 `start` / `length`: 박 단위. 노트 `start`는 클립 내부의 상대 박 위치다.
- 노트 `pitch`: 0–127, `velocity`: 1–127. 노트는 클립 길이를 벗어나지 않아야 한다.
- 파라미터 `value`: 0–1로 정규화한 값. `plugin_id`와 파라미터 `id`는 최신 state에서 가져온다.

현재 상한은 파일 8MB, 트랙 64개, 트랙당 클립 256개, 전체 노트 20,000개다. JSON으로 임의 코드를 실행하거나 새로운 VST를 로드하는 인터페이스는 없다. 플러그인 삽입은 UI에서 하고, 외부 AI는 노출된 파라미터를 읽고 수정한다.

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
transpose <clip_id> <semitones>
clear-notes <clip_id>
gain <track_id> <db>
parameter <track_id> <plugin_id> <parameter_id> <0부터 1까지의 값>
quit
```

helper는 최신 상태를 읽고 완성된 파일을 교체한 뒤, 요청 ID에 대한 응답을 기다린다. 제어 요청도 요청 ID·session·revision으로 확인한다. `clear-notes`는 해당 클립의 노트를 모두 지우며, `quit`는 앱을 종료한다.

## 반영과 오류 확인

정상 적용 시 화면 상태줄에 `Live sync`와 revision이 표시된다. `sync-status.json`의 `status`는 `synced`, `error`는 빈 문자열이어야 한다. 최종 값은 `state.json`에서 확인한다. 입력 파일에 쓴 값만 보고 성공으로 판단하지 않는다.

`applied_unpersisted`는 엔진에는 적용되었지만 파일 저장에 실패한 상태다. 변경 거절과 다르므로 같은 변경을 무작정 반복하지 않는다. 앱을 유지한 채 `error`에 나온 저장 경로·권한·디스크 상태를 확인한다. 저장 가능 상태가 복구되면 앱이 자동으로 다시 저장한다. 복구 전에는 디스크의 state가 최신이라고 가정해서는 안 된다.

잘못된 JSON, 값 범위 오류, 존재하지 않는 플러그인 파라미터, revision 충돌은 `Sync rejected`로 표시된다. 유효성 검사는 Edit 수정 전에 이루어지며, 적용 중 예외가 나면 현재 트랜잭션을 되돌린다. 오류가 있는 입력 파일은 진단할 수 있도록 보존한다. 최신 `state.json`을 바탕으로 수정한 내용을 다시 저장하면 앱을 재시작하지 않고 재시도할 수 있다.

라이브 갱신은 하나의 열린 `Edit`를 직접 수정한다. 기존 ID의 트랙·클립·노트를 찾아 갱신하며 매 저장마다 `loadEditFromFile`을 호출하거나 Edit 전체를 교체하지 않는다. 진단 파일의 `session_id`와 `edit_instance`를 비교해 동일 세션·Edit 유지 여부를 확인할 수 있다. 파일 읽기와 수정은 UI 메시지 스레드에서 실행하며 오디오 콜백에서 수행하지 않는다.

트랙 등 재생 그래프를 바꾸는 과정에서 엔진의 재생 플래그가 꺼지는 경우에는 사용자의 재생 의도를 유지해 다음 UI tick에서 재개한다. 프로젝트 재오픈은 필요 없지만, 소리가 전혀 끊기지 않는다는 보장은 아니다.

시작에 실패하면 작업 폴더의 `startup-error.txt`를 확인한다. 잘못된 저장 JSON은 네이티브 세션보다 오래된 revision 등을 포함할 수 있으므로 백업 후 실제 상태와 대조한다.

## 진단 옵션과 검증 상태

`--headless`는 창을 숨기고, `--play`는 시작과 함께 재생하며, `--screenshots`는 revision별 UI 상태를 작업 폴더의 `ui.png`에 덮어쓰고 실제 UI 라벨을 `ui-state.json`에 기록한다. 일반 실행에는 필요 없다.

Windows MSVC 19.44.35223 Release 빌드와 실제 앱을 대상으로 한 통합 검사 10개를 통과했다. 같은 Edit에서 음악·UI 변경, 잘못된 입력 거절, undo/redo, 저장 실패 복구 및 정상 종료 후 복원을 확인했다. 상세 결과는 [작업 기록](worklog.ko.md)에 있다. 실제 오디오 청취와 타사 VST3 호환성은 미검증이다.

통합 검사를 다시 실행하려면 현재 CoCompose를 먼저 정상 종료한 뒤 저장소 루트에서 다음을 실행한다. 검사는 새로운 테스트 폴더를 만들고 실제 앱을 숨긴 상태로 실행한다. 검사 중 정상 종료·재실행은 저장 복원 시험이며, 라이브 변경에 재실행이 필요한 것은 아니다.

```powershell
python tools/test_live_sync.py
```

결과 폴더는 기본적으로 `build-cocompose/live-test-<고유값>`이다. `test-report.json`, `before.png`, `after.png`, `ui-state.json`을 확인한다.
