# 모델 공통 DAW 도구 계약 — 초안

2026-09-11. 구현 대상 명세이며 아래 도구가 이미 존재한다는 뜻은 아니다. ai-chat-milestones.ko.md의 A0.5에 해당한다.

## 원칙

내부 채팅·외부 CLI·향후 MCP가 하나의 검증된 편집 서비스에 접근한다. 모델별로 음악 편집 로직을 복제하지 않는다. 독립 스크립트를 대량으로 만들기보다 작은 명령과 레시피를 조합한다. 기존 project.json/state.json 방식은 호환 입구로 유지한다. JSON Schema 같은 기계 판독 계약을 실제 명령 등록 정보와 함께 관리하고 예제가 스키마를 통과하는지 검사한다.

모델은 임의 코드를 실행하지 않고 선언형 제안을 만든다. 범위·ID·자료형·값·충돌·잠금 검증은 호스트가 수행한다. 모델 역량과 관계없이 프로젝트 보존 규칙이 적용되어야 한다.

## 음악 의미와 단위

- Channel은 악기/연주 대상, Pattern은 채널별 노트 묶음, Clip instance는 Pattern의 시간상 배치, Playlist lane은 화면 배치 행, Insert는 믹서 신호 경로다. 서로의 번호를 ID로 대체하지 않는다.
- 시간은 quarter-note beat 기준 숫자와 명시적 필드명을 사용한다. 구간은 [start_beat, end_beat). 초와 마디 표시는 tempo/박자 맵으로 변환한다. 샘플 offset 등 기존 필드의 단위는 어댑터에서 명시적으로 변환한다.
- pitch는 MIDI 정수, velocity는 기존 모델이 허용하는 정수 범위, normalized parameter는 0..1. dB/Hz/pan은 단위를 필드/스키마에 명시한다. UI 표시값과 정규화 값을 혼동하지 않는다.
- stable ID와 표시 이름/현재 번호를 함께 반환한다. 같은 패턴의 여러 배치에 미칠 영향 수를 포함한다. 선택 배치만 수정 시 독립 복제도 한 Undo transaction에 포함한다.
- 노트 ID가 현재 모델에서 지속적으로 보존되지 않는다면 먼저 보강한다. 배열 index를 장기 식별자로 쓰지 않는다.

## 최소 도구 목록

| 도구 | 역할 | 변경 여부 |
|---|---|---|
| get_capabilities | 계약 버전, 지원 도구/제안 종류, 제한, 단위, 오디오 기능 조회 | 읽기 |
| get_selection | 현재 선택의 stable ID·범위·revision 조회 | 읽기 |
| inspect_region | 구간의 음악 상태와 제한된 주변 문맥 조회 | 읽기 |
| inspect_insert | 효과 순서·공개 파라미터·output/send 조회 | 읽기 |
| inspect_pattern | 노트·채널·참조 배치와 공유 영향 조회 | 읽기 |
| create_proposal | 노트/클립/파라미터 변경안을 검증하고 보관 | 제안만 생성 |
| preview_proposal | 원본/제안을 별도 스냅샷에서 렌더 | 원곡 불변 |
| apply_proposal | 검증된 제안을 원자적으로 적용 | 음악 변경 |
| get_operation | 비동기 진행/완료/실패 결과 조회 | 읽기 |
| cancel_operation | 응답 대기·렌더 중단 요청 | 이미 적용된 음악은 되돌리지 않음 |
| undo / redo | 공통 편집 이력 조작 | 음악 변경 |

지원하지 않는 도구는 capabilities에 노출하지 않는다. 각 도구에 JSON Schema, 성공 예시, 실패 예시, 필요한 문맥, 부작용을 제공한다. 초기 버전은 A2 범위의 노트와 기존 파라미터 제안만 구현하고 복잡한 라우팅은 뒤로 미룬다.

## 요청·응답 계약

식별자 수명을 구분한다. project_id는 프로젝트의 영구 식별자, conversation_id는 프로젝트 대화/분기 식별자, session_id는 현재 앱 실행의 라이브 싱크 식별자, request_id는 개별 요청 식별자다. 질문마다 conversation_id를 새로 만들지 않는다. 앱 재실행 시 대화는 복원하되 session_id와 revision은 다시 조회한다. 모델 교체는 로컬 대화를 유지하고 provider별 세션 매핑만 바꾼다. 늦은 결과는 원래 project_id/conversation_id로 전달하고 활성 프로젝트라는 이유로 다른 곡에 적용하지 않는다.

