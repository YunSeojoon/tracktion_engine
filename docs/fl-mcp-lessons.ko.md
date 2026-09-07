# fl-mcp에서 가져갈 교훈: 외부 AI와 재오픈 없는 공동 작곡

조사일: 2026-09-07. 조사 대상은 로컬 `C:/project_private/fl-studio-mcp`의 운영 기록과 구현이다. 이 문서의 FL 사례는 해당 저장소가 기록한 관찰이며, 이번 조사에서 FL을 실행해 재현한 결과는 아니다. 코드와 문서를 읽기만 했고 기존 곡은 변경하지 않았다.

사용자 요구는 **외부 AI가 코드·데이터를 변경하면 실행 중인 앱과 음악 프로젝트에 반영되는 것**이다. 채팅 UI는 뒤로 미룬다. FL의 파일 편집 후 닫기/재열기 방식은 이식하지 않는다. 아래의 새 앱 적용안은 설계 요구이며, 이 문서 자체가 구현 완료를 뜻하지 않는다. C++ 실행 파일 자체의 무중단 재컴파일과 프로젝트 데이터의 라이브 반영도 구분한다.

## 1. 디스크 파일과 열린 프로젝트가 서로 덮어쓰지 않게 한다

- 근거: [CLAUDE.md:63](C:/project_private/fl-studio-mcp/CLAUDE.md:63)는 FL이 파일 변경을 감시하지 않으며, 열린 세션의 저장이 외부 편집을 덮어쓴다고 기록한다.
- 가져올 기능: 외부 도구는 변경을 제출하고, 앱은 실행 중인 Edit에 그 변경을 적용한 다음 적용 결과를 내보낸다. 문서만 다시 읽고 음악 엔진을 그대로 두면 라이브 싱크가 아니다.
- 필요한 안전장치: 완성된 입력만 적용하고, 파싱/검증 실패 시 현재 재생 가능한 프로젝트를 유지한다. UI 편집과 외부 편집이 겹칠 때는 revision 또는 동등한 충돌 검출로 덮어쓰기를 막는다.
- 확인할 동작: 앱을 한 번만 실행하고 외부에서 노트·템포를 연속 수정한다. UI와 소리가 바뀌고 저장 후에도 변경이 유지되어야 한다.

## 2. 선택된 UI 대신 명시적인 음악 객체를 대상으로 읽고 쓴다

- 근거: [CLAUDE.md:125](C:/project_private/fl-studio-mcp/CLAUDE.md:125)는 live read가 요청한 채널 대신 현재 열린 피아노롤의 노트를 반환한 사례를 기록한다. [flp_tool.py:2018](C:/project_private/fl-studio-mcp/flp/flp_tool.py:2018)의 구현도 선택과 키 입력을 거쳐 현재 score를 수집한다. [fl-live/SKILL.md:241](C:/project_private/fl-studio-mcp/.claude/skills/fl-live/SKILL.md:241)는 채널 필터가 인덱스를 바꿔 잘못된 악기로 쓰기가 향하는 사례를 설명한다.
- 가져올 기능: 트랙·클립·노트의 안정적인 ID를 사용한다. UI 선택과 정렬은 외부 API의 대상 식별을 바꾸지 않는다. 응답에 실제 대상 ID와 적용 revision을 포함한다.
- 확인할 동작: 다른 클립을 선택하고 트랙 표시 순서를 바꿔도 지정한 클립만 변경된다. readback은 화면과 무관하게 그 ID의 실제 노트를 돌려준다.

## 3. 요청 수신과 실제 적용 완료를 구분한다

- 근거: [CLAUDE.md:147](C:/project_private/fl-studio-mcp/CLAUDE.md:147)는 setparam 응답이 실제 적용보다 빨라 별도 params 확인이 필요하다고 한다. [flp_tool.py:2144](C:/project_private/fl-studio-mcp/flp/flp_tool.py:2144)는 새 capture가 시작하기 전에 이전 capture의 done 상태를 읽어 조기 성공한 경쟁 상태를 기록한다.
- 가져올 기능: 요청별 ID와 received/applied/failed 같은 상태를 갖는다. 완료는 엔진에 반영된 뒤에만 기록한다. 같은 요청을 재시도했을 때 중복 노트가 생기지 않게 한다.
- 확인할 동작: 연속 요청 두 개와 잘못된 입력을 보내 각 결과가 자기 요청에 대응하는지 확인한다. 캡처/렌더 기능을 붙일 때에는 결과 파일과 project revision도 연결한다.

## 4. 화면이 뒤에 있어도 동기화가 살아 있어야 한다

