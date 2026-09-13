# 동적 리뷰 — 2026-09-12

## 빌드와 범위

HEAD 78b81fe와 당시 미커밋 DAW 조작 변경을 함께 빌드했다. 기존 EXE가 다른 테스트에서 사용 중이어서 MSBuild OutDir를 build-cocompose/review-bin으로 지정했다. 사용자 소스를 되돌리지 않았고 기능 수정은 하지 않았다. 첫 앱 검사는 다른 테스트의 단일 인스턴스 점유로 초기화 시간 초과했다. 이를 기능 실패로 세지 않고 별도 실행 후 아래를 확인했다.

검사 EXE: build-cocompose/review-bin/CoCompose.exe
SHA-256: DE8920F4B8D9BAF2156B16BA471DB50C6B40F5664979DA9B3E41A212255380E5

실제 모델/API 호출 없이 합성 응답을 실제 앱의 chat-reply.json으로 전달했다. 이는 호스트 검증 경계 검사이며 음악 생성 품질/실제 provider 검증은 아니다. 사용자 곡이 아닌 UUID별 리뷰 프로젝트만 변경했다.

## 4차 리뷰 3개 실제 재현

tools/review_proposal_repro.py로 실제 앱을 열고 첨부→질문→응답→제안→적용→모델 readback까지 수행했다. 스크립트는 관찰 결과를 수집하는 재현 도구다. exit 0은 실행 완료이며 버그 없음의 뜻이 아니다.

| 이슈 | 관측 결과 | 판정 |
|---|---|---|
| R1 늦은 응답 revision | 요청 revision 7. 그 뒤 UI transpose로 8. 응답의 제안 base_revision이 8로 등록되고 Apply 성공. 사람이 바꾼 pitch 61이 85가 됨 | P1 재현 |
| R2 Insert-only 노트 수정 | Insert만 첨부한 질문에 유효 노트 ID를 담은 응답 전송. 제안 생성과 Apply 성공, pitch 60→85 | P1 재현 |
| R3 add 기본값 | 길이 0.5 beat 패턴에서 length 생략 add 요청. 1 beat 노트 생성·적용 승인. state.json 패턴 길이는 여전히 0.5 | P2 재현 |

원본 결과: build-cocompose/proposal-review-65d68cf6/report.json. 각 사례의 conversation.json, state.json, tool-response.json도 하위 폴더에 있다. 최초 report의 engine_readback 필드는 inspect_pattern 모델 조회 결과이며 DSP 청음 증거가 아니다. 재현 스크립트의 필드명은 model_readback으로 정정했다.

## 기존 검사

- bridge 독립 검사: 완료 요청 재시작 시 재전송 방지, 중단된 요청 재전송 방지, 과거 첨부 유지 모두 통과. provider spy 사용. build-cocompose/review-offline-74384dbc.
- test_daw_interaction.py: exit 0, FAILURES none. 실제 MouseEvent를 핸들러에 전달하는 검사이며 OS 입력 자동화와 구분한다. 텍스트 입력 중 Space 동작은 키보드 포커스 제약으로 NOT CHECKED HERE.
- 도구 계약 검사의 첫 호출은 jsonschema 미설치로 시작하지 못했다. 공유 Python을 바꾸지 않고 build-cocompose/review-python-deps에 설치했다.
- test_ai_chat.py: exit 0, FAILURES none. 제안 적용과 Undo를 포함한 기존 시나리오는 통과했지만 위 별도 3개 경계 재현은 막지 못했다.
- test_tool_contract.py 재시도: 다른 CoCompose 테스트가 다시 실행돼 단일 인스턴스 충돌. 앱 state 초기화 대기에서 막혀 이번 실행은 미완료로 분류한다. 의존성 문제와 앱 로직 실패를 혼동하지 않는다.
- 로그: build-cocompose/dynamic-review-0912/test_daw_interaction.log, test_ai_chat.log, contract-retry.log.

## 동시 개발과 해석 범위

리뷰 중 다른 개발 작업으로 HEAD가 ba6fc2f로 바뀌었고 작업 트리도 변경됐다. 다른 테스트(reliability-7bd79abf)가 실행되는 것도 확인했다. 위 결과는 명시한 SHA-256의 리뷰 EXE 기준이다. 새 HEAD에 같은 버그가 남아 있는지/고쳐졌는지는 이 결과만으로 단정하지 않는다. 새 소스를 강제로 덮어쓰거나 다른 테스트 프로세스를 종료하지 않았다. 수정 후 최신 단일 실행 환경에서 재현 도구와 전체 회귀를 다시 돌려야 한다.

## 수정 우선순위

기존 정상 시나리오가 통과하더라도 R1/R2의 권한·최신성 보장은 충족되지 않는다. 요청 당시 호스트 revision과 명시적 노트 수정 scope를 고정하는 수정이 우선이다. R3은 기본값을 한 곳에서 확정한 뒤 검증·diff·적용에 공유한다. 재현 스크립트를 거절 기대값을 갖는 회귀 검사로 전환한다.
