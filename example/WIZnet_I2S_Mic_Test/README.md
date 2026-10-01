# INMP441 I2S capture check

W6300-EVB-Pico2(RP2350)에서 INMP441 마이크를 PIO I2S로 읽는 단독 검증 프로젝트다.
전시 펌웨어에 합치기 전에 네 가지를 먼저 확인하기 위한 것이다.

1. 마이크가 반응하는가, 아니면 데이터 선이 죽어 있는가
2. 샘플레이트가 요청한 값대로 나오는가 — **시스템 클럭이 200 MHz**다. 대부분의 I2S 예제는 125 또는 150 MHz 기준으로 분주비가 박혀 있어서 그대로 가져오면 틀어진다
3. 왼쪽 채널만 살아 있는가 — `L/R`을 GND에 묶었을 때 그래야 한다. 오른쪽에도 값이 들어오면 `L/R`이 떠 있는 것이다
4. DC 오프셋이 얼마인가 — INMP441은 오프셋이 있어서, 무음이 0으로 읽힌다고 가정하면 안 된다

## 결선

```
INMP441        W6300-EVB-Pico2
VDD      ->    3V3 (OUT)       핀 36
GND      ->    GND             핀 38
SCK      ->    GP2             핀 4     BCLK
WS       ->    GP3             핀 5     LRCL
SD       ->    GP4             핀 6     DATA
L/R      ->    GND                      왼쪽 채널
```

**3.3V 전용이다. 5V를 넣지 마라.**

GP2와 GP3이 붙어 있어야 한다. PIO가 BCLK와 WS를 side-set 2비트로 함께 내보내기 때문에
두 핀이 연속이어야 한다. SD는 입력이라 아무 핀이나 된다.

이 세 핀을 고른 이유는 전시 빌드에서 **GP2, GP3, GP4만 비어 있기 때문**이다.
GP5~GP14는 카메라 PIO가, GP15~GP22는 W6300이, GP26~GP29는 카메라 SPI가 쓴다.

## 빌드

저장소 최상위에서 구성하고 이 타깃만 빌드한다. 보드·플랫폼·SDK 설정은 전시 빌드와
같은 최상위 `CMakeLists.txt`에서 온다. **200 MHz에서 맞는 분주비가 150 MHz에서
틀리는 종류의 문제를 따로 빌드하면 못 잡는다.**

```bash
cd D:/theo_git_project/ArduCam_Mega_X_WIZnet_Pico_UDP_Streaming
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target i2s_mic_test
```

결과물은 `build/example/WIZnet_I2S_Mic_Test/i2s_mic_test.uf2`다. BOOTSEL로 올린다.
콘솔은 USB 시리얼.

`PICO_SDK_PATH`가 설정돼 있어야 한다. 이 머신에서는 `D:\RP2040\pico-sdk`다.

`--target`을 빼면 카메라와 전시 이미지까지 전부 빌드된다. 이 검증 단계에서는 필요 없다.

## 출력 읽는 법

부팅 시 설정을 먼저 찍는다.

```
sys clock      : 200000000 Hz
pins           : BCLK=GP2  WS=GP3  SD=GP4
target rate    : 16000 Hz  (BCLK 1024000 Hz)
pio            : block 0  sm 0  offset 0
clkdiv         : 97.656250 requested -> 97.656250 programmed
expected rate  : 16000.00 Hz  (0.000 % off target)
```

16 kHz는 200 MHz에서 분주비가 정확히 떨어진다(97 + 168/256). 48 kHz로 바꾸면
0.04 % 정도 오차가 남는데, 음성 용도라면 문제없다.

그 다음 1초마다 한 줄씩 나온다.

```
[########            ] rate 16000.1 Hz | L rms    12043 peak   184320 | R peak        0 | DC L     -2150 R         0
```

| 항목 | 정상 | 이상하면 |
|---|---|---|
| `rate` | 16000 ± 몇 Hz | 크게 다르면 분주비나 클럭 문제 |
| `L peak` | 조용할 때 작게, 말하면 크게 | 항상 0이면 SD 결선/전원 |
| `R peak` | **거의 0** | 값이 크면 `L/R`이 떠 있다 |
| `DC L` | 0이 아닌 고정값 | 정상이다. 다운스트림에서 빼야 한다 |
| `OVERRUN` | 안 나와야 함 | 나오면 메인 루프가 못 따라가는 것 |

막대는 피크를 제곱근 스케일로 그린 것이다. 조용한 신호도 보이게 하려고 그렇게 했다.

첫 1초에는 원시 32비트 워드 4개와 L 채널의 최소/최대도 같이 찍는다.
전부 `00000000`이거나 전부 `ffffffff`면 데이터 선 문제다.

## 통과 기준

- `rate`가 16000에서 ±5 Hz 이내
- 손뼉을 치면 `L peak`가 확 뛰고 `R peak`는 그대로
- `OVERRUN` 0
- 원시 워드의 하위 8비트가 0 (INMP441은 24비트를 32비트 슬롯에 왼쪽 정렬해서 보낸다)

여기까지 되면 전시 빌드에 합친다.

## 전시 빌드에 합칠 때

`i2s_rx.pio`와 DMA 설정을 그대로 가져가면 되지만 두 가지를 지켜야 한다.

- **PIO 블록과 상태머신을 하드코딩하지 마라.** 여기서 쓴
  `pio_claim_free_sm_and_add_program_for_gpio_range()`를 그대로 쓴다.
  카메라가 `pio0` + `sm 0`을 명시적으로 잡고, W6300 QSPI도 PIO로 동작한다.
  RP2350은 블록 3개에 상태머신 12개라 개수는 넉넉하지만 충돌은 피해야 한다.
- **DMA 채널도 `dma_claim_unused_channel()`로 받는다.** 카메라가 이미 그렇게 쓴다.

합친 뒤에는 **fps 회귀를 반드시 측정한다.** I2S DMA가 계속 돌면 카메라 캡처가
VSYNC 직전 블랭킹 구간에 DMA를 거는 타이밍이 흔들릴 수 있다.
기준값은 TOE 15분에 19.2 fps, 프레임 19,457, 드롭 2다.

## 알아둘 것

- **`-O3`로 빌드하지 마라.** 같은 저장소의 카메라 예제에서 `Release`(`-O3`)가
  `Debug`(`-Og`)보다 느렸다. 720p가 15 fps로 주저앉았다. 이 프로젝트 자체는
  영향이 작지만 습관을 맞춰두는 게 낫다.
- INMP441은 클럭이 들어온 뒤 잠깐은 의미 없는 값을 낸다. `main()`이 첫 200 ms를
  버린다.
- 샘플레이트를 바꾸려면 `main.c`의 `SAMPLE_RATE`만 고치면 된다. BCLK는 자동으로
  `SAMPLE_RATE * 64`가 된다.
