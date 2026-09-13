# CoCompose 3차 리뷰와 실행 순서

2026-09-11. 기준 커밋 6caff09. 최근 A0/A0.5/A1 코드와 현재 작업 트리, DAW 입력 경로를 확인했다. A2로 보이는 Proposal.h와 Tools.h/검사/문서 변경은 미커밋 진행 중이므로 완료된 기능으로 판정하지 않는다. 기존 변경은 수정하거나 되돌리지 않았다.

## 검증 범위

정적 코드 리뷰와 Python build_prompt의 작은 문맥 보존 검사를 실행했다. 전체 EXE 빌드·28개 통합 검사·실제 모델 호출·청음·원격 CI는 이번에 실행하지 않았다. 아래 과거 통과 기록은 docs/worklog.ko.md의 보고다. 실제 provider 응답은 그 기록에서도 미검증이며 A1 미완료로 명시되어 있다.

## 구현 상황

| 단계 | 확인 결과 |
|---|---|
| 기존 DAW/RC | 기능 기반 유지. 최신 채팅 빌드를 과거 RC2 검증과 동일시하지 않음 |
| A0 | d914a84 선택 첨부 구현 커밋 존재 |
| A0.5 | 293bd52 공통 읽기 도구/외부 파일 입구/스키마 검사 |
| A1 | 6caff09 프로젝트 대화 저장과 외부 bridge 구현. 실제 모델 검증/완전한 UI 흐름은 미완료 |
| A2 | Proposal.h와 Tools.h 등 미커밋 작업 중. 이번에 완전성 감사하지 않음 |
| D0~D2 | 룰러 클릭=loop, 우클릭=삭제, 더블클릭=분할 경로 유지. 새 명세와 불일치 |
| CI | 4dde58f 이후 workflow_dispatch만. 자동 실행 제거는 명시적 정책이므로 회귀 버그로 보지 않음 |

## 리뷰 이슈

### C1 [P1] bridge 재시작이 이전 질문을 다시 외부에 보낼 수 있음

위치: tools/cocompose_bridge.py::serve, ChatBridge.h::ask.
answered는 메모리 set이고 chat-request.json은 답변 뒤 남는다. bridge를 재시작하면 set이 비어서 완료된 마지막 요청도 provider.answer로 다시 간다. 현재는 읽기 전용이지만 불필요한 중복 호출/비용과 기존 reply 교체가 생길 수 있다. future write와도 섞지 않아야 한다.

수정: request ID/project/conversation에 묶인 영속 완료 상태를 확인한다. provider 응답 도중 crash처럼 처리 여부가 불확실한 경우는 자동 재전송하지 않고 재시도 정책을 명시한다.
완료 검사: 첫 실행 완료 후 같은 폴더로 bridge 재시작해 provider spy 호출 수 불변. 새 request ID는 한 번 호출. 처리 중 crash와 사용자 취소도 별도 검사.

### C2 [P2] 종료된 bridge가 계속 연결됨으로 보이고 대기가 끝나지 않음

위치: ChatBridge.h::isConnected/poll, cocompose_bridge.py::serve.
ready=true는 시작 때 한 번 기록된다. finally가 실행되지 않는 강제 종료에서는 파일이 그대로 남는다. 앱은 ready만 읽고 pending timeout/heartbeat 만료를 확인하지 않는다. 이 상태에서 질문을 보내면 입력을 소비하고 답 없이 기다릴 수 있다.

수정: bridge instance ID·갱신 heartbeat와 만료 기준, 작업별 deadline을 도입한다. 네트워크 호출 중에도 생존 상태를 갱신하거나 busy lease를 구분한다. draft/실패 요청을 복원하고 늦은 응답을 올바른 요청에만 귀속한다.
완료 검사: 대기 전/중 프로세스 강제 종료, 네트워크 hang, 재연결에서 UI가 유한 시간 내 상태를 알리고 음악 재생과 사용자 글을 보존.

### C3 [P2] 대화는 저장하지만 과거 첨부 문맥은 모델에 전달되지 않음

위치: cocompose_bridge.py::build_prompt.
history의 이전 메시지에서 from/text[:600]만 전달하고 첨부를 제외한다. '그 부분을 덜 복잡하게'라는 다음 질문에 현재 첨부가 없으면 이전 선택의 음악 상태를 모델이 읽을 수 없다. 600자 절단도 확정 조건을 조용히 버릴 수 있다. 저장된 대화와 모델이 실제로 받는 대화가 다르다.

수정: 과거 첨부의 고정 스냅샷/식별자와 필요한 요약을 연결한다. 원문 역할과 데이터 출처를 구분하고 문맥 예산 초과/요약 사실을 표시한다. 과거 대상은 지금도 같은 상태라고 가정하지 않는다.
완료 검사: 첫 질문에만 선택 첨부→재시작→'그 부분' 질문에서 이전 대상과 보존 조건 전달, 현 revision과 차이 표시. 긴 사용자 조건 끝부분이 사라지는 사례 검사.

### C4 [P2] 구간 진단용 prompt가 음악 내용을 과도하게 줄임

