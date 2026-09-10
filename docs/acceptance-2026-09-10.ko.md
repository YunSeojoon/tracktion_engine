# CoCompose 수용 검사 기록

P2. 이 문서의 표는 스크립트가 기계에서 읽어 채웠고, 아래 체크리스트는 사람이 채운다.
빈 항목은 통과가 아니라 미검증이다.

## 이 기계

| 항목 | 값 |
|---|---|
| 앱 | CoCompose 0.1.0 |
| 커밋 | 25b4ad8135d8 |
| 오디오 장치 | スピーカー(4- MosArt USB Audio Device) (Windows Audio) |
| 샘플레이트 / 버퍼 | 48000.0 Hz / 480 samples |
| 출력 지연 | 480 samples |
| MIDI 입력 | FL Bridge Out (켜짐), FL Bridge In (켜짐), Impact LX Mini (켜짐), MIDIIN2 (Impact LX Mini) (켜짐), All MIDI Ins (켜짐) |
| 오디오 입력 | Input channel 1 + 2 (켜짐) |
| 건반 검사 가능 | 가능 |
| 스캔된 플러그인 | 악기 12, 효과 11 |
| 프로젝트 폴더 | `C:\Users\yun seojoon\AppData\Local\Temp\claude\C--project-private-tracktion-engine\bd054f00-ccd2-47eb-aa78-5f39c9993f38\scratchpad\acceptance\session` |

## 스크립트가 확인한 것

| 검사 | 결과 |
|---|---|
| 재생 중 외부 편집이 열린 프로젝트에 닿음 | O |
| 그동안 세션이 바뀌지 않음 | O |
| 재생이 시작됨 | O |
| 저장 후 다시 열기 | O |
| 믹스 렌더 | 60.0초, peak 0.1838 |
| 렌더 파일 SHA-256 | `CF96D6E3D17D76604F8EB50F2FB4C5CC1E902F3795EEA79179F73A4C5396C218` |

## 사람이 채우는 항목

각 줄에 실제로 한 것과 결과를 적는다. 하지 않았으면 `미검증`으로 남긴다.

| 항목 | 어떻게 | 결과 |
|---|---|---|
| 악기 음색 선택 | 채널의 `...` 버튼으로 플러그인 창을 열고 실제로 쓸 음색을 고른다 | |
| 음색 바꾸고 바로 렌더 | 음색을 바꾸자마자 Export WAV. 들리는 것과 파일이 같은가 | |
| MIDI 건반 녹음 | 채널을 Arm하고 Record. 친 것이 패턴으로 들어왔는가 | |
| 오디오 입력 녹음 | 오디오 입력을 Arm하고 Record. 파형이 들어왔는가 | |
| 재생 중 장치 전환 | 재생 중 Tools > Audio settings에서 다른 장치로 바꾼다. 앱이 살아 있고 소리가 이어지는가 | |
| 인터페이스 분리 | 재생 중 USB 인터페이스를 뽑는다. 앱이 죽지 않고 상태를 알리는가 | |
| 저장 / 복원 | 닫았다 다시 연다. 고른 음색과 설정이 그대로인가 | |
| 청음 | 렌더된 WAV를 듣는다. 무음·클리핑·끊김·타이밍 | |

## 무음이었던 플러그인

`docs/compatibility-2026-09-10.ko.md`에서 렌더 peak가 0이었던 악기들은 자기 라이브러리를
불러와야 소리가 난다고 적었다. 그것은 아직 추정이다. 위에서 음색을 실제로 고른 뒤에도
무음이면 원인이 다른 것이므로 여기에 적는다.

| 플러그인 | 음색을 고른 뒤 | 비고 |
|---|---|---|
| VOCALOID VSTi | | |
| SINE Player | | |
| sforzando | | |
| Kontakt 8 | | |
| Ample Guitar M | | |
| Ample Guitar LP | | |
| Ample Bass U | | |
| Ample Bass P | | |

## 판정

위 사람 항목이 모두 채워지기 전까지 P2는 완료가 아니다.