작업 메모의 사용자 확정 조건·AI 추정·현재 요청은 서로 다른 필드로 전달한다. 강도 설정은 허용 write scope를 확장하지 않는다. 확정 보존 조건은 create_proposal과 apply_proposal에서 모두 검사한다. 대화/후보/메모 도구는 음악 편집 명령과 별도로 등록하고, 대화 저장만으로 음악 revision이나 Undo를 변경하지 않는다.

요청에는 contract_version, request_id, project_id/session_id, 변경 요청의 base_revision을 포함한다. 제안에는 attachment/context ID, 읽은 의존 범위, 허용 write scope, 보존 조건을 포함한다. 타임아웃 후 같은 요청 ID로 재시도하면 중복 적용되지 않아야 한다. 세션 변경 후 이전 ID의 결과를 새 프로젝트에 적용하지 않는다.

응답은 status, request_id, 실제 revision, error(있을 때), change 요약, warnings를 반환한다. 비동기 작업은 operation_id를 먼저 반환한다. 오류는 최소 INVALID_ARGUMENT, NOT_FOUND, STALE_REVISION, OUT_OF_SCOPE, LOCKED, UNSUPPORTED, CANCELLED, IO_ERROR로 구분하고 재시도 가능 여부/다음 행동을 설명한다. 기존 applied_unpersisted는 '음악 적용됨, 저장 미완료'로 보존하며 재적용을 권하지 않는다.

create_proposal은 원곡을 수정하지 않는다. apply_proposal은 최신 상태와 의존 대상·범위·잠금을 재검사한 뒤 한 transaction으로 적용한다. 초기에는 revision 전체 불일치를 거절하는 보수적 정책이 가능하다. 최신 스냅샷에 오래된 전체 결과를 덮어씌우는 재시도는 금지한다. 부분 성공이 필요한 작업은 별도 명세 전까지 지원하지 않는다.

undo는 전역 최신 음악 작업을 되돌린다. AI 작업 뒤 사람이 편집했다면 특정 AI 작업만 되돌리는 것으로 오해하지 않도록 expected transaction ID를 검사하고 다르면 거절한다. 선택적 과거 Undo는 별도 기능이다.

## 초기 레시피 5개

레시피는 독립 엔진 구현이 아니라 위 도구의 호출 순서/보존 조건/실패 분기 예제다.

1. 구간 진단: selection → region(+앞뒤 문맥) → 구조 분석. 음악 변경 없음. 오디오가 없으면 청음 판단과 구분.
2. 멜로디 재작성: pattern/참조 조회 → pitch만 변경 제안 → start/length/velocity 보존 검사 → preview → 사용자 적용 → Undo.
3. velocity 정리: 선택 노트만 목표 범위에 조절 → pitch/시간 불변 → diff → 적용.
4. Insert 검토: 효과/파라미터/라우팅 조회 → 읽을 수 없는 opaque 상태 명시 → 설명. 수정 요청 시에만 기존 파라미터 제안.
5. 클립 이동: 선택 배치와 목표 위치 조회 → instance 이동 제안 → 원본 pattern 불변 검사 → 적용. 해당 제안 종류가 구현된 단계에서만 활성화.

레시피마다 성공 사례와 stale revision/선택 밖/잠금 오류 예제를 둔다. 문서 예제는 실제 validator로 실행한다. 완성된 스크립트는 CLI의 얇은 래퍼로 제공하고 검증을 우회하지 않는다.

## 산출물과 완료 조건

- 계약 문서, 기계 판독 schema, 공통 dispatcher/validator, CLI 입구, 초기 레시피와 재현 검사.
- 동일 요청을 내부 채팅 어댑터와 CLI로 실행했을 때 동등한 모델 결과/오류/Undo가 나온다.
- 잘못된 단위·ID·범위·오래된 revision·잠금 위반은 음악을 바꾸지 않는다. 중복 요청은 한 번만 적용된다.
- 스키마 버전을 바꾸면 지원/거절과 기존 프로젝트 변환을 명시한다. 모델별 프롬프트만 바꿔 의미가 달라지지 않는다.
- 두 모델로 같은 대표 작업을 평가하되 실제 모델 검사는 코드 validator 회귀 검사와 구분한다. 모델이 못하는 작업은 실패로 보고하고 호스트가 임의로 성공 처리하지 않는다.

MCP는 필요할 때 같은 서비스에 붙이는 어댑터다. 모든 모델이 작곡을 잘한다는 보장은 목표가 아니다. 지원 계약 안에서 대상을 정확히 읽고 안전하게 수정하며 실패 원인을 되돌려주는 것이 목표다.