위치: cocompose_bridge.py::describe_attachments.
region은 clip 이름/start/length만 문자열로 보내고 context_clips는 개수만 보낸다. Insert도 파라미터 앞 12개만 전달하며 잘림을 명시하지 않고 sends 정보를 표시하지 않는다. 모델이 구조/화성이나 send 원인을 진단하는 데 필요한 내용이 빠진다. '아무 오디오 없음' 표시는 올바르지만 MIDI 구조까지 생략하면 진단 능력이 제한된다.

실행 확인: 메모리 fixture에 note pitch 73, CONTEXT_SENTINEL, 과거 첨부 OLD_TARGET_SENTINEL을 넣어 build_prompt를 호출했다. 세 값 모두 결과에서 사라짐을 assert로 확인했다. 이는 formatter 경로 검사이며 실제 tool packet 전체의 end-to-end 검사는 아니다.

수정: 구간 안 패턴/채널/노트와 읽기용 주변 문맥을 bounded packet으로 전달한다. 잘린 항목 수와 추가 조회 수단을 제공한다. 현재 provider는 도구를 스스로 추가 호출하지 않으므로 최초 packet의 필수 정보 보장 또는 실제 tool loop 구현이 필요하다.
완료 검사: 같은 클립 이름·길이지만 다른 화성인 두 입력이 서로 다른 음악 내용을 전달. send와 13번째 이후 파라미터의 조회 가능 여부/잘림 표시 검사.

## 기능 갭: DAW 조작

PlaylistGrid::mouseDown 룰러 분기는 setLoopRange, 클립 우클릭은 Delete clip. PianoRoll 우클릭도 Delete note. PlaylistGrid::mouseDoubleClick은 split이다. 최근 사용자가 확정한 동작과 다르므로 daw-interaction-spec.ko.md의 D0~D2가 필요하다. 피아노롤 자체는 존재하므로 재작성하지 않는다.

이 변경은 기존 동작의 무조건 버그가 아니라 새 UX 요구에 따른 사양 변경이다. 예전 테스트의 우클릭 삭제 기대도 함께 갱신해야 한다.

## 다음 실행 마일스톤 — 기존 A/D 번호 유지

### 1. A1-R: 채팅 연결·문맥 신뢰성
C1~C4부터 수정하고 작은 provider spy/formatter 검사를 추가한다. 실제 provider 한 번의 단발 응답뿐 아니라 프로젝트 대화 3턴, 재시작, 취소/실패 복구를 확인한다. 실제 연결을 안 돌렸으면 A1 완료 선언하지 않는다. 테스트에는 실제 API 키가 필요 없는 재현 경로를 유지한다.

### 2. D0/D1: seek와 우클릭 공통 동작
룰러 좌클릭 seek, 범위/loop 분리, 우클릭 대상 메뉴, 텍스트/채팅 입력 포커스 보호. 메뉴 진입은 음악 불변, 삭제는 Delete/Erase. 실제 pointer 이벤트로 확인한다.

### 3. D2~D4: 일관된 편집 작업 공간
패턴 더블클릭→피아노롤, Select/Draw 구분, 줌/스크롤/드래그 취소/Undo, Mixer 파라미터 메뉴. Ableton의 Clip/Device 보기와 overview는 daw-interaction-spec의 범위대로 적용한다. Logic Inspector처럼 선택 대상/속성을 분명히 보여주는 흐름은 참고 후보이며, 아직 해당 공식 매뉴얼을 검증한 구현 사양은 아니므로 별도 출처 확인 후 구체화한다. Mac 단축키를 Windows에 그대로 옮기지 않는다.

### 4. A2: 수정 제안 완료 판정
진행 중 Proposal.h를 먼저 읽고 기존 작업과 충돌 없이 이어간다. 범위·보존 조건·패턴 공유/독립 복제·stale·중복 적용·원자적 Undo를 검사한다. AI 응답을 실제 제안으로 연결하고 UI diff/적용 경로까지 통과해야 한다. 서비스 API만 구현하고 '채팅으로 음악 수정 가능'이라 하지 않는다.

### 5. A3/A4: 청음·프로젝트 문맥 완성
고정 스냅샷 A/B와 선택 오디오 전송, 작업 메모/추정 구분, 후보 이력/모델 교체를 순서대로 붙인다. 이전 대화가 남는 것과 현재 모델이 필요한 문맥을 읽는 것을 각각 검사한다.

### 6. A6/릴리스: 같은 바이너리의 증거
새 코드 빌드→DAW/도구/채팅 회귀→실제 AI 3시나리오→청음/장치 확인→패키징. CI는 현재 수동 정책을 존중해 필요한 시점에 실행한다. 과거 RC2나 echo 검사 성공을 새 AI 릴리스 성공으로 재사용하지 않는다.

## Claude 전달용

현재 미커밋 A2 작업을 보존하고 C1~C4와 D0/D1을 우선 처리해줘. 이 문서의 코드 기준은 6caff09이므로 시작 시 최신 변경과 다시 비교해. provider 중복 호출/죽은 연결/과거 첨부 소실부터 작은 재현 검사로 확인하고, 새 DAW 조작 명세와 기존 테스트 기대를 함께 갱신해. 실제 모델 검증과 echo 검증을 분리하고, 단계마다 실행 결과와 남은 제한을 기록해.
