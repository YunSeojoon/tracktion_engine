# CoCompose UI 폴리싱 애셋

## 준비된 이미지

- `assets/branding/welcome-waves-v1.png`: 1536×1024 시작 화면 아트워크. 내장 image_gen 도구로 생성. 두 겹의 음향 리본과 어두운 여백으로 공동 작곡을 표현한다.
- 생성 결과를 직접 시각 확인했다. 문자/버튼 없이 하단 여백이 있으며 시작 화면의 제목과 동작은 JUCE 텍스트/컴포넌트로 별도 배치한다.
- 현재는 사전 준비된 애셋이다. 앱 코드에 연결하거나 시작 화면을 새로 구현하지 않았다. 시작 화면 도입 시에만 사용하며 기존 프로젝트를 바로 여는 흐름을 막지 않는다.

## 구현 시 유지할 규칙

작업 영역의 배경·파형·노트·노브·미터·아이콘은 코드/벡터로 만든다. 아트워크를 타임라인이나 믹서 뒤에 넣지 않는다. 래스터 텍스트나 조작 컨트롤을 만들지 않는다. 앱 아이콘은 작은 크기 가독성을 위해 별도 벡터 심볼과 Windows ICO로 제작하는 후속 작업이다. 이 큰 아트워크를 축소해 아이콘으로 사용하지 않는다.

P4에서 색상/간격/글꼴/상태 토큰을 현재 UI와 통합하고, 초기 제안 색상은 charcoal 배경과 절제된 teal 강조로 한다. 최종 대비는 실제 텍스트/컨트롤 조합에서 검증한다. 이미지 생성으로 UI 폴리싱 완료를 선언하지 않는다.

## 최종 생성 프롬프트

아래는 welcome-waves-v1의 프롬프트다. 추가된 controls-reference-v1 프롬프트는 문서 마지막에 있다.

Use case: stylized-concept. Asset type: production background artwork for the welcome screen of CoCompose, a professional music composition desktop app. Create one refined widescreen 1536x1024 abstract acoustic sculpture: two delicate intertwined ribbons made of closely spaced fine metallic strands, suggesting two musical voices composing together, with subtle waveform-like undulations. Studio-rendered dimensional material, graphite and muted silver with restrained teal reflections. Dark charcoal seamless background, generous quiet negative space around sculpture for separately rendered app title and controls. Sculpture concentrated in central upper half; bottom quarter almost plain dark charcoal. Sophisticated, calm, tactile, precise, no loud neon or sci-fi aesthetic. No text, letters, logos, UI, buttons, frames, watermarks or multiple panels. This is a decorative welcome asset, not a screenshot, not a diagram.

## 이미지별 구체적인 사용법

| 파일 | 성격 | 사용 위치 | 배포 포함 |
|---|---|---|---|
| assets/branding/welcome-waves-v1.png | 실제 사용 가능한 1536×1024 배경 | 시작/최근 프로젝트 화면 상단 브랜드 영역 | 해당 화면 구현 시 포함 |
| assets/design/controls-reference-v1.png | 1536×1024 디자인 참고 시트 | 개발자가 노브·페이더·미터를 구현할 때 참고 | 런타임에는 포함하지 않음 |

### 시작 화면 아트워크

1. 기본 레이아웃은 상단 이미지 영역과 하단 프로젝트 동작 영역으로 분리한다. 이미지 영역 권장 크기는 논리 픽셀 기준 600×300, 좁은 창에서는 400×220까지 줄인다. 작곡 작업 영역을 줄여 이 화면을 유지하지 않는다.
2. 원본 종횡비 3:2를 유지해 contain으로 표시하고 남는 영역은 #191D20 계열 단색으로 채운다. 원본을 늘여 변형하지 않는다. 가로로 긴 슬롯에서 cover를 선택하면 중앙 리본이 잘리는지 실제 화면으로 확인한다.
3. CoCompose 제목, 버전, 최근 프로젝트, 새 프로젝트/열기 버튼은 이미지에 굽지 않고 JUCE Label/Button으로 만든다. 가독성이 불확실한 이미지 위에 버튼을 놓지 않는다. 하단 별도 단색 패널에 텍스트와 버튼을 배치하는 것을 기본으로 한다.
4. CMake JUCE BinaryData 리소스로 넣거나 명시적 설치 리소스로 묶는다. 개발자 컴퓨터의 절대 경로를 런타임에서 읽지 않는다. 이미지 디코딩은 화면 생성 시 한 번만 수행하고 paint에서 다시 로드하지 않는다.
5. 이미지 누락/디코딩 실패 시 단색 배경과 제목으로 정상 시작한다. 최근 프로젝트 자동 복원을 늦추거나 이 화면에서 추가 클릭을 강제하지 않는다.

### 노브 시안 해석과 구현

시트의 금속 면·얇은 포인터·외곽 값 arc를 참고하되, 시트를 잘라 스프라이트로 쓰지 않는다. JUCE LookAndFeel::drawRotarySlider에서 동일한 시각 요소를 벡터로 그린다. 픽셀 질감과 glow는 작은 컨트롤에서는 생략한다.

