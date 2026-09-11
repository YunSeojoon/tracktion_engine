# Windows에서 CoCompose 실행하기

CoCompose는 DAW·VST 사용자가 외부 AI와 함께 작곡하기 위한 Windows 편집기다. 외부 스크립트나 AI가 JSON을 수정하면 현재 열린 프로젝트의 UI와 엔진에 그 결과가 반영된다. 앱 안에도 곡의 일부를 붙여 물어보는 채팅 패널이 있지만, 그쪽은 읽고 답하기만 하며 곡을 바꾸지 않는다.

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

왼쪽에 Browser, 가운데 위아래로 Channel Rack과 Mixer, 그다음 위아래로 Pattern picker와 Playlist, 오른쪽 끝에 AI chat이 놓인다. 패널 사이 막대를 끌어 크기를 조절하고, View 메뉴 또는 `Alt+1`~`Alt+6`으로 각 패널을 숨기거나 되살린다. 패널 헤더 오른쪽에는 최소화·최대화·닫기 버튼이 있다. 최소화하면 헤더만 남고, 최대화는 같은 버튼으로 되돌리며, 닫은 패널은 View 메뉴로 되살린다. 셋 중 무엇을 해도 직전에 사람이 끌어 둔 분할 비율로 돌아가고, 닫힌 패널도 들고 있던 내용을 잃지 않는다. 포커스가 있는 패널은 테두리가 밝게 표시되며 `F6`으로 다음 패널로 이동한다. 패널 크기·표시 여부·현재 선택은 세션에 저장되어 다시 열 때 복원된다.

