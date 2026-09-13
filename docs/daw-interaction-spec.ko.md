# DAW 기본 조작 재검토 — FL Studio / Cubase 참고

2026-09-11. 로컬 HEAD 293bd52에서 PlaylistGrid.h, PianoRoll.h, Workspace.h의 이벤트 경로를 정적으로 확인했다. 실제 마우스 조작/빌드 검사는 이번에 수행하지 않았다. 아래 '채택 사양'은 구현 요구사항이며 타 제품과 동일하다는 주장이 아니다.

## 공식 참고 자료와 채택 판단

- [FL Playlist](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/playlist.htm): 재생 마커 이동, 범위 선택, 클립 도구, 줌/스크롤, 트랙 메뉴 참고. 빠른 패턴 배치와 직접 조작을 채택한다. 우클릭 삭제는 우리 기본값으로 채택하지 않는다.
- [FL Piano roll](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/pianoroll.htm): 노트 격자와 편집 도구 참고. 직접 노트 작성·미리듣기와 세밀한 편집을 목표로 한다.
- [Cubase Pro 15 Key Editor](https://www.steinberg.help/r/cubase-pro/15.0/en/cubase_nuendo/topics/midi_editors/midi_editors_key_editor_r.html): MIDI part 더블클릭/명령으로 에디터를 여는 흐름과 하단/별도 창 배치 참고. 선택한 음악 대상과 편집창 연결을 채택한다.
- [Cubase Pro 15 Toolbox](https://www.steinberg.help/r/cubase-pro/15.0/en/cubase_nuendo/topics/project_window/project_window_toolbox_c.html): 우클릭 context menu와 toolbox의 설정상 구분 참고. 우리 기본은 대상 메뉴, 도구 선택은 별도 툴바로 한다.

위 매뉴얼은 참고이며 서로 다른 modifier/단축키를 동시에 복제하지 않는다. 이하 단축키와 오류 방지 정책은 CoCompose 제안이다. 전체 Cubase/FL 기능 동등성 감사는 아니다.

## 현재 코드와 차이

| 항목 | 확인한 경로 | 판단 |
|---|---|---|
| 룰러 클릭 | PlaylistGrid::mouseDown, y < rulerHeight → setLoopRange / dragMode=loop | 단순 좌클릭 seek와 분리 필요 |
| 클립 우클릭 | PlaylistGrid::mouseDown → Delete clip | 기본 context menu 요구와 충돌 |
| 노트 우클릭 | PianoRoll의 canvas mouseDown → Delete note | 기본 context menu 요구와 충돌 |
| 빈 격자 클릭 | Playlist는 place, PianoRoll은 addNoteAt | 명시적 Select/Draw 도구 구분 필요 |
| 피아노롤 | Workspace::openPianoRoll 및 Rack 버튼 존재 | 새로 만드는 대신 선택 대상 연결과 창 조작 보강 |
| 클립 더블클릭 | PlaylistGrid::mouseDoubleClick → split | 에디터 진입으로 변경하고 분할은 Split 도구/명령으로 이동 필요 |
| 효과/자동화 메뉴 | Workspace의 effects/routing/automate 버튼에 PopupMenu 존재 | 대상 우클릭에서도 같은 명령 재사용 필요 |

## Ableton Live 참고 추가

2026-09-11 사용자 요청으로 공식 Live 12 매뉴얼을 추가 확인했다. 다음은 구현 예정 판단이며 기존 코드 지원 여부는 별도 확인한다.

| 참고 | CoCompose 채택 방향 | 완료 검사 |
|---|---|---|
| [Clip View](https://www.ableton.com/en/manual/clip-view/) — 선택 클립 내용/속성 편집 | 하단 에디터에 MIDI는 피아노롤, 오디오는 파형/클립 속성 표시. 헤더에 대상 이름과 종류 명시 | 선택 전환 시 올바른 대상 표시, 드래그 도중 선택이 바뀌어 다른 대상을 수정하지 않음 |
| [Device View](https://www.ableton.com/en/live-manual/12/working-with-instruments-and-effects/) — 선택 트랙의 기기와 효과 | 선택 Channel/Insert의 효과 체인과 공개 파라미터를 한곳에서 확인. Clip 편집과 효과 보기를 전환하거나 충분한 공간에서 함께 표시 | 효과 순서와 실제 엔진 일치, bypass/수치 변경/Undo. VST 창은 필요 시 따로 열기 |
| [Arrangement](https://www.ableton.com/en/live-manual/12/arrangement-view/) — 전체 구간 overview와 자동화 이동 정책 | 전체 곡 미니맵으로 현재 viewport 표시/탐색. 클립 이동 시 관련 자동화 동반 여부를 명시 | 줌·스크롤 좌표 정확. 자동화 동반/시간 고정 각각 이동·Undo 검사 |

위 기능은 D2(문맥 에디터), D3(overview/자동화 이동), D4(효과 보기)의 확장 조건으로 추적한다. 작은 창에서는 Clip/Device를 탭으로 전환하고, 두 뷰를 동시에 표시하도록 강제하지 않는다. 개별 패널 최대화·복원 요구는 유지한다.

FL의 패턴은 여러 채널을 포함할 수 있으므로 Live의 '트랙 선택 = 단일 기기 체인'을 그대로 가정하지 않는다. 패턴 클립에서 어떤 채널/Insert를 보고 있는지 명시하며 레인 번호로 효과 체인을 추정하지 않는다. 채팅 첨부 대상도 동일한 ID 관계를 사용한다.

Session View/클립 런처는 [Live의 별도 즉흥 연주 흐름](https://www.ableton.com/en/live-manual/12/launching-clips/)이다. 좋은 확장 후보지만 현재의 선형 편곡/AI 수정 MVP에는 추가하지 않는다. Warp 전체, Max for Live, Live 단축키 전면 복제도 이번 범위가 아니다. 클립 본문 이동과 Ctrl/Alt 규칙은 기존 CoCompose 명세를 유지한다.

## 공통 입력 규칙 (CoCompose 기본값)

좌클릭은 현재 도구의 직접 조작, 우클릭은 대상별 메뉴다. 우클릭 자체로 삭제/값 변경/재생 점프하지 않는다. 기존 우클릭 삭제 회귀 검사는 새 사양에 맞게 변경한다. 삭제는 Delete 또는 명시적 Erase 도구다. 초기부터 여러 DAW 호환 모드를 늘리지 않는다.

메뉴는 클릭한 대상을 보여주되 선택 그룹 안을 우클릭하면 그룹을 보존한다. 선택 밖 대상을 우클릭하면 그 대상으로 전환한 후 메뉴를 연다. 메뉴를 여는 동안 음악은 불변이다. '설정'이 필요한 항목만 Properties/Inspector를 열고 매번 큰 설정창을 띄우지 않는다. 닫힌 메뉴의 비동기 callback은 대상 삭제/프로젝트 변경을 다시 확인한다.

드래그 한 번은 Undo 한 번. 이동 거리 임계값을 넘기 전 복제/이동을 확정하지 않는다. Esc로 진행 중 드래그를 취소하면 이전 음악 상태로 돌아간다. 텍스트/채팅 입력 중 Space/Delete/Ctrl+A가 transport나 노트를 건드리지 않는다. 도구·modifier·포커스는 툴팁/커서/상태줄에 표시한다.

## D0. 재생 위치와 탐색 — 먼저

사용자가 말한 프로그레스 바는 DAW의 타임룰러/플레이헤드로 구현한다. 렌더 진행률 바와 혼동하지 않는다.

| 동작 | 채택 사양 |
|---|---|
| 룰러 좌클릭 | 클릭 위치로 seek. 정지 중에는 정지 유지, 재생 중에는 재생 유지 |
| 플레이헤드 좌드래그 | 위치 미리표시, mouse-up에서 최종 seek. 연속 음향 scrub은 별도 도구로 후순위 |
| 룰러 Shift+드래그 | 시간 범위 선택. loop on/off와 별개이며 AI 첨부에도 사용 |
| 루프 핸들 드래그 | 루프 시작/끝만 변경, 역전/0길이 처리 명확화 |
| Space | 텍스트 입력이 아닐 때 재생/정지 토글. 기본 정지는 현재 위치 유지 |
| Stop 버튼 | 정지. 이미 정지 상태에서 재누르면 마지막 명시적 재생 시작점으로 이동 |
| Home | 프로젝트 시작. 텍스트 입력 중에는 원래 텍스트 동작 |
| Follow playhead | 켜고 끌 수 있음. 사용자가 화면을 탐색하면 따라가기 일시 중지 표시 |

시간 선택·루프·클립 선택을 다른 시각 요소로 구분한다. 음원 파형/클립 본문 클릭은 seek가 아니다. 자동 편집이 재생 중 루프를 몰래 바꾸지 않도록 기존 RC 수정 유지. 클릭 seek에는 별도 snap 정책(기본 자유 위치), 편집 snap은 기존 격자 설정을 사용한다.

완료: 재생/정지 각각에서 룰러 클릭·헤드 드래그·범위 선택 검증. 시간 이동이 노트/클립을 변경하거나 음악 Undo를 소비하지 않음. tempo 변화·줌·스크롤 후 클릭 위치 정확. MIDI seek 후 stuck note 검사. 녹음 중 seek는 초기 버전에서 비활성화하고 이유를 안내.

## D1. 대상 우클릭 메뉴와 Inspector

| 대상 | 최소 메뉴 |
|---|---|
| 패턴 클립 | 피아노롤 열기, 이름/색, 복제, Make unique, mute, 삭제, 속성, AI에게 물어보기 |
| 오디오 클립 | 샘플/클립 편집, gain/fade/offset, 원본 위치, 복제/분할/mute/삭제, AI 첨부 |
| 노트/선택 노트 | pitch/start/length/velocity 속성, 복사/붙여넣기/삭제, quantize, transpose, AI 첨부 |
| 레인/채널 헤더 | 이름/색, 추가/복제/삭제, mute/solo, 관련 악기/Insert 이동 |
| Insert/FX 슬롯 | 플러그인 열기, bypass, 순서/대체/제거, 파라미터/라우팅, AI 첨부 |
| 노브/페이더 | 수치 입력, 기본값, 자동화 생성/열기, MIDI learn, 파라미터 AI 첨부 |
| 빈 격자/룰러 | 도구/snap, 붙여넣기 가능한 경우, 마커, 범위/루프 명령 |

메뉴와 toolbar/단축키/AI 도구는 같은 명령 검증을 호출한다. 미지원 항목을 동작하는 것처럼 표시하지 않는다. 지원 예정은 제외하고 현재 선택에만 불가능한 항목은 이유를 표시한다. 잘못된 플러그인 파라미터 단위 변환을 피하고 수치 범위를 검사한다.

완료: 메뉴 열기/취소는 데이터 불변. 다중 선택에서 명령이 전체에 적용되고 Undo 한 번. 메뉴가 열린 사이 대상 삭제/외부 변경을 안전하게 거절. 모든 패널 기본 우클릭이 예상과 일치.

## D2. 피아노롤을 자연스럽게 열고 편집하기

패턴 클립 더블클릭, 우클릭 '피아노롤', Rack 버튼, 선택 후 Enter로 같은 에디터를 연다. 헤더에 패턴/채널/선택 배치와 공유 참조 수를 표시한다. 한 패턴에 여러 악기가 있으면 현재 채널을 명확히 선택할 수 있어야 한다. '이 배치만 편집'은 Make unique 후 편집, '패턴 편집'은 모든 참조 반영으로 구분한다.

기존 피아노롤에 Select/Draw 도구, 노트 본문 이동/끝 길이 조절, Shift 추가선택, 빈 곳 드래그 박스선택(Select), 노트 복제, velocity lane, 수치 편집, snap/triplet, quantize 강도, 반음/옥타브 이동을 제공한다. Draw에서 빈 곳 클릭은 노트 생성, Select에서는 생성하지 않는다. 도구별 역할을 화면에 표시한다.

건반 좌클릭 미리듣기는 mouse-up/포커스 상실에서 note-off를 보장한다. 드래그 음정 미리듣기는 프로젝트 녹음과 분리한다. 다른 채널 ghost note는 선택적 표시이며 직접 편집 대상으로 오해하지 않게 한다. 에디터는 크기 조절/최대화/복원, 가능하면 하단 dock와 별도 창을 동일 모델에 연결한다. 별도 창 도입은 필수 기본 조작 뒤에 한다.

완료: 클립에서 진입→12개 노트 작성→다중 이동/길이/velocity→Undo→원 Playlist 반영을 실제 마우스로 확인. 더블클릭 진입이 첫 클릭의 새 노트/클립 생성이나 이동을 남기지 않음. 재생 중 외부 노트 변경도 같은 창에 반영.

## D3. 편곡·스크롤·편집 감각

- Playlist 기본 Select, 명시적 Draw/Paint/Erase/Split 도구. 빈 곳 드래그는 선택이며 Draw에서만 배치한다.
- 클립 본문 이동, 끝 trim, Ctrl+드래그 복제, snap 임시 해제는 Alt. 오디오 길이 변경의 trim/stretch 모드를 명시적으로 선택한다. 드래그 끝에서 원치 않는 새 소스 파일을 만들지 않는다.
- 세로 휠 스크롤, Shift+휠 가로, Ctrl+휠 포인터 중심 가로 줌, 가운데 버튼 드래그 pan을 Playlist/PianoRoll에서 통일한다. 중앙 버튼 없는 장치에도 스크롤바 제공.
- 선택 영역 맞춤 줌, 전체 곡 맞춤, 이전 줌 복원. 가장자리 드래그 자동 스크롤, 레인 높이 조절, 시간선/마커와 소절 격자 명도 차이.
- 자동화 점 선택/이동/삭제는 같은 입력 규칙, 곡선 modifier는 충돌 없는 별도 명세. 기존 Alt velocity/선택과 충돌하는 곳을 입력 맵으로 정리 후 변경한다.

완료: 줌/스크롤 후 포인터 위치와 음악 시간이 일치. 다중 편집의 상대 간격 유지. Ctrl+드래그 취소 시 복제 잔여물 없음. 창 가장자리에서도 드래그 완료 가능. 이미지/스크립트 호출 검사만으로 완료 처리하지 않는다.

## D4. 믹서·Browser·창·단축키 수용 검사

FX 더블클릭으로 플러그인 창, 노브 더블클릭 기본값/우클릭 수치 입력, channel↔insert 선택 연결, mute/solo 복원 정책을 통일한다. Browser 샘플 클릭은 선택, 명시적 preview는 청음, 드래그는 Rack sampler 또는 Playlist audio 배치로 구분한다. 파일 검색 중 Delete는 자산 삭제를 실행하지 않는다.

패널은 기존 — □ × 규약과 크기 저장을 따른다. 미니맵/오버뷰, 키보드 재지정, 독립 모니터 창은 기반 완료 후 확장이다. 초기 단축키 충돌표를 하나 유지하고 메뉴에 실제 단축키를 표시한다. AI 채팅의 입력 포커스와 DAW 재생/삭제 단축키 충돌을 반드시 검사한다.

완료: 1280×720 및 실제 DPI에서 루프 설정→패턴 더블클릭→노트 작성→우클릭 quantize→편곡 복제→FX 열기→AI 첨부를 수행. 재생 위치·선택·음악 데이터가 기대대로 유지. 새 마우스 사양의 자동 회귀와 실제 조작 기록을 함께 남긴다.

## 우선순위와 Claude 인수인계

D0 → D1 → D2 → D3 → D4. A0 선택 첨부는 D0/D1의 선택/범위 계약과 함께 구현한다. 내부 채팅만 완성하고 기본 마우스 동작은 후순위로 밀지 않는다. 이번 작업은 명세 작성이며 소스 동작을 바꾸지 않았다.

Claude: 최신 코드와 이 문서를 비교한 뒤 D0부터 수정해줘. 기존 피아노롤/메뉴를 재사용하고, 우클릭 삭제를 기본 context menu로 바꾸면서 기존 테스트 기대값도 갱신해. FL/Cubase 참고 사실과 CoCompose에서 선택한 단축키는 구분해. 실제 pointer 이벤트 검사와 사용자 조작 기록을 남기고 라이브 싱크/Undo 회귀를 확인해.