- 근거: [CLAUDE.md:31](C:/project_private/fl-studio-mcp/CLAUDE.md:31)는 FL이 전경일 때만 OnIdle을 호출해 백그라운드 명령이 NO_SELECTOR가 되는 문제를 기록한다. [noteprobe.cpp:530](C:/project_private/fl-studio-mcp/experiments/noteprobe/noteprobe.cpp:530)는 모달 MessageBox가 GUI 스레드의 PollServer를 막는다고 설명한다.
- 가져올 기능: 전경 창이나 키 입력에 의존하지 않는 입력 경로를 둔다. GUI 스레드에 필요한 엔진 변경만 전달하고, 오류는 상태/로그로 노출한다. 최소화 상태도 시험한다.
- 오디오 제약: [noteprobe.cpp:347](C:/project_private/fl-studio-mcp/experiments/noteprobe/noteprobe.cpp:347)는 오디오 스레드에서 파일 I/O를 하지 않는다. 새 앱에서도 파일 읽기·JSON 파싱·디스크 저장을 오디오 콜백에 넣지 않는다.

## 5. 한 번의 제안은 한 번에 적용하고 정확히 되돌린다

- 근거: [fl-live/SKILL.md:258](C:/project_private/fl-studio-mcp/.claude/skills/fl-live/SKILL.md:258)는 replace가 선택 채널만 비워 오입력한 다른 채널의 노트가 남는다고 기록한다. 같은 파일 [168행](C:/project_private/fl-studio-mcp/.claude/skills/fl-live/SKILL.md:168)은 FL undo를 사용자의 복구 수단으로 설명한다.
- 가져올 기능: 외부 편집을 명명된 undo transaction으로 묶고, 대상 범위를 명시한다. 전체 입력을 검증한 다음 적용해서 중간까지 변경된 프로젝트를 남기지 않는다. 비어 있는 notes 배열은 실제 비우기를 지원해야 한다.
- 확인할 동작: 여러 노트를 바꾼 요청을 한 번 undo하면 원본으로 돌아오고, 다른 클립은 그대로다. 잘못된 노트 하나가 포함된 요청은 전체 거부하거나 명시한 정책대로 처리하며 부분 성공을 숨기지 않는다.

## 6. MIDI는 음높이만 있는 데이터가 아니다

- 근거: [ComposeWithBridge.pyscript:94](C:/project_private/fl-studio-mcp/experiments/noteprobe/ComposeWithBridge.pyscript:94)의 snapshot은 위치·길이·velocity 외에도 pan, color, selected, release, pitch offset, slide 등의 속성을 수집한다. [CLAUDE.md:65](C:/project_private/fl-studio-mcp/CLAUDE.md:65)는 노트 정렬에 따른 무음, [71행](C:/project_private/fl-studio-mcp/CLAUDE.md:71)은 옥타브 표기 차이를 기록한다.
- 가져올 기능: 절대 MIDI 번호와 명시적인 시간 단위를 쓰고, 수정하지 않는 속성은 보존한다. 첫 스키마의 지원 범위를 좁힐 수는 있지만 누락 속성을 조용히 지우면 안 된다. 키스위치와 일반 음표를 일괄 transpose하는 위험도 고려한다.
- 확인할 동작: 선택 구간의 음높이만 바꿨을 때 길이·velocity·다른 구간이 유지된다. 음높이 범위, 유한한 숫자, 양수 길이, 음수 위치 정책을 검증한다. FL 특유의 내부 정렬 규칙을 Tracktion에 그대로 복사하는 것은 아니다.

## 7. 수정한 음악을 실제 악기로 확인하는 루프를 유지한다

- 근거: [CLAUDE.md:133](C:/project_private/fl-studio-mcp/CLAUDE.md:133)는 capture → analyze → 한 가지 변경 → 재캡처 루프를 설명한다. [89행](C:/project_private/fl-studio-mcp/CLAUDE.md:89)은 일부 플러그인이 외부 state 주입을 무시하거나 기본 패치를 유지한 사례를 기록한다.
- 가져올 기능: 이후 실제 호스팅된 악기의 출력 캡처/렌더, 구간 A/B, 상태 저장·복원을 붙인다. 명령 성공과 원하는 소리가 난다는 판단은 분리한다. 외부 AI에 측정치와 실제 변경 내용을 제공하되 음악적 채택은 사용자가 결정한다.
- 미구현으로 남겨도 되는 범위: 첫 데이터 싱크 시제품에 모든 VST 상태 복원, 오디오 평가, 채팅을 동시에 넣을 필요는 없다. 기본 신스만 검증했다면 VST 호환성까지 검증했다고 쓰지 않는다.

## 우선순위