| 영역 | 하는 일 |
|---|---|
| Browser | 프로젝트의 채널·패턴·레인·인서트·사용 중인 샘플, 등록한 폴더 트리, 즐겨찾기, 최근 미리듣기, 스캔된 플러그인. 검색·미리듣기·누락 파일 찾기와 레인으로 끌어놓기 |
| Channel Rack | 채널 추가/삭제, 이름 변경, mute/solo, 볼륨·팬, 믹서 인서트 번호, 악기 선택(4OSC·샘플러·스캔된 VST3 악기)과 악기 창 열기, 스텝 길이·기준음 설정, 그리고 선택한 패턴의 16분음표 스텝 그리드 |
| Mixer | Master와 인서트별 미터·볼륨·팬·mute, 효과 체인, 센드, 출력 라우팅, 배정된 채널 수 |
| Pattern picker | 패턴 목록과 배치 횟수, 새로 만들기·복제·삭제 |
| Playlist | 마디 눈금과 레인별 행으로 된 편곡 격자. 클립 배치·이동·크기 조절·복제·분할·삭제, 루프 범위 지정, 확대 |
| AI chat | 물어볼 대상을 붙여 두는 첨부 카드, 대화 기록, 입력란. 자세한 내용은 [앱 안에서 AI에게 묻기](#앱-안에서-ai에게-묻기) |

상단 툴바는 재생/정지, Song·Pattern 루프 전환, BPM, 마디:박 위치, 메트로놈, CPU 사용률, 포커스된 패널을 표시한다. 메뉴는 File(저장·복사본 저장·폴더 열기·종료), Edit(undo/redo, 채널·패턴 추가, 패턴 배치, 배치 독립화, 반음 이동), View(패널 표시), Tools(플러그인 스캔·오디오 설정), Help로 구성된다.

단축키는 `Space` 재생/정지, `Ctrl+L` Song·Pattern 전환, `Ctrl+M` 메트로놈, `Ctrl+S` 저장, `Ctrl+Z`/`Ctrl+Shift+Z` undo/redo, `Ctrl+T` 채널 추가, `Ctrl+P` 패턴 추가, `Ctrl+B` 패턴 배치, `Ctrl+U` 선택 배치 독립화, `Ctrl+↑`/`Ctrl+↓` 패턴 반음 이동이다.

### 스텝 그리드와 피아노롤

Channel Rack 오른쪽 격자는 선택한 패턴의 16분음표 스텝이다. 누르거나 끌면 채널의 `step_pitch` 음을 `step_length` 길이로 쓰고, 켜진 스텝을 다시 누르면 지운다. 인서트 번호 옆 버튼에서 스텝 길이(1/16~1마디)와 기준음을 고른다. 패턴을 바꾸면 격자도 그 패턴의 내용으로 바뀐다.

`Piano roll` 버튼은 선택한 채널과 패턴의 노트 편집기를 연다. 빈 칸을 누르면 노트를 추가하고, 끌면 이동, 노트 오른쪽 끝을 끌면 길이 조절, Alt를 누른 채 끌면 velocity를 바꾼다. 오른쪽 버튼은 삭제, 배경을 끌면 사각형 선택이다. `Ctrl+D` 복제, `Q` 퀀타이즈, `Delete` 삭제, `Ctrl+A` 전체 선택을 쓴다. 스냅과 확대는 위쪽 컨트롤로 정하고, 왼쪽 건반을 누르면 채널의 실제 악기로 미리듣기한다. 아래 VELOCITY 칸은 노트별 세기를 보여준다.

패턴은 공유되므로 스텝이든 피아노롤이든 편집은 그 패턴의 모든 배치에 반영되고 Undo 한 번으로 함께 되돌아간다.

### 편곡하기

Playlist는 가로가 마디, 세로가 레인인 격자다. Pattern picker에서 패턴을 끌어다 레인에 놓거나, 빈 칸을 누르면 선택한 패턴이 그 자리에 놓인다.

- 클립을 끌면 마디와 레인을 옮기고, 오른쪽 끝을 끌면 길이를 바꾼다. Ctrl을 누른 채 끌면 복사한다.
- 오른쪽 버튼은 삭제, Alt를 누른 채 배경을 끌면 사각형 선택이다.
- 클립을 더블클릭하면 그 지점에서 자른다. 뒤쪽 조각은 패턴의 이어지는 부분을 재생하므로 소리가 그대로 이어진다.
- `Ctrl+E`는 재생 커서 위치에서 자르고, `Ctrl+R`은 선택한 클립을 바로 뒤에 반복하며, `Ctrl+U`는 그 배치만 독립시킨다. `Delete`는 삭제한다.
- 눈금자를 끌면 루프 범위를 정한다. Ctrl+휠은 확대·축소, 아래쪽 Snap은 격자 단위를 정한다.

패턴보다 짧은 클립은 패턴의 일부만, 긴 클립은 패턴을 반복해 재생한다. 클립 안의 세로 실선이 패턴이 다시 시작하는 지점이다.

Song 모드는 편곡 전체를, Pattern 모드는 선택한 패턴의 첫 배치를 반복한다.

### 샘플과 오디오 클립

Browser 아래 `+`로 샘플 폴더를 등록하면 트리에 나타난다. 폴더는 누를 때만 펼쳐지므로 큰 라이브러리를 등록해도 느려지지 않는다. 위쪽 상자로 이름을 검색하고, `Play`로 선택한 샘플을 미리듣고, `*`로 즐겨찾기에 넣는다. 미리듣기한 파일은 Recent에 남는다. 이 목록들은 세션에 저장된다.

샘플을 Playlist 레인으로 끌어놓거나 탐색기에서 직접 끌어놓으면 오디오 클립이 된다. 클립은 패턴 클립과 똑같이 이동·크기 조절·분할·복제·삭제할 수 있고, 파형과 페이드 모양이 함께 그려진다. 크기를 줄이면 트림, `offset`을 주면 파일 뒷부분부터 재생한다. 게인·페이드 인/아웃·속도는 외부 편집이나 `--ui-script`의 `shape`로 조절한다.

파일이 사라지면 Browser의 Project samples와 클립이 빨갛게 표시되고, `Find` 버튼으로 파일을 하나 찾아주면 같은 폴더에서 나머지도 함께 다시 연결한다.

`File > Collect samples`는 프로젝트가 쓰는 모든 샘플을 세션 옆 `samples` 폴더로 복사하고 클립이 그 사본을 가리키게 한다. 폴더 전체를 그대로 옮겨도 소리가 유지된다.

오디오 파일이 아닌 파일을 지정한 요청은 프로젝트를 건드리기 전에 거절한다. 파일이 아직 없는 경우는 거절하지 않고 `missing`으로 표시한다.

### 믹서

채널은 자기 인서트 번호의 스트립으로 흐르고, 인서트는 `output`이 가리키는 곳으로, 마지막에는 Master로 간다. 스트립의 `FX` 버튼에서 효과를 넣고, 창을 열고, bypass하고, 순서를 바꾸고, 지우고, 다른 인서트로 보내는 센드를 만든다. 그 아래 버튼이 출력 대상을 고른다. 자기 자신으로 돌아오는 라우팅은 거절한다.

효과는 내장 여섯 가지(EQ, limiter, saturation, delay, chorus, reverb)와 스캔된 VST3 효과다. saturation만 직접 만들었고 나머지 내장 효과는 엔진 플러그인이다. 인서트의 `FX` 버튼이 둘을 함께 보여주며, 프로젝트가 이 기계에 없는 플러그인을 요구하면 그 칸만 비고 나머지는 그대로 열린다. `wet` 값은 해당 효과에 wet 또는 mix 파라미터가 있을 때 적용된다. 효과의 공개 파라미터는 채널 파라미터와 같은 방식으로 `state.json`에 나오고 외부에서 바꿀 수 있다.

각 스트립의 세로 막대는 실제 출력 레벨이다. 체인 순서를 바꿔도 플러그인은 다시 만들어지지 않고 자리만 옮기므로 설정이 유지된다.

### 자동화

`state.json`에 나오는 파라미터라면 곡선을 만들 수 있다. 곡선은 모델이 갖고 엔진 자동화는 거기서 다시 만들어지므로, 사람이 만들든 외부 AI가 만들든 같은 데이터를 고친다. 점이 없는 파라미터는 마지막 값을 유지한다.

Playlist 아래쪽 `Automate` 버튼이 선택한 채널의 악기·페이더와 그 채널이 지나는 인서트의 효과 파라미터를 보여준다. 하나를 고르면 편곡 아래에 그 파라미터의 곡선 줄이 생긴다.

곡선 줄에서 빈 곳을 누르면 점이 생기고, 끌면 시간과 값이 함께 움직이며, 오른쪽 버튼이나 `Delete`로 지운다. 스냅과 확대는 편곡과 같은 설정을 쓴다. 같은 버튼의 `Remove automation`에서 곡선 자체를 없앤다.

채널 행의 `P` 버튼은 그 악기의 설정을 이름을 붙여 저장하고 다시 불러온다. 프리셋은 사용자 앱 데이터 폴더의 `CoCompose\presets`에 쌓이며 같은 종류의 악기에만 적용된다. Tools 메뉴에도 같은 두 명령이 있다.

Edit 메뉴의 Undo/Redo는 무엇을 되돌리는지 함께 보여준다.

자동화가 걸린 파라미터는 재생 중 계속 움직이므로, 그동안에는 플러그인 내부 상태 변화 감지를 하지 않는다. 그렇지 않으면 revision이 2초마다 올라가 외부 편집이 계속 충돌한다. 플러그인 창에서 손으로 돌린 값은 재생을 멈췄을 때 반영된다.

자동화와 사람·AI 편집의 우선권: 곡선이 있는 파라미터는 곡선이 값을 정한다. 모델에 저장된 값(예: 채널 볼륨)은 그 파라미터에 곡선이 없을 때만 기록되므로, 편집과 자동화가 같은 컨트롤을 두고 서로 덮어쓰지 않는다. 곡선을 지우면 파라미터는 다시 모델의 값을 따른다.

### 녹음

`Tools > Arm channel for recording`가 사용 가능한 입력 장치를 선택한 채널의 트랙에 연결한다. `Ctrl+Shift+R`로 녹음을 시작하고 다시 눌러 멈춘다. `Count in one bar`를 켜면 한 마디 클릭 후 시작한다.

멈추면 녹음된 내용을 모델이 가져간다. MIDI는 새 패턴이 되어 연주한 위치에 배치되고, 오디오는 오디오 클립이 된다. 그래서 녹음 결과도 다른 것과 똑같이 편집·이동·분할된다.

멈추는 경로는 하나다. 화면의 Play/Stop, `Ctrl+Shift+R`, 외부 `control.json`의 `stop`, 창을 닫는 것 모두 같은 종료를 지나므로 어느 쪽으로 멈춰도 take를 잃지 않는다. 외부에서 `record`를 보내 녹음을 시작·중지할 수도 있고, 녹음 중인지는 `sync-status.json`의 `recording`으로 확인한다.

### 백업과 복구

작업 폴더의 `backups`에 세션 사본이 쌓인다. 내용이 바뀌는 동안 1분에 한 번, 그리고 take를 보관했을 때와 앱을 닫을 때는 즉시 쓴다. 최신 10개를 남기고 오래된 것을 지운다.

`File > Restore a backup...`에서 하나를 고르면 그것이 다음에 열릴 세션이 된다. 현재 세션은 지우지 않고 `backups`에 함께 남긴다. 열려 있는 프로젝트를 도중에 바꿔치기하지 않는다.

세션 파일이 사라졌거나 열리지 않으면 시작할 때 가장 최근의 열리는 백업으로 복구하고, `sync-status.json`의 `recovered_from`에 어느 파일이었는지 남긴다. 백업 목록은 `backups`, 클립이 찾지 못하는 샘플은 `missing_assets`에 나오며 상태줄에도 표시된다.

### 출력

`File > Export WAV`는 편곡 전체를, 편곡 범위 안에 루프가 설정되어 있으면 그 구간만 24비트 WAV로 쓴다. `File > Export stems`는 채널마다 파일 하나씩 쓰며, 각 채널의 인서트와 그 출력 경로를 함께 렌더하므로 그 채널의 실제 소리가 나온다.

렌더는 시작 시점의 프로젝트 사본을 별도 Edit로 열어 별도 스레드에서 돌린다. 그래서 렌더 도중에도 편집·Undo가 그대로 되고, 그 편집은 다음 렌더부터 반영된다.

결과 파일은 옆에 임시로 쓴 뒤 완성된 것을 확인하고 나서야 교체한다. 렌더가 실패해도 이전에 정상적으로 뽑아둔 파일은 사라지지 않는다.

진행 상황은 작업 폴더의 `render-status.json`에 남는다. `running`이 false가 되면 끝난 것이고, `revision`은 어느 시점의 프로젝트를 렌더했는지, `complete`는 요청한 파일을 모두 썼는지, `files`는 결과 경로다.

스템 파일 이름은 렌더를 시작하기 전에 모두 정하고 겹치면 번호를 붙인다. 이름이 같은 채널이 둘 있어도 서로 덮어쓰지 않는다. 스템은 그 채널의 인서트와 출력 경로에 더해 그 채널이 보내는 send 대상까지 함께 렌더한다. wet 버스는 여러 채널이 공유하므로 개별 스템을 더한 것이 전체 믹스와 같지는 않다.

### 적용 결과 확인과 충돌 처리

`sync-status.json`의 `change`가 방금 적용된 요청이 실제로 무엇을 바꿨는지 알려준다. 섹션별로 추가·삭제·변경된 ID 목록과 템포 변경 여부가 들어 있으며, 입력 파일을 그대로 되풀이한 것이 아니라 적용 전후의 엔진 상태를 비교한 결과다. 자기가 쓴 파일이 아니라 이 값을 확인한다.

오래된 revision이나 끝난 session을 가리키는 요청은 프로젝트를 건드리지 않고 거절된다. 그러면 `state.json`을 다시 읽고 현재 상태 위에 수정을 다시 얹는다. `tools/cocompose.py`의 `apply_change`가 그 절차를 대신한다.

```python
from cocompose import apply_change

def busier(state):
    notes = state["patterns"][0]["sequences"][0]["notes"]
    notes.append({"id": "extra", "pitch": 38, "velocity": 90, "start": 2.0, "length": 0.25})

state, _ = apply_change(r"C:\song\project.json", busier)
```

최신 상태를 읽어 수정 함수를 적용하고 제출하며, 그 사이 누군가 먼저 편집했으면 처음부터 다시 읽어 새로 얹는다. 오래된 스냅샷을 새 상태 위에 덮어쓰지 않는다.

요청 하나는 무엇을 바꿨든 Undo 하나다. 사람이 변경을 듣고 한 번에 되돌릴 수 있다.

MIDI learn과 하드웨어 컨트롤 서피스 연결은 아직 없다.

## 앱 안에서 AI에게 묻기

오른쪽 끝 AI chat 패널에서 지금 보고 있는 곡의 일부를 붙여 질문할 수 있다. **답을 만드는 것은 앱이 아니다.** 앱은 질문을 프로젝트 폴더에 파일로 적고, 따로 띄운 bridge 프로그램이 그것을 집어 모델에 묻고 답을 적는다. API 키는 bridge 프로세스에만 있으며 앱·프로젝트·대화 기록·로그 어디에도 들어가지 않는다.

답이 곡을 고치자고 제안할 수는 있지만, **제안이 곧 편집은 아니다.** 제안은 어떤 노트가 무엇에서 무엇이 되는지를 숫자로 보여 주는 데까지만 가고, 실제로 곡이 바뀌는 것은 사람이 `Apply change`를 누를 때뿐이다. 그렇게 적용한 것은 무엇을 몇 개 건드렸든 **Undo 한 번**으로 되돌아온다.

### 물어볼 대상 붙이기

질문 전에 무엇에 대한 질문인지부터 붙인다. 단축키는 셋이다.

| 단축키 | 붙는 것 |
|---|---|
| `Ctrl+K` | Playlist에서 선택한 클립 또는 루프 범위, 즉 편곡의 한 구간 |
| `Ctrl+Shift+K` | 피아노롤에서 선택한 노트 |
| `Ctrl+Alt+K` | Mixer에서 선택한 인서트 |

붙일 것이 없으면 상태 표시줄이 무엇을 먼저 골라야 하는지 알려 준다.

첨부는 **붙이는 순간의 상태로 고정된다.** 이후 다른 곳을 클릭해도 첨부는 그대로 같은 마디, 같은 노트, 같은 인서트를 가리킨다. 채널 이름이 바뀌거나 템포가 바뀌어도 가리키는 대상은 움직이지 않는다. 카드의 버튼은 셋이다.

- `Go` — 그 음악이 있는 자리로 화면을 옮긴다.
- `Update` — 지금 선택한 것으로 첨부를 다시 가져온다. 첨부가 가리키는 대상이 바뀌는 것은 이 버튼을 누를 때뿐이다.
- `Remove` — 뗀다.

첨부가 가리키던 클립이나 노트를 지우면 카드가 더 이상 존재하지 않는다고 표시한다. `What gets sent` 버튼은 지금 붙어 있는 것이 실제로 무엇을 담고 있는지 보여 주고, `Clear`는 전부 뗀다.

첨부·이동·제거와 패널 최소화·최대화·닫기는 모두 곡을 건드리지 않는다. revision이 오르지 않고 undo 이력에도 남지 않는다. 편집 도중에 질문해도 그 질문이 편집의 일부가 되지 않는다.

### bridge 실행하기

앱과 별개로 PowerShell 창을 하나 더 열어 실행한다. `--project`는 앱에 넘긴 경로와 같아야 한다. Python 3만 있으면 되고 추가 패키지는 필요 없다.

```powershell
python tools/cocompose_bridge.py --project 'C:\project_private\my-song\project.json' --provider echo
```

`echo`는 모델이 아니다. 네트워크를 쓰지 않고, 모델에 보내졌을 프롬프트를 그대로 되돌려주며, 모든 답변에 자기가 모델이 아니라고 적는다. 앱과 bridge 사이의 배선이 살아 있는지 확인하는 용도다.

실제 모델에 물으려면 `openai` provider를 쓴다. 키는 환경 변수에서만 읽는다.

```powershell
$env:OPENAI_API_KEY = '<키>'
python tools/cocompose_bridge.py --project 'C:\project_private\my-song\project.json' --provider openai
```

`--model`로 모델 이름을, `--key-name`으로 키를 담은 환경 변수 이름을 바꿀 수 있다. `--once`는 한 번 답하고 끝낸다. bridge를 끄면 앱이 곧 알아차린다.

채팅 패널 아래쪽에 현재 연결 상태가 적힌다. `connected: echo (no model, plumbing only)`처럼 어느 provider가 듣고 있는지 함께 보여 주므로, echo로 받은 답을 실제 모델의 답으로 오해할 일은 없다. 듣는 것이 없으면 `no bridge running`이다.

### 묻기

입력란에 질문을 쓰고 `Ask`를 누른다. 기다리는 동안 재생은 계속되고 편집도 계속할 수 있다. 답이 오지 않는 것과 앱이 멈추는 것은 다른 일이며, 모델과의 통신은 이 앱 안에서 일어나지 않는다. `Stop`은 기다리기를 그만둔다.

bridge가 돌고 있지 않으면 질문은 보내지지 않고, **입력란에 쓴 글은 그대로 남는다.** 취소했을 때도, 실패했을 때도 마찬가지다. 입력란은 질문이 실제로 나갔을 때만 비워진다.

대화는 프로젝트에 묶여 있다. 앱을 닫았다 열어도, 패널을 닫았다 열어도 이어진다. 기록은 프로젝트 폴더의 `conversation.json`에 있고 곡 파일과는 별개다. 지우거나 다른 곳에 복사해도 곡에는 아무 영향이 없다.

### 제안된 변경 읽고 적용하기

모델은 평소처럼 말로 답하고, 바꾸자고 할 때만 답 끝에 ` ```cocompose-change ` 블록을 하나 붙인다. bridge가 그 블록을 들어내 앱에 넘기므로 **화면에는 문장만 남고 JSON은 보이지 않는다.** 블록이 없으면 그냥 답이고, 블록이 깨져 있거나 바꿀 것이 없으면 변경은 없는 것으로 친다. 반쯤 이해한 편집이 곡에 얹히는 것보다 버튼 없는 답이 낫기 때문이다.

답에 변경이 딸려 오면 대화 아래에 블록이 하나 생긴다. 무엇을 하려는지, 노트 몇 개가 바뀌고 몇 개가 더해지고 몇 개가 지워지는지, 그리고 노트마다 `pitch 60 -> 62`처럼 **지금 값과 바뀔 값**이 함께 나온다. 약속이 아니라 숫자를 보고 결정하라는 뜻이다.

마음에 들면 `Apply change`를 누른다. 누르기 전까지 곡은 전혀 바뀌지 않는다. 제안이 대기하고 있는 것은 편집이 아니므로 revision도 오르지 않는다.

적용된 것은 **Undo 한 번**으로 전부 돌아온다. 노트를 여섯 개 바꿨든 파라미터까지 함께 건드렸든 한 번이다. Redo도 마찬가지다. 이미 적용한 제안은 블록에 `applied`로 표시되고 버튼이 사라지므로 같은 변경이 두 번 들어갈 일은 없다.

제안은 만들 때 한 번, 적용하는 순간 다시 한 번 검사된다. 그래서 다음의 경우 거절되고, 거절된 이유가 같은 자리에 나온다.

| 거절 | 언제 |
|---|---|
| `OUT_OF_SCOPE` | 첨부하지 않은 노트나 인서트를 건드리려 할 때, 또는 노트가 패턴 끝을 넘어갈 때 |
| `LOCKED` | 지키기로 한 것(리듬·음높이·벨로시티)을 어길 때, 또는 자동화 곡선이 걸린 파라미터를 바꾸려 할 때 |
| `STALE_REVISION` | 제안을 만든 뒤 곡이 움직였을 때. 사람이 그 사이에 편집했거나 Undo했으면 여기서 걸린다 |
| `NOT_FOUND` | 바꾸려던 노트가 이미 없을 때 |

`STALE_REVISION`으로 거절됐으면 다시 물으면 된다. 거절은 곡을 절반쯤 바꿔 놓고 멈추지 않는다. 하나라도 어긋나면 아무것도 쓰지 않는다.

바꿀 수 있는 것은 **첨부 안의 노트와 파라미터뿐**이다. 노트의 음높이·시작 위치·길이·벨로시티, 노트 추가와 삭제, 그리고 공개 파라미터 값. 클립을 옮기거나 패턴을 새로 만들거나 효과를 붙이거나 라우팅을 바꾸는 것은 제안할 수 없다.

믹서 파라미터도 같은 규칙을 따른다. 다만 노트는 첨부한 패턴과 채널이 울타리가 되어 주는 반면 파라미터에는 그런 것이 없으므로, **`Ctrl+Alt+K`로 인서트를 붙여야만** 그 인서트를 건드리는 제안이 성립한다. 아무 인서트도 붙이지 않은 채 파라미터를 바꾸자는 제안이 오면 `OUT_OF_SCOPE`로 거절된다. 노트만 붙여 놓고 물었는데 답이 페이더를 움직이자고 하는 일은 없다는 뜻이다.

파라미터 변경은 노트 변경과 **같은 하나의 Undo**에 들어간다. 한 제안이 노트와 페이더를 함께 건드렸어도 Undo 한 번이면 둘 다 돌아온다.

### 지금 할 수 없는 것

이 항목은 "아직 안 되는 것"이 아니라 **확인되지 않았거나 존재하지 않는 것**이다.

- **실제 모델로 끝까지 확인되지는 않았다.** 이 환경에서 실제 API를 부른 적이 없다. 자동 검사가 증명한 것은 앱과 bridge 사이의 배선, 그리고 실제 답이 지나갈 블록 읽기까지다. 진짜 모델이 규약대로 블록을 쓰는지는 **키를 가진 사용자가 실연결로 직접 확인할 몫**이다.
- echo가 내놓는 변경은 음악적 판단이 아니라 첨부된 노트를 온음 올리는 기계적인 것이다. 모델 없이 검사·적용 경로를 돌려 보기 위한 것이므로 제안의 본보기로 읽지 않는다.
- **제안할 수 있는 것은 첨부 안의 노트와 파라미터뿐이다.** 클립 이동, 패턴 생성, 효과 추가, 라우팅 변경은 제안 대상이 아니다.
- **오디오는 보내지 않는다.** 어떤 경로로도 소리가 나가지 않으므로 모델은 곡이 어떻게 들리는지 알 수 없다. 구조·노트·레벨·라우팅에 대해서만 답한다.
- 새 대화로 분기하는 버튼은 화면에 없다. 진단용 `--ui-script`로만 할 수 있다.
- 답은 `echo`에서만 조각으로 나뉘어 들어온다. `openai`는 답이 다 온 뒤 한 번에 표시된다.

### 채팅이 쓰는 파일

프로젝트 폴더에 다음이 생긴다. 전부 프로젝트 옆에 있을 뿐 곡 파일의 일부가 아니다.

| 파일 | 역할 |
|---|---|
| `conversation.json` | 이 프로젝트의 대화 기록. `project_id`에 묶이므로 앱을 다시 열어도 이어진다 |
| `chat-request.json` | 앱이 적은 질문. bridge가 읽는다 |
| `chat-reply.json` | bridge가 적은 답. 앱이 읽는다 |
| `chat-cancel.json` | 기다리기를 그만두겠다는 요청 |
| `chat-bridge.json` | bridge가 듣고 있는지와 어느 provider인지 |
| `chat-inspector.json` | 지금 붙어 있는 첨부를 그대로 적은 읽기 전용 결과물. 앱이 쓰기만 하고 읽지 않는다 |

`session.tracktionedit`에는 `project_id`가 함께 저장된다. 실행마다 바뀌는 `session_id`와 달리 폴더를 옮기거나 이름을 바꿔도 유지되며, 대화가 묶이는 대상이 이것이다.

### 도구를 스크립트에서 직접 부르기

채팅 패널이 곡을 읽을 때 쓰는 것과 **같은 서비스**를 밖에서도 부를 수 있다. 앱이 실행 중일 때 `tool-request.json`을 쓰면 앱이 `tool-response.json`으로 답하며, helper에 그 명령이 있다.

```powershell
python tools/cocompose.py --project 'C:\project_private\my-song\project.json' tool get_capabilities
python tools/cocompose.py --project 'C:\project_private\my-song\project.json' tool inspect_region --arg start_beat=32 --arg end_beat=64
```

| 도구 | 인자 | 답하는 것 |
|---|---|---|
| `get_capabilities` | 없음 | 이 빌드가 실제로 제공하는 도구, 단위, 상한, 오디오 전송 가능 여부 |
| `get_selection` | 없음 | 현재 선택된 채널·패턴·레인·인서트 |
| `inspect_region` | `start_beat`, `end_beat`, 선택적으로 `lanes`, `context_beats` | 그 구간의 클립과, `context_clips`로 구분된 양옆의 맥락 |
| `inspect_insert` | `insert` | 인서트의 효과 순서, 공개 파라미터, 센드, 출력, 배정된 채널 |
| `inspect_pattern` | `pattern`, 선택적으로 `channel` | 패턴의 노트와 그 패턴이 배치된 횟수 |

쓰기 도구는 셋이며 채팅 패널의 `Apply change`가 지나는 것과 같은 경로다.

| 도구 | 인자 | 하는 일 |
|---|---|---|
| `create_proposal` | `pattern`, `channel`, `allowed_notes`, `allowed_inserts`, `base_revision`, `keeps`, `notes`, `parameters` | 변경을 만들어 두기만 한다. 곡은 바뀌지 않고 `diff`로 무엇이 무엇이 될지 돌려준다 |
| `get_proposal` | `proposal` | 만들어 둔 제안과 그 `diff`를 다시 본다 |
| `apply_proposal` | `proposal` | 전부 다시 검사한 뒤 한 번의 Undo로 적용한다. 두 번 불러도 `already_applied` |

`notes` 항목은 `{"what": "change"|"add"|"remove", "id": ..., "pitch"/"start_beat"/"length_beats"/"velocity": ...}`, `parameters` 항목은 `{"owner": "<인서트 id>", "plugin": "<효과 id>", "parameter": "<파라미터 id>", "value": 0~1}`이다. 세 ID 모두 `inspect_insert`가 내놓은 것을 그대로 쓴다. `allowed_inserts`가 비어 있으면 어떤 인서트도 건드릴 수 없다. "전부 허용"이 아니라 "하나도 허용하지 않음"이다.

바꿀 수 있는 것의 전부는 `get_capabilities`의 `writes`에 있다. `note.pitch`·`note.start_beat`·`note.length_beats`·`note.velocity`·`note.add`·`note.remove`·`parameter.value`이며, 여기 없는 것은 어떻게 요청해도 거절된다. `preview_proposal`·`get_operation`·`cancel_operation`은 계약에 이름만 있고 `UNSUPPORTED`로 거절되며 `get_capabilities` 목록에도 나오지 않는다.

거절은 항상 정해진 단어로 온다. `INVALID_ARGUMENT`, `NOT_FOUND`, `STALE_REVISION`, `OUT_OF_SCOPE`, `LOCKED`, `UNSUPPORTED`, `CANCELLED`, `IO_ERROR`. 옆의 문장은 사람이 읽기 위한 것이므로 분기에 쓰지 않는다. 같은 `request_id`로 다시 물으면 앱이 이미 정한 답을 그대로 돌려준다.

요청과 응답의 정확한 모양은 [`docs/ai-tool-contract.schema.json`](ai-tool-contract.schema.json)에 있다.

### 채팅 관련 검사

앱을 종료한 상태에서 저장소 루트에서 실행한다. 검사는 새 폴더에 실제 앱을 띄운다.

```powershell
python tools/test_ai_chat.py --output build-cocompose/chat-test
python tools/test_tool_contract.py --output build-cocompose/tool-test
```

앞의 것은 세 종류의 첨부와, 첨부·이동·제거·패널 조작이 revision을 건드리지 않는다는 것, `project_id`가 재시작을 넘고 `session_id`는 넘지 않는다는 것을 확인한다. 제안도 같은 검사가 다룬다. 첨부 밖의 노트와 지키기로 한 것을 어기는 변경이 거절되고 그 거절이 곡을 건드리지 않는다는 것, 적용하면 지목한 노트만 바뀌고 나머지는 그대로라는 것, 제안을 만든 뒤 사람이 편집하면 적용이 `STALE_REVISION`으로 거절된다는 것, 그리고 붙이고 묻고 `Apply`를 눌러 Undo 한 번으로 되돌리는 과정 전체를 실제 패널로 지나간다. 답에서 `cocompose-change` 블록을 읽어 내는 부분도 여기서 따로 확인한다. 블록이 없는 답은 그대로 두고, 깨진 블록과 아무것도 바꾸지 않는 블록은 변경으로 치지 않는다. 믹서 제안도 한 벌 있다. `inspect_insert`가 준 ID가 그대로 통하는지, 붙이지 않은 인서트가 거절되는지, 적용한 파라미터가 Undo 한 번으로 돌아오는지를 본다. 뒤의 것은 앱의 실제 답을 스키마와 대조하고, 잘못된 요청이 약속한 단어로 거절되며 아무것도 바꾸지 않는지, 그리고 앱 안의 채팅과 밖의 스크립트가 같은 질문에 같은 답을 받는지 확인한다. `test_tool_contract.py`는 `jsonschema` 패키지를 사용하므로 다른 검사와 달리 표준 라이브러리만으로는 실행되지 않는다.

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
| `channels` | 악기 채널. `gain_db`, `pan`, `mute`, `solo`, 믹서 `insert` 번호, 연주할 `instrument`(`4osc`·`sampler`·스캔된 플러그인 식별자), 샘플러용 `sample` 경로, 스텝 그리드가 쓰는 `step_pitch`·`step_length`, 플러그인 공개 `parameters` |
| `patterns` | 이름이 붙은 노트 묶음. 채널마다 `sequences` 항목 하나를 가지며 각 항목에 `notes`가 들어간다 |
| `playlist.lanes`, `playlist.clips` | 클립은 패턴을 레인의 특정 박 위치에 **배치**한 것이다. 여러 클립이 한 패턴을 참조할 수 있다. `offset`은 패턴의 어느 지점부터 재생할지를 뜻하며, 패턴보다 짧은 클립은 그 일부만, 긴 클립은 패턴을 반복해 재생한다 |
| `playlist.audio` | 레인 위의 오디오 클립. 파일 경로, `start`, `length`, 파일 안에서의 `offset`, `gain_db`, `fade_in`, `fade_out`, `speed`를 갖는다. `missing`은 파일을 찾지 못했다는 뜻이다 |
| `automation.curves` | 파라미터별 자동화 곡선. 플러그인을 가진 채널 또는 인서트를 가리키는 `source`, `plugin_id`, `parameter`와 `points`(박 단위 `time`, 0–1 `value`, `curve` 모양)를 갖는다. `engine_points`는 엔진이 실제로 재생 중인 점의 수다 |
| `mixer.inserts` | 번호가 붙은 인서트. 페이더·팬·mute와 함께, 어디로 보낼지를 뜻하는 `output`(다른 인서트 ID 또는 `master`), 순서가 있는 `effects` 체인, 신호 사본을 다른 인서트로 보내는 `sends`를 갖는다 |
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

`--scale <배율>`은 화면 배율을 지정해 100%/150%/200% 배치를 확인한다. `--headless`는 창을 숨기고, `--play`는 시작과 함께 재생하며, `--screenshots`는 화면이 바뀔 때마다 UI 상태를 작업 폴더의 `ui.png`(열려 있으면 `piano-roll.png`도)에 덮어쓰고 실제 UI 라벨을 `ui-state.json`에 기록한다.

`--ui-script <파일>`은 기록된 화면 조작을 실제 패널에 250ms 간격으로 재생한다. JSON 배열이며 `{"command": "<메뉴 항목 이름>"}`, `{"select_channel": <번호>}`, `{"select_pattern": ...}`, `{"select_lane": ...}`, `{"step": [<채널 번호>, <스텝 번호>]}`, `{"note": [<pitch>, <시작 박>, <길이 박>, <velocity>]}`, `{"place": [<레인 번호>, <시작 박>]}`, `{"pick_clip": [<레인 번호>, <박>]}`, `{"move_clip": [<이동 박>, <이동 레인 수>]}`, `{"split_clip": <박>}`, `{"audio": [<레인 번호>, "<파일 경로>", <시작 박>]}`, `{"shape": ["gain"|"fade_in"|"fade_out"|"speed"|"offset"|"length", <값>]}`, `{"arm": [<채널 번호>, true|false]}`, `{"automation": [<채널 번호>, "<plugin_id>", "<parameter>", <박>, <값>, ...]}`, `{"export": "mix"|"stems"}`, `{"automate": [<채널 번호>, "<plugin_id>", "<parameter>"]}`, `{"curve_click": [<곡선 번호>, <박>, <값>]}`, `{"curve_drag": [...]}`, `{"curve_remove": [<곡선 번호>, <박>]}`, `{"take": [<채널 번호>, <시작 박>, <길이 박>, <pitch>, ...]}`, `{"keep_takes": true}`를 받는다. 뒤의 둘은 MIDI 입력 장치 없이 녹음 결과를 모델이 가져가는 경로를 확인하기 위한 것이다. 진행 상황과 회차는 `ui-script-status.json`에 기록되며, 파일을 다시 쓰면 같은 세션에서 다음 단계를 이어서 재생한다. 화면이 실제로 동작하는지 검사하기 위한 진단 기능이며 라이브 싱크 규약의 일부가 아니다.

이 옵션들은 일반 실행에 필요 없다.

Windows MSVC 19.44.35223 Release 빌드와 실제 앱을 대상으로 한 통합 검사 26개를 통과했다. 배포 ZIP에서 꺼낸 실행 파일로도 전부 통과했다. 같은 Edit에서 음악·UI 변경, 잘못된 입력 거절, undo/redo, 저장 실패 복구, 정상 종료 후 복원에 더해 한 패턴의 두 배치가 함께 바뀌고 함께 되돌아가는지, 복제한 배치가 독립하는지, 이전 모델의 세션과 JSON이 변환되는지, 패널 배치가 저장·복원되는지, 그리고 드럼·베이스·멜로디 3채널 패턴을 스텝 그리드와 피아노롤만으로 작성해 재생·저장·복원하고 외부 노트 수정이 열린 편집기에 반영되는지, 그리고 32마디 편곡을 격자에서 배치·이동·분할하고 Undo한 결과가 소리·화면·state에 일치하는지, WAV를 배치해 트림·페이드·속도를 주고 자산을 모아 폴더째 옮겨도 재생되는지, 3악기가 각자 인서트를 거쳐 드럼 bus와 reverb 센드를 지나 Master에 도달하는지, 필터 자동화가 엔진에 반영되고 믹스·스템 WAV가 무음도 클리핑도 아닌 실제 오디오로 나오는지, 그리고 외부 에이전트가 재생 중인 32마디 곡에서 드럼 변형·베이스 생성·구간 재배치·믹스 조정을 각각 Undo 하나로 수행하는지, 모든 메뉴 명령이 실제로 동작하는지, 150%·200% 배율에서 화면이 배치되는지, 설치되지 않은 플러그인과 비정상 종료를 견디는지를 확인했다. 상세 결과는 [작업 기록](worklog.ko.md)에 있다. 이와 별도로 이 기계에 설치된 VST3 23개(악기 12, 효과 11)와 실제 오디오 인터페이스로 스캔·로드·창·자동화·렌더·재시작 복원, 100/150/200% 배율, 25회 반복 저장·Undo, 65분 연속 재생을 확인했다. 표는 [호환성 표](compatibility-2026-09-10.ko.md)에 있다. 실제 오디오 청취, 재생 중 장치 전환, 다른 오디오 인터페이스, VST2·AU는 여전히 미검증이다.

GitHub Actions는 수동 실행(`Run workflow`)으로만 돈다. push마다 자동으로 돌지 않는다. 실행하면 빌드 후 ZIP을 풀어 통합 검사 26개, 배포본 시작 검사, 설치·업데이트·제거 검사를 모두 통과해야 artifact를 올린다. 통과하면 검사 보고서와 문서를 ZIP의 `verification/`에 넣고 그 상태의 해시를 `SHA256SUMS.txt`에 적는다. 봉인은 보고서가 통과라고 적혀 있고 그 보고서가 검사한 실행 파일이 ZIP 안의 실행 파일과 같을 때만 이루어진다. 배포 방식·코드 서명·업데이트 채널 결정은 [배포 방식](release.ko.md)에 있다. 검사가 실패하면 workflow가 실패하고 ZIP은 배포되지 않는다. CI 러너에는 오디오 장치와 타사 플러그인이 없으므로 실제 청음·실장치 녹음·타사 VST3는 CI 검사 범위 밖이며, 그쪽은 개발 기계에서 `tools/test_plugin_compatibility.py`와 `tools/test_real_environment.py`로 따로 확인한다.

통합 검사를 다시 실행하려면 현재 CoCompose를 먼저 정상 종료한 뒤 저장소 루트에서 다음을 실행한다. 검사는 새로운 테스트 폴더를 만들고 실제 앱을 숨긴 상태로 실행한다. 검사 중 정상 종료·재실행은 저장 복원 시험이며, 라이브 변경에 재실행이 필요한 것은 아니다.

```powershell
python tools/test_live_sync.py
```

결과 폴더는 기본적으로 `build-cocompose/live-test-<고유값>`이다. `test-report.json`, `before.png`, `after.png`, `ui-state.json`을 확인한다.
