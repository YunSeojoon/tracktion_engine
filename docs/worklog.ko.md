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