첫 성공 조건은 재오픈 없이 외부 데이터 변경 → 실행 중 프로젝트 변경 → UI 갱신 → 적용 결과 확인이다. 그 다음 안정 ID, 충돌 처리, undo와 실제 악기 출력 검증을 차례로 확인한다. 이 문서의 제안 중 실제로 구현·검증된 범위는 실행 안내와 구현 보고에서 별도로 표시한다.

## 현재 CoCompose 시제품에 반영된 범위

아래는 2026-09-07 구현의 정적 코드 검토 결과다. 실제 실행 시험 결과는 별도 구현 보고를 따른다.

| 요구 | 코드에서 확인한 구현 | 아직 보장하지 않는 범위 |
|---|---|---|
| 재오픈 없는 적용 | `LiveProject.h`는 시작 시 한 번 Edit를 로드하고 `apply()`가 기존 객체를 갱신한다. 동일 ID의 트랙·MIDI 클립·노트는 재사용한다. | 트랙 삭제/클립 이동처럼 객체 자체가 바뀌는 편집의 선택 상태 보존, 모든 VST의 무중단 동작 |
| 안정 ID | ValueTree의 `coComposeId`로 트랙·클립·노트를 식별하며 중복 입력 ID를 검증한다. 파라미터는 plugin ID와 parameter ID로 찾는다. | GUI 복제·가져오기 등 모든 경로에서 ID가 유일하게 유지되는지의 실측 |
| UI↔외부 충돌 검출 | 250ms 타이머가 live snapshot 차이를 revision으로 기록하고, 입력 revision이 다르면 거절한다. | snapshot 밖 플러그인 상태도 약 2초마다 flush/signature 비교로 감지한다. 모든 VST가 변경 상태를 정확히 노출하는지, 추가 MIDI 속성까지 감지하는지는 별도 시험 대상이다. |
| 적용 후 확인 | `state.json`은 입력 JSON 대신 live engine snapshot으로 생성한다. `sync-status.json`과 CLI는 request ID를 연결한다. | snapshot readback이 플러그인의 비동기 DSP 적용이나 실제 오디오 출력까지 확인하는 것은 아니다. |
| 부분 입력 방어 | 두 번 같은 파일 내용 확인, 전체 스키마/범위 검증 후 적용, 임시 파일 후 교체를 사용한다. | 파일 교체의 원자성과 여러 파일·여러 작성자의 트랜잭션 원자성은 다르다. |
| 되돌리기 | 외부 변경을 undo transaction으로 묶고 apply 예외 시 현재 transaction의 rollback을 시도한다. CLI undo/redo도 있다. | 플러그인별 파라미터/불투명 상태의 rollback은 별도 시험이 필요하다. apply 이후 저장 실패까지 원상복구한다고 보장하지 않는다. |
| 범위·속성 보존 | 같은 노트 객체에서 pitch/velocity/start/length만 갱신하며 빈 notes 배열은 실제 노트를 제거한다. | 새 객체로 재생성하거나 다른 클립으로 옮길 때 snapshot에 없는 속성은 복원되지 않는다. 키스위치 보호는 자동 제공되지 않는다. |
| 외부 AI 연결 | Python 표준 라이브러리 CLI가 읽기, 템포·노트·게인·노출 파라미터 수정, 재생/정지와 undo/redo를 제공한다. | 채팅 UI, 오디오 평가·A/B, C++ 바이너리 hot reload는 이 데이터 싱크 기능에 포함되지 않는다. |

검토한 구현: [LiveProject.h](C:/project_private/tracktion_engine/examples/CoCompose/LiveProject.h), [Main.cpp](C:/project_private/tracktion_engine/examples/CoCompose/Main.cpp), [cocompose.py](C:/project_private/tracktion_engine/tools/cocompose.py).

### 검토에서 발견해 수정한 사항

초기 정적 검토의 다섯 과제는 다음과 같이 구현이 수정되었다. 내장 FourOsc를 사용한 통합 시험은 아래 최종 검증 결과에 기록했다. 타사 VST까지 확인한 결과는 아니다.

1. **재생 범위 보존:** live `apply()`에서 loop range·looping 강제 설정을 제거했다. 초기 예제의 루프 설정과 live 편집을 분리했다.
2. **입력 파일 덮어쓰기 제거:** `project.json`은 입력 전용이며 `publish()`는 native/state/status만 쓴다. 초기 파일 생성 이후 앱이 제출 파일을 다시 덮어쓰지 않아 기존 compare-then-write 경쟁을 제거했다. 다중 외부 작성자가 같은 입력 파일을 동시에 덮어쓰는 문제까지 해결하는 큐는 아니다.
3. **실패 상태 구분:** 음악 적용 후 저장 실패는 `applied_unpersisted`, 입력 거부는 `rejected`로 구분한다. 여러 출력 파일의 일괄 원자적 저장까지 제공하는 것은 아니다.
4. **불투명 플러그인 상태 감지:** 약 2초마다 `pluginSignature()`가 상태 flush와 비교를 수행한다. 각 VST의 state 회신·부작용·소요 시간은 실측 대상이며, 오디오 스레드에서 실행하지 않는다.
5. **세션 경계 검증:** project 데이터에도 `session_id` 검증을 추가했다. 외부 도구는 매 편집 전 실행 중인 앱의 `state.json`을 읽어야 한다.