**시안의 의미상 보정:** 생성 이미지의 PAN은 중앙 포인터인데 arc가 왼쪽부터 채워져 있다. 실제 bipolar pan은 중앙 0에서 현재 값 방향으로만 arc를 채워야 하며, 중앙에서는 값 arc가 없어야 한다. 노브 포인터와 arc 끝은 모든 값에서 일치해야 한다. 시안보다 아래 동작 명세가 우선한다.

- 크기: compact pan/send 지름 24~28 논리 px, 일반 36~40, 큰 filter 56. hit area는 최소 32×32를 확보한다. 크기는 DPI 픽셀이 아닌 JUCE 논리 단위다.
- 회전: 하단에 90도 빈 구간을 둔 270도 sweep. JUCE 각도 기준 시작 1.25π, 끝 2.75π. 정규화 값 t에 angle=start+t*(end-start)를 적용한다. 주파수 등 skew가 있는 값은 slider의 정규화 변환 결과를 쓴다.
- 층: 어두운 원형 면 → 얇은 외곽선 → 비활성 트랙 arc → 값 arc → 포인터 → 포커스 링. 작은 노브에는 복잡한 그림자나 텍스처를 넣지 않는다.
- normal: 흑연색 면, 은색 포인터, 절제된 teal arc. hover: 외곽선 명도만 올린다. dragging: 현재 수치가 보이도록 한다. focus: 별도 1~2px 링. disabled: 낮은 대비와 입력 비활성화, 마지막 값 위치는 유지한다. 색만으로 상태를 구분하지 않는다.
- 상하 드래그로 수정, Shift 미세 조절, 더블클릭은 파라미터 기본값 복원, 키보드 화살표 조절과 포커스 이동을 지원한다. 패널 스크롤 중 의도치 않은 휠 값 변경을 피하도록 휠 정책을 통일한다.
- 포인터 아래/툴팁에 단위가 포함된 값 표시: pan L/C/R, gain dB, cutoff Hz/kHz, send dB. 접근성 이름에는 채널과 파라미터 이름을 넣는다.
- 드래그 한 번은 Undo 한 작업이다. UI와 외부 AI 변경은 같은 파라미터 경로를 사용한다. 외부 변경 시 포인터/수치/arc를 함께 갱신한다. 드래그 도중 외부 수정 충돌 정책은 기존 revision 정책과 일치시킨다.

### 페이더와 미터

페이더는 폭 14~18px 손잡이와 최소 32px 조작 폭, 0dB 기준선을 제공한다. 손잡이 색은 선택 상태를 보조하며 mute/solo는 별도 버튼으로 표현한다. 값의 비선형 매핑은 기존 엔진/슬라이더 설정을 유지하고 그림의 위치로 dB를 추정하지 않는다.

미터는 실제 엔진 측정값으로 표시한다. 녹색 기본 레벨·황색 상단·적색 clip 표시와 peak hold/reset을 구분한다. 그림의 고정 막대를 가져오지 않는다. 무음/장치 미연결 상태를 구분하고 약 30Hz 시각 갱신으로 시작해 부하를 확인한다. 오디오 스레드에서 UI repaint나 이미지 처리를 하지 않는다.

## 구현 순서와 시각 검수

1. 공통 색/간격/폰트와 노브 LookAndFeel부터 하나의 채널에 적용한다.
2. min/25%/center/75%/max 값에서 arc·포인터 일치, hover/focus/disabled, pan 중앙, 긴 이름과 큰 단위 표시를 확인한다.
3. Mixer/Rack에 확장하고 키보드·드래그·Undo·외부 값 수정 회귀를 검사한다.
4. 시작 화면이 필요할 때 아트워크를 연결한다. 이미지 때문에 시작 화면을 반드시 새로 만들 필요는 없다.
5. 100/150/200% 실제 배율에서 비교 스크린샷을 남긴다. 이미지 시안만으로 폴리싱 완료 처리하지 않는다.

## controls-reference-v1 생성 기록

내장 image_gen으로 생성하고 직접 시각 확인했다. 앱 코드는 아직 변경하지 않았다. 아래 프롬프트로 만든 참고 시트이며, 개별 노브 상태의 동작과 수치 정확성은 위 명세에 따라 구현한다.

Use case: ui-mockup. Create a precise industrial UI component design reference sheet for CoCompose professional audio workstation, landscape 1536x1024. Charcoal matte background. Top row four large identical rotary knobs front-on orthographic: normal, hovered, focused with fine teal outer ring, disabled. Each knob graphite machined matte face, very subtle beveled edge, thin silver radial pointer, restrained teal value arc outside the face covering a 270 degree sweep with gap at bottom. No numbers printed on faces. Middle row: compact mixer pan knob with centered pointer and bipolar arc, large filter knob, small send knob, all same design language. Bottom row: simple vertical dark mixer fader with silver handle and teal selection accent and a narrow stereo meter green with amber peak. Generous spacing, flat readable professional DAW UI, understated shading, no photoreal desk or hardware or perspective. Labels only: NORMAL, HOVER, FOCUS, DISABLED, PAN, FILTER, SEND, FADER. Crisp typography. This is a visual design reference sheet, not a sprite atlas, no app screenshot or branding.