근거: [LiveProject.h](C:/project_private/tracktion_engine/examples/CoCompose/LiveProject.h)의 `poll()`, `pluginSignature()`, `publish()`.

### 추가 실측 교훈: 저장·상태 확인도 음악 편집에 부작용을 만들 수 있다

구현 담당 에이전트의 통합 시험에서 **MIDI clear 후 한 번 undo로 원래 노트가 복구되지 않는 문제**를 발견했다. 원인 조사 결과 FourOsc의 `flushPluginStateToValueTree()`가 동일한 MODMATRIX를 매번 제거·재추가하여, 세 번의 flush만으로 불필요한 Undo 동작 여섯 개가 들어갔다. 상태 저장이 실제 음악 편집의 undo 경계를 오염시켰고 playback graph에도 불필요한 변경을 전달했다. 이 실측은 구현 담당 에이전트의 보고를 옮긴 것이며 이 문서 작성 에이전트가 별도로 재현한 것은 아니다.

수정은 **같은 matrix이면 교체하지 않는 것**이다. 비교용 detached matrix 생성에는 undo manager를 넘기지 않는다. 변경 코드는 [tracktion_FourOscPlugin.cpp:1416](C:/project_private/tracktion_engine/modules/tracktion_engine/plugins/effects/tracktion_FourOscPlugin.cpp:1416)에서 확인할 수 있다. 후속 통합 시험에서 MIDI clear → 한 번 Undo로 복원 → Redo 재적용을 통과했다(내장 FourOsc 한정).

여기서 얻는 요구는 “라이브 상태를 읽으면 안전하다”라고 가정하지 않는 것이다. 상태 flush를 반복한 뒤에도 revision이 이유 없이 늘지 않는지, 저장이 undo 항목을 만들지 않는지, 노트 편집을 한 번 undo하면 복원되는지 확인해야 한다. 프로젝트 객체의 주소가 유지되는 것만으로 재생 그래프와 플러그인 실행 상태까지 보존됐다고 판정하지 않는다.

### 최종 검증 결과와 추가 Undo 교훈

[최종 통합 시험 보고서](C:/project_private/tracktion_engine/build-cocompose/live-test-a66d5569/test-report.json)의 10개 항목이 모두 통과했다. 보고서 파일을 직접 확인했으며, 실행 담당 에이전트가 시험 프로세스 exit 0을 보고했다.

- Windows 앱 시작과 Tracktion 재생, 템포·트랙 이름·음높이·velocity 변경 및 UI 반영
- 잘못된 MIDI·부분 JSON의 무변경 거절, 오래된 revision·잘못된 session 거절
- MIDI clear/Undo/Redo, 신스 파라미터 변경/Undo의 엔진 readback
- 동일 Edit를 유지하는 트랙 추가·순서 변경·삭제
- 저장 실패 상태 구분과 저장 가능해진 뒤 자동 복구
- 정상 종료 후 native session 재시작 시 음악·플러그인 상태 복원

UI label 누락도 수정한 뒤 실행 담당자가 이미지와 label 트리를 확인했다. live 편집 시험에서 프로젝트 재오픈은 필요하지 않았다. 마지막 종료/재시작 항목은 저장 복원 시험이며 live sync를 위한 절차가 아니다.

추가로 **플러그인 파라미터의 실제 playback value도 Undo로 복구해야 한다**는 점을 확인했다. [LiveProject.h](C:/project_private/tracktion_engine/examples/CoCompose/LiveProject.h)의 `ParameterAction`으로 명시적인 파라미터 값 복구를 추가했으며, FourOsc `filterFreq` 변경 후 Undo가 엔진 readback으로 검증됐다. ValueTree의 변경 이력만 존재한다고 플러그인 실행 값 복원까지 자동 보장되는 것은 아니다.

검증 범위는 **내장 FourOsc 시제품**이다. 타사 VST의 상태 저장·복원·Undo, 무음이나 오디오 끊김 없는 동작, 오디오 A/B 품질 평가는 이번 시험으로 입증하지 않았다.
