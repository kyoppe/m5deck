#include <M5Unified.h>
#include <WiFi.h>
#define ENABLE_LEGACY_EXTRAS 0
#if ENABLE_LEGACY_EXTRAS
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#endif
#include <Wire.h>
#include <time.h>

#include "secrets.h"
#include "agav_network.h"
#include "agavydration.h"
#include "agav_ui.h"
#include "agav_thumb.h"
#include "MujiNum.h"  // 無印 駅の時計用の数字フォント(Jost*)
#include "Seg7.h"     // デジタル時計用 7セグフォント(DSEG7 Classic Bold)
#if ENABLE_LEGACY_EXTRAS
#include "AngryVoice.h"  // 持ち上げ時の怒りボイス(WAV, Kyoko)
#endif

// ---- 見た目の設定 -------------------------------------------------------
static constexpr uint16_t COLOR_BG = TFT_BLACK;
// ターミナル風のフォスファーグリーン（RGB565）
static constexpr uint16_t COLOR_GREEN = lgfx::color565(0, 255, 70);
static constexpr uint16_t COLOR_DIM = lgfx::color565(0, 110, 30);

// アナログ時計（無印 駅の時計）の色
static constexpr uint16_t COLOR_WHITE = TFT_WHITE;
static constexpr uint16_t COLOR_BLACK = TFT_BLACK;

// 320x240（横向き）に描くためのオフスクリーンキャンバス（ちらつき防止）
static M5Canvas canvas(&M5.Display);

#if ENABLE_LEGACY_EXTRAS
// Legacy extras: Datadog alert, schedule reminder, movement reaction, and mute.
static constexpr int MUTE_W = 196;
static constexpr int MUTE_H = 150;
static constexpr uint8_t MUTE_ALPHA = 115;  // 0..255（薄め＝約45%）
static M5Canvas muteSpr(&M5.Display);
static bool muteSprReady = false;
#endif

// 時刻文字（HH:MM）のレイアウト（DSEG7 7セグ）
static float timeScale = 1.0f;
static int hhX = 0, mmX = 0, timeY = 0;
static int colonCx = 0;    // 四角コロンの中心X
static int colonSide = 0;  // コロン四角の一辺
static int colonDy = 0;    // 中心からの上下オフセット

static bool wifiOk = false;
static bool ntpOk = false;

// ---- パネル切替 ---------------------------------------------------------
enum Panel { PANEL_DIGITAL = 0, PANEL_ANALOG = 1, PANEL_COUNT = 2 };
static int currentPanel = PANEL_DIGITAL;

// ---- 重量モード（Mini Scales Unit @ I2C 0x26, PORT-A G32/G33）---------
// ボタンAでON/OFF。重量モード中はBでタレ(ゼロ調整)。
static constexpr uint8_t MINI_SCALE_ADDR = 0x26;
static constexpr int SCALE_SDA = 32;
static constexpr int SCALE_SCL = 33;
// ユニット内蔵フィルタ (I2C レジスタ)
static constexpr int SCALE_HW_AVG_LEVEL = 32;
static constexpr int SCALE_HW_EMA_ALPHA = 12;
static constexpr float WEIGHT_SW_EMA_ALPHA_UP = 0.38f;
static constexpr float WEIGHT_SW_EMA_ALPHA_DOWN = 0.10f;
static constexpr float WEIGHT_SW_EMA_ALPHA_IDLE = 0.06f;
static constexpr float WEIGHT_DEADBAND_G = 0.5f;
static constexpr float WEIGHT_IDLE_DEADBAND_G = 1.2f;
static constexpr float WEIGHT_WIND_IGNORE_G = 3.5f;
static constexpr float WEIGHT_ZERO_CLAMP_G = 1.2f;
static constexpr float WEIGHT_DISPLAY_HOLD_G = 0.35f;
static constexpr uint32_t WEIGHT_DISPLAY_HOLD_MS = 180;
static constexpr float WEIGHT_DISPLAY_SNAP_G = 5.0f;
// 空載判定 (ヒステリシス + 安定時間。載せ途中の値をゼロ扱いしない)
static constexpr float WEIGHT_EMPTY_G = 1.5f;
static constexpr float WEIGHT_LOADED_G = 4.0f;
static constexpr uint32_t WEIGHT_EMPTY_HOLD_MS = 700;

enum WeightLoadState { WLS_IDLE = 0, WLS_LOADED = 1 };
static WeightLoadState weightLoadState = WLS_IDLE;
static uint32_t emptyStableSinceMs = 0;

static bool weightMode = false;
static bool manualTareActive = false;
static uint32_t weightLastActivityMs = 0;
static constexpr uint32_t WEIGHT_MODE_TIMEOUT_MS = 60UL * 1000UL;
static bool scaleSessionReady = false;
static bool scaleWireReady = false;
static bool scaleFiltersReady = false;
static bool scaleFound = false;
static float weightTareG = 0.0f;
static float weightRawG = 0.0f;
static float weightGrams = 0.0f;  // 表示用 (フィルタ後)
static uint32_t weightAdc = 0;
static uint32_t lastWeightRead = 0;
static uint32_t lastWeightLog = 0;
static float weightEma = 0.0f;
static bool weightEmaInit = false;
static float scaleGap = 0.0f;
static bool scaleGapOk = false;
static int32_t scaleZeroAdc = 0;
static float weightUnitG = 0.0f;
static float weightDisplayG = 0.0f;
static uint32_t weightDisplayHoldMs = 0;
static char weightHint[48] = "";

enum ScaleZeroPhase {
  SCALE_ZERO_IDLE = 0,
  SCALE_ZERO_WARMUP,
  SCALE_ZERO_CAPTURE,
};
static ScaleZeroPhase scaleZeroPhase = SCALE_ZERO_IDLE;
static bool scaleZeroManual = false;
static int scaleZeroSampleCount = 0;
static int64_t scaleZeroAdcSum = 0;
static uint32_t scaleZeroLastSampleMs = 0;
static constexpr int SCALE_ZERO_WARMUP_SAMPLES = 12;
static constexpr int SCALE_ZERO_CAPTURE_SAMPLES = 12;

static int32_t miniScaleAdcU32ToS32(uint32_t adc) {
  int32_t v;
  memcpy(&v, &adc, sizeof(v));
  return v;
}

static bool miniScaleReadGap(float *gapOut);
static bool miniScaleResetOffset();

static void initScaleWire() {
  if (scaleWireReady) return;
  Wire.begin(SCALE_SDA, SCALE_SCL);
  Wire.setClock(400000);
  scaleWireReady = true;
}

static bool miniScaleWriteReg(uint8_t reg, const uint8_t *data, size_t len) {
  Wire.beginTransmission(MINI_SCALE_ADDR);
  Wire.write(reg);
  for (size_t i = 0; i < len; i++) Wire.write(data[i]);
  return Wire.endTransmission() == 0;
}

static void initScaleFilters() {
  if (!scaleFound) return;
  if (scaleFiltersReady) return;
  const uint8_t lpfOn = 0x01;
  const uint8_t avgLevel = (uint8_t)SCALE_HW_AVG_LEVEL;
  const uint8_t emaAlpha = (uint8_t)SCALE_HW_EMA_ALPHA;
  const bool ok = miniScaleWriteReg(0x80, &lpfOn, 1) &&
                  miniScaleWriteReg(0x81, (const uint8_t *)&avgLevel, 1) &&
                  miniScaleWriteReg(0x82, (const uint8_t *)&emaAlpha, 1);
  scaleFiltersReady = ok;
  Serial.printf("scale filters -> %s (lpf=on avg=%d ema=%d)\n", ok ? "OK" : "FAIL",
                SCALE_HW_AVG_LEVEL, SCALE_HW_EMA_ALPHA);
}

static void resetWeightFilter() {
  weightEmaInit = false;
  weightGrams = 0.0f;
}

static void resetWeightState() {
  resetWeightFilter();
  weightLoadState = WLS_IDLE;
  emptyStableSinceMs = 0;
}

static void updateWeightLoadState(float rawG, uint32_t nowMs) {
  if (rawG >= WEIGHT_LOADED_G) {
    weightLoadState = WLS_LOADED;
    emptyStableSinceMs = 0;
    return;
  }
  if (rawG < WEIGHT_EMPTY_G) {
    if (emptyStableSinceMs == 0) emptyStableSinceMs = nowMs;
    if (weightLoadState == WLS_LOADED &&
        nowMs - emptyStableSinceMs >= WEIGHT_EMPTY_HOLD_MS) {
      weightLoadState = WLS_IDLE;
    }
    return;
  }
  // 3..6 g のヒステリシス帯: 状態維持、空載タイマーリセット
  emptyStableSinceMs = 0;
}

static bool weightEmptyStable(float rawG, uint32_t nowMs) {
  if (weightLoadState != WLS_IDLE) return false;
  if (fabsf(rawG) >= WEIGHT_EMPTY_G) return false;
  if (emptyStableSinceMs == 0) return false;
  return (nowMs - emptyStableSinceMs) >= WEIGHT_EMPTY_HOLD_MS;
}

static float applyWeightFilter(float rawG, bool snapZero) {
  // 空載が安定したときだけ 0 表示 (載荷中はフィルタ遅延でもゼロにしない)
  if (snapZero) {
    weightEma = 0.0f;
    weightGrams = 0.0f;
    weightEmaInit = true;
    return 0.0f;
  }
  if (!weightEmaInit) {
    weightEma = rawG;
    weightGrams = rawG;
    weightEmaInit = true;
    return weightGrams;
  }
  float filterIn = rawG;
  if (weightLoadState == WLS_IDLE && fabsf(rawG) < WEIGHT_WIND_IGNORE_G) {
    filterIn = 0.0f;
  }
  float alpha;
  if (weightLoadState == WLS_IDLE) {
    alpha = WEIGHT_SW_EMA_ALPHA_IDLE;
  } else {
    alpha = (fabsf(filterIn) > fabsf(weightEma)) ? WEIGHT_SW_EMA_ALPHA_UP
                                                : WEIGHT_SW_EMA_ALPHA_DOWN;
  }
  weightEma += alpha * (filterIn - weightEma);
  if (weightLoadState == WLS_LOADED && fabsf(rawG - weightEma) > 8.0f) {
    weightEma = rawG;
  }
  const float deadband =
      (weightLoadState == WLS_IDLE) ? WEIGHT_IDLE_DEADBAND_G : WEIGHT_DEADBAND_G;
  if (fabsf(weightEma - weightGrams) >= deadband) {
    weightGrams = weightEma;
  }
  return weightGrams;
}

static bool miniScaleProbe() {
  Wire.beginTransmission(MINI_SCALE_ADDR);
  return Wire.endTransmission() == 0;
}

static bool miniScaleGapSane(float gap) {
  if (!isfinite(gap)) return false;
  const float a = fabsf(gap);
  return a >= 100.0f && a <= 1000000.0f;
}

static void refreshScaleGap() {
  float gap = 0.0f;
  scaleGapOk = miniScaleReadGap(&gap) && miniScaleGapSane(gap);
  if (scaleGapOk) scaleGap = gap;
}

static float miniScaleGramsFromAdcDelta(int32_t adcDelta) {
  if (!scaleGapOk || scaleGap == 0.0f) return 0.0f;
  return (float)adcDelta / scaleGap;
}

static bool miniScaleReadRaw(uint32_t *adcOut, float *gramsOut);

static bool miniScaleCaptureZeroAdc(int32_t *adcOut, int samples = 12) {
  int64_t sum = 0;
  for (int i = 0; i < samples; i++) {
    uint32_t adcU = 0;
    float g = 0.0f;
    if (!miniScaleReadRaw(&adcU, &g)) return false;
    sum += miniScaleAdcU32ToS32(adcU);
    delay(40);
  }
  *adcOut = (int32_t)(sum / samples);
  return true;
}

static bool miniScaleReadRaw(uint32_t *adcOut, float *gramsOut) {
  uint8_t buf[4];

  Wire.beginTransmission(MINI_SCALE_ADDR);
  Wire.write(0x10);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MINI_SCALE_ADDR, 4) != 4) return false;
  for (int i = 0; i < 4; i++) buf[i] = Wire.read();
  float grams;
  memcpy(&grams, buf, sizeof(grams));

  Wire.beginTransmission(MINI_SCALE_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MINI_SCALE_ADDR, 4) != 4) return false;
  for (int i = 0; i < 4; i++) buf[i] = Wire.read();
  uint32_t adc;
  memcpy(&adc, buf, sizeof(adc));

  if (adcOut) *adcOut = adc;
  if (gramsOut) *gramsOut = grams;
  return true;
}

static bool scaleZeroActive() {
  return scaleZeroPhase != SCALE_ZERO_IDLE;
}

static void updateZeroingDisplay(uint32_t adcU) {
  weightAdc = adcU;
  const int32_t adcS = miniScaleAdcU32ToS32(adcU);
  weightRawG = miniScaleGramsFromAdcDelta(adcS - scaleZeroAdc);
  weightGrams = weightRawG;
  weightDisplayG = roundf(weightRawG * 10.0f) / 10.0f;
  weightDisplayHoldMs = 0;
}

static bool beginScaleZero(bool manual, bool warmup) {
  if (!scaleFound) return false;
  if (!scaleFiltersReady) initScaleFilters();
  refreshScaleGap();
  scaleZeroPhase = warmup ? SCALE_ZERO_WARMUP : SCALE_ZERO_CAPTURE;
  scaleZeroManual = manual;
  scaleZeroSampleCount = 0;
  scaleZeroAdcSum = 0;
  scaleZeroLastSampleMs = 0;
  manualTareActive = manual;
  resetWeightState();
  snprintf(weightHint, sizeof(weightHint), "ZEROING 0/%d",
           warmup ? SCALE_ZERO_WARMUP_SAMPLES + SCALE_ZERO_CAPTURE_SAMPLES
                  : SCALE_ZERO_CAPTURE_SAMPLES);
  return true;
}

static void serviceScaleZero(uint32_t nowMs) {
  if (!scaleZeroActive()) return;
  const uint32_t intervalMs =
      scaleZeroPhase == SCALE_ZERO_WARMUP ? 50 : 40;
  if (scaleZeroLastSampleMs != 0 &&
      nowMs - scaleZeroLastSampleMs < intervalMs) {
    return;
  }
  scaleZeroLastSampleMs = nowMs;

  uint32_t adcU = 0;
  float unitGrams = 0.0f;
  if (!miniScaleReadRaw(&adcU, &unitGrams)) {
    scaleFound = false;
    scaleFiltersReady = false;
    scaleSessionReady = false;
    scaleZeroPhase = SCALE_ZERO_IDLE;
    snprintf(weightHint, sizeof(weightHint), "I2C read fail");
    return;
  }
  weightUnitG = unitGrams;
  const int32_t adcS = miniScaleAdcU32ToS32(adcU);

  if (scaleZeroPhase == SCALE_ZERO_WARMUP) {
    if (scaleZeroSampleCount == 0) scaleZeroAdc = adcS;
    scaleZeroSampleCount++;
    updateZeroingDisplay(adcU);
    snprintf(weightHint, sizeof(weightHint), "ZEROING %d/%d",
             scaleZeroSampleCount,
             SCALE_ZERO_WARMUP_SAMPLES + SCALE_ZERO_CAPTURE_SAMPLES);
    if (scaleZeroSampleCount >= SCALE_ZERO_WARMUP_SAMPLES) {
      scaleZeroPhase = SCALE_ZERO_CAPTURE;
      scaleZeroSampleCount = 0;
      scaleZeroAdcSum = 0;
    }
    return;
  }

  scaleZeroAdcSum += adcS;
  scaleZeroSampleCount++;
  scaleZeroAdc = (int32_t)(scaleZeroAdcSum / scaleZeroSampleCount);
  updateZeroingDisplay(adcU);
  const int completed =
      (scaleZeroManual ? 0 : SCALE_ZERO_WARMUP_SAMPLES) +
      scaleZeroSampleCount;
  const int total =
      (scaleZeroManual ? 0 : SCALE_ZERO_WARMUP_SAMPLES) +
      SCALE_ZERO_CAPTURE_SAMPLES;
  snprintf(weightHint, sizeof(weightHint), "ZEROING %d/%d", completed, total);

  if (scaleZeroSampleCount >= SCALE_ZERO_CAPTURE_SAMPLES) {
    scaleZeroPhase = SCALE_ZERO_IDLE;
    scaleSessionReady = scaleFiltersReady && scaleZeroAdc != 0;
    resetWeightState();
    weightRawG = 0.0f;
    weightDisplayG = 0.0f;
    weightHint[0] = '\0';
    Serial.printf("scale zero ready -> adc=%ld manual=%d\n",
                  (long)scaleZeroAdc, scaleZeroManual ? 1 : 0);
  }
}

static void restoreScaleFilters() {
  scaleFiltersReady = false;
  initScaleFilters();
  if (!scaleFound) return;
  for (int i = 0; i < 12; i++) {
    uint32_t adc = 0;
    float g = 0.0f;
    miniScaleReadRaw(&adc, &g);
    delay(50);
  }
}

static bool pollWeight(uint32_t nowMs) {
  float unitGrams = 0.0f;
  uint32_t adcU = 0;
  if (!miniScaleReadRaw(&adcU, &unitGrams)) {
    scaleFound = false;
    scaleFiltersReady = false;
    snprintf(weightHint, sizeof(weightHint), "I2C read fail");
    return false;
  }
  if (!isfinite(unitGrams)) return false;
  if (!scaleFiltersReady) initScaleFilters();
  refreshScaleGap();
  weightAdc = adcU;
  weightUnitG = unitGrams;
  const int32_t adcS = miniScaleAdcU32ToS32(adcU);

  weightHint[0] = '\0';
  if (!scaleGapOk) {
    snprintf(weightHint, sizeof(weightHint), "GAP未設定 B長:キャリブ");
    weightRawG = 0.0f;
    weightGrams = 0.0f;
    weightEmaInit = false;
    return true;
  }

  const int32_t adcDelta = adcS - scaleZeroAdc;
  weightRawG = miniScaleGramsFromAdcDelta(adcDelta);
  updateWeightLoadState(weightRawG, nowMs);
  applyWeightFilter(weightRawG, false);
  if (fabsf(weightGrams) < WEIGHT_ZERO_CLAMP_G) weightGrams = 0.0f;

  const float rounded = roundf(weightGrams * 10.0f) / 10.0f;
  const float dispDelta = fabsf(rounded - weightDisplayG);
  if (dispDelta >= WEIGHT_DISPLAY_SNAP_G || dispDelta < 0.05f) {
    weightDisplayG = rounded;
    weightDisplayHoldMs = 0;
  } else if (dispDelta <= WEIGHT_DISPLAY_HOLD_G) {
    if (weightDisplayHoldMs == 0) weightDisplayHoldMs = nowMs;
    if (nowMs - weightDisplayHoldMs >= WEIGHT_DISPLAY_HOLD_MS) {
      weightDisplayG = rounded;
    }
  } else {
    weightDisplayG = rounded;
    weightDisplayHoldMs = 0;
  }
  return true;
}

static void miniScaleTare(bool manual = false) {
  if (!scaleFound) return;
  if (!miniScaleCaptureZeroAdc(&scaleZeroAdc)) return;
  scaleZeroPhase = SCALE_ZERO_IDLE;
  manualTareActive = manual;
  weightTareG = 0.0f;
  resetWeightState();
  weightDisplayG = 0.0f;
  weightDisplayHoldMs = 0;
  refreshScaleGap();
  scaleSessionReady = scaleFiltersReady && scaleZeroAdc != 0;
  weightHint[0] = '\0';
  Serial.printf("scale tare -> zero_adc=%ld gap=%.1f\n", (long)scaleZeroAdc,
                scaleGap);
}

// ---- キャリブレーション (10円硬貨・GAP はユニット内 Flash) ---------------
static constexpr int CAL_10YEN_COUNT = 10;      // 10円10枚 = 45.0g
static constexpr float CAL_10YEN_ONE_G = 4.5f;
static constexpr float CAL_REF_GRAMS = CAL_10YEN_ONE_G * CAL_10YEN_COUNT;
static constexpr int32_t CAL_MIN_ADC_DELTA = 80;  // 載荷でこれ未満なら失敗

enum CalStep {
  CAL_EMPTY = 0,
  CAL_LOAD,
  CAL_DONE,
  CAL_FAIL,
};

static bool calMode = false;
static CalStep calStep = CAL_EMPTY;
static uint32_t calAdcEmpty = 0;
static uint32_t calAdcLoad = 0;
static float calGramsEmpty = 0.0f;
static float calGramsLoad = 0.0f;
static float calGapWritten = 0.0f;
static float calVerifyG = 0.0f;
static int32_t calLiveAdc = 0;
static float calLiveG = 0.0f;
static char calMsg[72] = "";

static void disableScaleFiltersForCal() {
  const uint8_t off = 0;
  const int8_t zero = 0;
  miniScaleWriteReg(0x80, &off, 1);
  miniScaleWriteReg(0x81, (const uint8_t *)&zero, 1);
  miniScaleWriteReg(0x82, (const uint8_t *)&zero, 1);
  scaleFiltersReady = false;
}

static bool miniScaleReadSampleAvg(int32_t *adcOut, float *gramsOut,
                                   int samples = 16) {
  if (!scaleFiltersReady) initScaleFilters();
  int64_t adcSum = 0;
  float gSum = 0.0f;
  bool ok = true;
  for (int i = 0; i < samples; i++) {
    uint32_t adcU = 0;
    float g = 0.0f;
    if (!miniScaleReadRaw(&adcU, &g)) {
      ok = false;
      break;
    }
    adcSum += miniScaleAdcU32ToS32(adcU);
    gSum += g;
    delay(40);
  }
  if (!ok) return false;
  *adcOut = (int32_t)(adcSum / samples);
  *gramsOut = gSum / (float)samples;
  return true;
}

static bool miniScaleResetOffset() {
  const uint8_t one = 0x01;
  return miniScaleWriteReg(0x50, &one, 1);
}

static bool miniScaleReadGap(float *gapOut) {
  Wire.beginTransmission(MINI_SCALE_ADDR);
  Wire.write(0x40);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MINI_SCALE_ADDR, 4) != 4) return false;
  uint8_t buf[4];
  for (int i = 0; i < 4; i++) buf[i] = Wire.read();
  memcpy(gapOut, buf, sizeof(float));
  return true;
}

static bool miniScaleWriteGap(float gap) {
  const bool ok = miniScaleWriteReg(0x40, (const uint8_t *)&gap, sizeof(gap));
  delay(100);
  return ok;
}

static void enterCalMode() {
  scaleZeroPhase = SCALE_ZERO_IDLE;
  calMode = true;
  calStep = CAL_EMPTY;
  calMsg[0] = '\0';
  initScaleWire();
  scaleFound = miniScaleProbe();
  if (scaleFound) restoreScaleFilters();
}

static void calFail(const char *msg) {
  calStep = CAL_FAIL;
  snprintf(calMsg, sizeof(calMsg), "%s", msg);
  if (scaleFound) restoreScaleFilters();
}

static void exitCalMode() {
  calMode = false;
  weightTareG = 0.0f;
  resetWeightState();
  if (scaleFound) {
    restoreScaleFilters();
    miniScaleTare();
  }
}

static void handleCalButtonA() {
  switch (calStep) {
    case CAL_EMPTY:
      if (!scaleFound) {
        calFail("Mini Scale not found");
        break;
      }
      if (!miniScaleReadSampleAvg(&calLiveAdc, &calGramsEmpty)) {
        calFail("空載の読取失敗");
        break;
      }
      calAdcEmpty = (uint32_t)calLiveAdc;
      calStep = CAL_LOAD;
      break;
    case CAL_LOAD: {
      int32_t adcLoad = 0;
      if (!miniScaleReadSampleAvg(&adcLoad, &calGramsLoad)) {
        calFail("載荷の読取失敗");
        break;
      }
      calAdcLoad = (uint32_t)adcLoad;
      const float knownG = CAL_REF_GRAMS;
      const int32_t adcEmptyS = miniScaleAdcU32ToS32(calAdcEmpty);
      const int32_t adcDelta = adcLoad - adcEmptyS;
      const int32_t adcAbs =
          adcDelta < 0 ? -adcDelta : adcDelta;
      const float gramDelta = calGramsLoad - calGramsEmpty;
      const float gramAbs = fabsf(gramDelta);

      float gap = 0.0f;
      if (knownG <= 0.0f) {
        calFail("硬貨設定が不正");
        break;
      }
      if (adcAbs >= CAL_MIN_ADC_DELTA) {
        gap = (float)adcDelta / knownG;
      } else {
        char detail[72];
        snprintf(detail, sizeof(detail), "変化小 dADC=%ld dG=%.1f",
                 (long)adcDelta, gramDelta);
        calFail(detail);
        break;
      }
      {
        const float verifyG = (float)adcDelta / gap;
        if (fabsf(verifyG) < knownG * 0.7f || fabsf(verifyG) > knownG * 1.3f) {
          char detail[72];
          snprintf(detail, sizeof(detail), "載荷%.1fg 想定%.1fg", verifyG,
                   knownG);
          calFail(detail);
          break;
        }
      }
      if (gap == 0.0f || !isfinite(gap)) {
        calFail("GAP計算が不正");
        break;
      }
      if (!miniScaleWriteGap(gap)) {
        calFail("GAP書込失敗");
        break;
      }
      if (!miniScaleGapSane(gap)) {
        calFail("GAP値が不正");
        break;
      }
      calGapWritten = gap;
      scaleGap = gap;
      scaleGapOk = true;
      scaleZeroAdc = adcEmptyS;
      calVerifyG = (float)adcDelta / gap;
      calStep = CAL_DONE;
      break;
    }
    case CAL_DONE: {
      uint32_t adcU = 0;
      float g = 0.0f;
      if (!miniScaleReadRaw(&adcU, &g)) {
        calFail("読取失敗");
        break;
      }
      const int32_t adcS = miniScaleAdcU32ToS32(adcU);
      const int32_t emptyS = miniScaleAdcU32ToS32(calAdcEmpty);
      const int32_t dAbs = adcS - emptyS;
      const int32_t dLoad = miniScaleAdcU32ToS32(calAdcLoad) - emptyS;
      const int32_t rem = dAbs < 0 ? -dAbs : dAbs;
      const int32_t loadAbs = dLoad < 0 ? -dLoad : dLoad;
      if (rem > loadAbs / 4) {
        calFail("硬貨を外してA");
        break;
      }
      scaleZeroAdc = adcS;
      exitCalMode();
      break;
    }
    case CAL_FAIL:
      calStep = CAL_LOAD;  // Aで手順2から再試行
      break;
  }
}

static void renderCal() {
  const uint16_t bg = lgfx::color565(18, 14, 8);
  canvas.fillScreen(bg);
  const int W = canvas.width();
  const int H = canvas.height();
  const uint16_t titleCol = lgfx::color565(255, 210, 80);
  const uint16_t textCol = lgfx::color565(220, 220, 230);
  const uint16_t dimCol = lgfx::color565(140, 140, 155);

  canvas.setFont(&fonts::lgfxJapanGothicP_16);
  canvas.setTextDatum(top_center);
  canvas.setTextColor(titleCol, bg);
  canvas.drawString("CALIBRATION", W / 2, 8);

  canvas.setTextColor(dimCol, bg);
  {
    char refLine[40];
    snprintf(refLine, sizeof(refLine), "10yen x%d (%.1fg)", CAL_10YEN_COUNT,
             CAL_REF_GRAMS);
    canvas.drawString(refLine, W / 2, 26);
  }

  canvas.setTextColor(textCol, bg);
  int y = 48;
  const int lineH = 20;

  switch (calStep) {
    case CAL_EMPTY:
      canvas.drawString("手順1/2: 空載", W / 2, y);
      y += lineH + 6;
      canvas.setTextColor(dimCol, bg);
      canvas.drawString("何も載せない", W / 2, y);
      y += lineH;
      canvas.drawString("Aでゼロ点取得", W / 2, y);
      y += lineH;
      {
        char live[40];
        snprintf(live, sizeof(live), "now %.1fg", calLiveG);
        canvas.drawString(live, W / 2, y);
      }
      break;
    case CAL_LOAD: {
      char loadLine[48];
      snprintf(loadLine, sizeof(loadLine), "Step2: 10yen x%d", CAL_10YEN_COUNT);
      canvas.drawString(loadLine, W / 2, y);
      y += lineH + 6;
      canvas.setTextColor(dimCol, bg);
      canvas.drawString("載せて2秒待ってからA", W / 2, y);
      y += lineH;
      canvas.drawString("(10枚・中央に)", W / 2, y);
      y += lineH;
      {
        char live[56];
        snprintf(live, sizeof(live), "now %.1fg (+%.1f) adc %ld",
                 calLiveG, calLiveG - calGramsEmpty, (long)calLiveAdc);
        canvas.drawString(live, W / 2, y);
      }
      break;
    }
    case CAL_DONE: {
      char result[48];
      snprintf(result, sizeof(result), "target %.1fg / now %.1fg", CAL_REF_GRAMS,
               calVerifyG);
      canvas.drawString("完了", W / 2, y);
      y += lineH + 6;
      canvas.setTextColor(dimCol, bg);
      canvas.drawString(result, W / 2, y);
      y += lineH;
      canvas.drawString("Mini Scalesに保存済み", W / 2, y);
      y += lineH;
      canvas.drawString("硬貨を外してA", W / 2, y);
      break;
    }
    case CAL_FAIL:
      canvas.setTextColor(lgfx::color565(230, 20, 20), bg);
      canvas.drawString("失敗", W / 2, y);
      y += lineH + 6;
      canvas.setTextColor(dimCol, bg);
      canvas.drawString(calMsg[0] ? calMsg : "不明なエラー", W / 2, y);
      break;
  }

  canvas.setTextColor(dimCol, bg);
  canvas.setTextDatum(bottom_left);
  if (calStep == CAL_DONE || calStep == CAL_FAIL) {
    canvas.drawString("A:back", 8, H - 6);
  } else {
    canvas.drawString("A:next", 8, H - 6);
  }
  canvas.setTextDatum(bottom_right);
  canvas.drawString("C:中止", W - 8, H - 6);

  canvas.pushSprite(0, 0);
}

#if ENABLE_LEGACY_EXTRAS
// ---- Datadog アラート ---------------------------------------------------
// ポーリングは別タスク(core0)で実行し、メインループ(core1)を止めない。
// ホットパスはスカラ値(volatile)で読み、表示用の String だけ mutex で保護する。
static constexpr uint16_t COLOR_RED = lgfx::color565(230, 20, 20);
static constexpr int MAX_ALERTS = 6;          // 同時保持するアラートの上限
static volatile bool gAlertActive = false;    // サーバが ALERT 状態か
static volatile long alertSeq = 0;            // サーバの発報シーケンス
static volatile long alertAckedSeq = 0;       // 確認済みシーケンス
static volatile int alertCount = 0;           // 現在のアラート件数
static String alId[MAX_ALERTS];               // mutex 保護: アラートID（クリア用）
static String alMon[MAX_ALERTS];              // mutex 保護: モニター名
static String alPri[MAX_ALERTS];              // mutex 保護: 優先度
static String alLink[MAX_ALERTS];             // mutex 保護: Datadog $LINK
static SemaphoreHandle_t alertMux = nullptr;
static bool hubDismissed = false;             // 調査ハブ画面を閉じたか
static int hubIndex = 0;                       // ハブで表示中のアラート番号

// ---- ミュート（サイレン/チャイムの音をまとめて消す。視覚は残す）--------
static bool gMuted = false;

// ---- Google カレンダー通知（穏やかな先約リマインド）---------------------
static constexpr uint16_t COLOR_CAL = lgfx::color565(40, 120, 230);  // 落ち着いた青
static volatile long remSeq = 0;       // サーバのリマインドシーケンス
static volatile long remAckedSeq = 0;  // 確認済みリマインド
static volatile long remStart = 0;     // 直近リマインドの開始時刻(epoch秒)
static volatile long remEnd = 0;       // 直近リマインドの終了時刻(epoch秒)
static String remTitle;                // mutex 保護: 予定名

// ハブ画面の「CLEAR」ボタン領域（画面座標）。この中をタップ＝表示中の1件を消す。
static constexpr int CLR_X = 16;
static constexpr int CLR_Y = 192;
static constexpr int CLR_W = 96;
static constexpr int CLR_H = 30;

// サイレン鳴動中か（未確認の ALERT がある）
static inline bool sirenActive() {
  return gAlertActive && alertSeq > alertAckedSeq;
}
#endif

// ---- 時刻ソース ---------------------------------------------------------
static void systemTimeFromRtc() {
  if (!M5.Rtc.isEnabled()) return;
  auto dt = M5.Rtc.getDateTime();
  struct tm tm = {};
  tm.tm_year = dt.date.year - 1900;
  tm.tm_mon = dt.date.month - 1;
  tm.tm_mday = dt.date.date;
  tm.tm_hour = dt.time.hours;
  tm.tm_min = dt.time.minutes;
  tm.tm_sec = dt.time.seconds;
  time_t t = mktime(&tm);  // RTC は現地時刻（JST）で保持
  struct timeval tv = {t, 0};
  settimeofday(&tv, nullptr);
}

static void rtcFromSystemTime() {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  m5::rtc_datetime_t dt;
  dt.date.year = tm.tm_year + 1900;
  dt.date.month = tm.tm_mon + 1;
  dt.date.date = tm.tm_mday;
  dt.date.weekDay = tm.tm_wday;
  dt.time.hours = tm.tm_hour;
  dt.time.minutes = tm.tm_min;
  dt.time.seconds = tm.tm_sec;
  M5.Rtc.setDateTime(&dt);
}

static void syncTime() {
  // まずは RTC から大まかな時刻を復元（WiFi が無くても動くように）
  systemTimeFromRtc();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi connecting to %s", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (!wifiOk) {
    Serial.println("WiFi failed, falling back to RTC time.");
    return;
  }
  Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

  configTime(NTP_TZ_OFFSET_SEC, NTP_DST_OFFSET_SEC,
             NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  struct tm tm;
  if (getLocalTime(&tm, 10000)) {
    ntpOk = true;
    rtcFromSystemTime();  // 同期した時刻を RTC に保存
    Serial.printf("NTP synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    Serial.println("NTP sync failed.");
  }
}

// ---- レイアウト計算 -----------------------------------------------------
static void computeLayout() {
  const int W = canvas.width();
  const int H = canvas.height();

  canvas.setFont(&Seg7);
  canvas.setTextSize(1.0f);
  const int basePair = canvas.textWidth("00");   // 2桁ぶんの送り幅
  const int baseGap = (int)(canvas.textWidth("0") * 0.34f);  // コロンの間隔
  const int baseH = canvas.fontHeight();
  const int baseTotal = basePair * 2 + baseGap;

  // HH:MM は横長なので横幅で頭打ちになる。左右余白を 1px まで詰めて均等スケールを最大化。
  // （縦は余るが、黒背景なので中央寄せでOK。非均等な縦伸ばしはしない）
  const int dateArea = 22;
  const float scaleW = (float)(W - 2) / (float)baseTotal;
  const float scaleH = (float)(H - 2) / (float)baseH;
  timeScale = (scaleW < scaleH) ? scaleW : scaleH;
  canvas.setTextSize(timeScale);

  const int pairW = canvas.textWidth("00");
  const int gapW = (int)(canvas.textWidth("0") * 0.34f);
  const int total = pairW * 2 + gapW;
  const int startX = (W - total) / 2;
  hhX = startX;
  mmX = startX + pairW + gapW;
  colonCx = startX + pairW + gapW / 2;
  timeY = (H - dateArea) / 2;  // 日付ぶんを除いた領域の中央

  const int digitH = canvas.fontHeight();
  colonSide = (int)(digitH * 0.15f);  // 四角コロンの一辺
  colonDy = (int)(digitH * 0.20f);    // 中心からの上下オフセット
}

// 未解決アラートがあるとき、時計の右上に点滅する赤いバッジを重ねる。
// タップでハブ（QR）に戻れることを示す痕跡。
#if ENABLE_LEGACY_EXTRAS
static void drawAlertBadge() {
  if (!gAlertActive) return;
  const int n = alertCount;
  char buf[20];
  if (n > 1) snprintf(buf, sizeof(buf), "! ALERT %d", n);
  else snprintf(buf, sizeof(buf), "! ALERT");
  const int w = (n > 1) ? 100 : 78;
  const int h = 22;
  const int x = canvas.width() - w - 6;
  const int y = 6;
  const bool on = ((millis() / 500) % 2) == 0;
  const uint16_t bg = on ? lgfx::color565(230, 20, 20) : lgfx::color565(90, 0, 0);
  canvas.fillRoundRect(x, y, w, h, 5, bg);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_WHITE, bg);
  canvas.setFont(&fonts::Font2);
  canvas.drawString(buf, x + w / 2, y + h / 2);
}
#endif

// 右上のバッテリー残量。アイコン＋%、充電中は稲妻。
// パネルのテイストに合わせて配色を切替（デジタル=ダーク調/緑黄赤、アナログ=白地に黒のミニマル）。
static void drawBatteryOverlay() {
  int level = M5.Power.getBatteryLevel();  // 0-100
  if (level < 0) level = 0;
  if (level > 100) level = 100;
  const bool charging = M5.Power.isCharging();
  const bool analog = (currentPanel == PANEL_ANALOG);

  // 配色：アナログは白地に黒（低下時のみ赤）、デジタルはダーク調＋残量カラー
  const uint16_t col =
      analog ? ((level < 20) ? lgfx::color565(200, 30, 30) : COLOR_BLACK)
             : ((level >= 50)  ? COLOR_GREEN
                : (level >= 20) ? TFT_YELLOW
                                : lgfx::color565(230, 40, 40));
  const uint16_t pbg = analog ? COLOR_WHITE : lgfx::color565(18, 18, 18);
  const uint16_t boltCol = analog ? lgfx::color565(200, 30, 30) : TFT_WHITE;

  const int padW = 92, padH = 26;
  const int px = canvas.width() - padW - 6;
  const int py = 6;
  // アナログは白地そのままに溶け込ませる（ピル背景は描かない）
  if (!analog) canvas.fillRoundRect(px, py, padW, padH, 6, pbg);

  // バッテリー外枠＋端子
  const int bw = 30, bh = 14;
  const int bx = px + 8, by = py + (padH - bh) / 2;
  canvas.drawRoundRect(bx, by, bw, bh, 2, col);
  canvas.fillRect(bx + bw, by + 4, 3, bh - 8, col);

  // 残量バー
  const int innerW = bw - 4;
  const int fillW = innerW * level / 100;
  if (fillW > 0) canvas.fillRect(bx + 2, by + 2, fillW, bh - 4, col);

  // 充電中の稲妻マーク
  if (charging) {
    const int zx = bx + bw / 2, zy = by + bh / 2;
    canvas.fillTriangle(zx - 1, by + 2, zx + 4, zy, zx, zy, boltCol);
    canvas.fillTriangle(zx, zy, zx - 4, zy, zx + 1, by + bh - 2, boltCol);
  }

  // %テキスト
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", level);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(col, pbg);
  canvas.drawString(buf, bx + bw + 8, py + padH / 2);
}

// ミュートアイコン（スピーカー＋音波＋斜線）を小スプライトに不透明で1度だけ描く。
// 背景キーは黒(0)。アイコンは明るいグレーで描く。
#if ENABLE_LEGACY_EXTRAS
static void buildMuteSprite() {
  muteSpr.setColorDepth(16);
  muteSpr.setPsram(true);
  muteSpr.createSprite(MUTE_W, MUTE_H);
  muteSpr.fillScreen(0);  // 0 = 透明キー
  const uint16_t ic = lgfx::color565(220, 220, 220);

  // スピーカー：胴（四角）＋コーン（台形＝三角形2枚）
  muteSpr.fillRect(16, 57, 28, 36, ic);
  muteSpr.fillTriangle(44, 57, 44, 93, 76, 122, ic);
  muteSpr.fillTriangle(44, 57, 76, 122, 76, 28, ic);

  // 音波（右に開く弧）。スプライト上は不透明なので、点を重ねても濁らない。
  const float wcx = 80, wcy = 75;
  const float radii[3] = {32, 52, 72};
  for (int k = 0; k < 3; k++) {
    for (float a = -46; a <= 46; a += 1.5f) {
      const float r = radii[k];
      const float th = a * DEG_TO_RAD;
      muteSpr.fillCircle((int)(wcx + cosf(th) * r),
                         (int)(wcy + sinf(th) * r), 4, ic);
    }
  }

  // 消音の斜線（太め）
  for (int i = -4; i <= 4; i++)
    muteSpr.drawLine(14, 16 + i, 176, 134 + i, ic);

  muteSprReady = true;
}

// ミュート中だけ、アイコンを画面中央へαブレンドで重ねる（背景＝時計が透けて見える）。
static void drawMuteOverlay() {
  if (!gMuted) return;
  if (!muteSprReady) buildMuteSprite();
  const int ox = (canvas.width() - MUTE_W) / 2;
  const int oy = (canvas.height() - MUTE_H) / 2;
  const int A = MUTE_ALPHA, IA = 255 - A;
  for (int y = 0; y < MUTE_H; y++) {
    const int ty = oy + y;
    if (ty < 0 || ty >= canvas.height()) continue;
    for (int x = 0; x < MUTE_W; x++) {
      if (muteSpr.readPixel(x, y) == 0) continue;  // 透明キー
      const int tx = ox + x;
      if (tx < 0 || tx >= canvas.width()) continue;
      const uint16_t bg = canvas.readPixel(tx, ty);
      const uint8_t br = ((bg >> 11) & 0x1F) * 255 / 31;
      const uint8_t bgc = ((bg >> 5) & 0x3F) * 255 / 63;
      const uint8_t bb = (bg & 0x1F) * 255 / 31;
      const uint8_t R = (220 * A + br * IA) / 255;
      const uint8_t G = (220 * A + bgc * IA) / 255;
      const uint8_t B = (220 * A + bb * IA) / 255;
      canvas.drawPixel(tx, ty, lgfx::color565(R, G, B));
    }
  }
}
#endif

// ---- デジタル時計パネル -------------------------------------------------
static void renderDigital(const struct tm &tm, bool colonOn) {
  canvas.fillScreen(COLOR_BG);

  char hh[3], mm[3];
  snprintf(hh, sizeof(hh), "%02d", tm.tm_hour);
  snprintf(mm, sizeof(mm), "%02d", tm.tm_min);

  // 大きな HH:MM（DSEG7 7セグ・緑）
  canvas.setFont(&Seg7);
  canvas.setTextSize(timeScale);
  canvas.setTextColor(COLOR_GREEN, COLOR_BG);
  canvas.setTextDatum(middle_left);
  canvas.drawString(hh, hhX, timeY);
  canvas.drawString(mm, mmX, timeY);

  // 四角いコロン（500ms 周期で点滅）
  if (colonOn) {
    const int s = colonSide;
    canvas.fillRect(colonCx - s / 2, timeY - colonDy - s / 2, s, s, COLOR_GREEN);
    canvas.fillRect(colonCx - s / 2, timeY + colonDy - s / 2, s, s, COLOR_GREEN);
  }

  // 日付（ターミナル風・小フォント）
  static const char *wd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  char date[24];
  snprintf(date, sizeof(date), "%04d-%02d-%02d %s",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
  canvas.setFont(&fonts::Font2);
  canvas.setTextSize(1.0f);
  canvas.setTextColor(COLOR_DIM, COLOR_BG);
  canvas.setTextDatum(bottom_left);
  canvas.drawString(date, 8, canvas.height() - 6);

  // 接続ステータス（左上の小さなドット）
  uint16_t dot = ntpOk ? COLOR_GREEN : (wifiOk ? TFT_YELLOW : COLOR_DIM);
  canvas.fillCircle(8, 10, 4, dot);

  drawBatteryOverlay();
  canvas.pushSprite(0, 0);
}

// ---- アナログ時計パネル（無印 駅の時計）---------------------------------
// 画面が横長なので文字盤も楕円（rx > ry）にして「縦に潰れた」見た目を踏襲する。
// 左右5%・上下3%の余白を確保した楕円半径（数字グリフ端が余白に収まる）
static constexpr float CLOCK_RX = 130.0f;  // 横半径
static constexpr float CLOCK_RY = 96.0f;   // 縦半径

// 各時刻マーカーの正規化座標（参考画像 tools/analyze_clock.py の実測値）。
// nx は横半径、ny は縦半径に対する比率。index 0 は未使用、1..12 が各時刻。
static const struct { float nx, ny; } MARKERS[13] = {
    {0.000f, 0.000f},                       // 0: 未使用
    {0.387f, -0.961f}, {0.725f, -0.599f},   // 1, 2
    {1.000f, -0.013f}, {0.726f, 0.578f},    // 3, 4
    {0.388f, 0.937f},  {0.000f, 1.000f},    // 5, 6
    {-0.397f, 0.935f}, {-0.737f, 0.573f},   // 7, 8
    {-1.000f, -0.051f}, {-0.736f, -0.605f}, // 9, 10
    {-0.398f, -0.964f}, {0.000f, -1.000f},  // 11, 12
};

// 端を四角くした太線（角丸を避けるため三角形2枚で描く）
static void thickSeg(float x0, float y0, float x1, float y1, float half,
                     uint16_t col) {
  const float dx = x1 - x0, dy = y1 - y0;
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;
  const float px = -dy / len * half, py = dx / len * half;
  canvas.fillTriangle(x0 + px, y0 + py, x0 - px, y0 - py, x1 + px, y1 + py, col);
  canvas.fillTriangle(x0 - px, y0 - py, x1 + px, y1 + py, x1 - px, y1 - py, col);
}

// 先細りの針。先端は楕円上、根元側は中心を越えて tail だけ伸ばす（無印は尾が長め）
static void drawHand(int cx, int cy, float deg, float lenFrac, float tailFrac,
                     float baseHalf, float tipHalf, uint16_t col) {
  const float th = deg * DEG_TO_RAD;
  const float sx = sinf(th), sy = -cosf(th);
  const float tx = cx + CLOCK_RX * lenFrac * sx;
  const float ty = cy + CLOCK_RY * lenFrac * sy;
  const float bx = cx - CLOCK_RX * tailFrac * sx;
  const float by = cy - CLOCK_RY * tailFrac * sy;
  // 画面空間での針方向の垂直ベクトル
  float vx = tx - bx, vy = ty - by;
  const float vl = sqrtf(vx * vx + vy * vy);
  if (vl < 0.001f) return;
  const float px = -vy / vl, py = vx / vl;
  canvas.fillTriangle(tx + px * tipHalf, ty + py * tipHalf,
                      tx - px * tipHalf, ty - py * tipHalf,
                      bx + px * baseHalf, by + py * baseHalf, col);
  canvas.fillTriangle(tx - px * tipHalf, ty - py * tipHalf,
                      bx + px * baseHalf, by + py * baseHalf,
                      bx - px * baseHalf, by - py * baseHalf, col);
}

static void renderAnalog(const struct tm &tm) {
  const int cx = canvas.width() / 2;
  const int cy = canvas.height() / 2;

  canvas.fillScreen(COLOR_WHITE);

  // 時刻位置のマーカー：12/3/6/9 は数字、その他は四角い短い目盛り
  // 位置は実測の正規化座標をそのまま採用。数字は縁、目盛りは縁に沿う放射状の短線。
  canvas.setTextColor(COLOR_BLACK, COLOR_WHITE);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&MujiNum);
  for (int h = 1; h <= 12; h++) {
    const float mx = cx + CLOCK_RX * MARKERS[h].nx;
    const float my = cy + CLOCK_RY * MARKERS[h].ny;
    if (h % 3 == 0) {
      const char *num = (h == 12) ? "12" : (h == 3) ? "3" : (h == 6) ? "6" : "9";
      canvas.drawString(num, mx, my);
    } else {
      // マーカー位置を中心に、中心から外向き（放射方向）の短い四角目盛り
      float dx = mx - cx, dy = my - cy;
      const float dl = sqrtf(dx * dx + dy * dy);
      dx /= dl;
      dy /= dl;
      const float halfLen = 13.0f;
      thickSeg(mx - dx * halfLen, my - dy * halfLen,
               mx + dx * halfLen, my + dy * halfLen, 2.5f, COLOR_BLACK);
    }
  }

  // 針（秒針なし。分針は秒で微小に進める）
  const float hourDeg = ((tm.tm_hour % 12) + tm.tm_min / 60.0f) * 30.0f;
  const float minDeg = (tm.tm_min + tm.tm_sec / 60.0f) * 6.0f;
  // 時針：短め・太め、分針：長め・細め（目盛りの中ほどまで届く）。根元の尾は長め。
  drawHand(cx, cy, hourDeg, 0.58f, 0.26f, 7.0f, 3.5f, COLOR_BLACK);
  drawHand(cx, cy, minDeg, 0.92f, 0.26f, 5.0f, 2.5f, COLOR_BLACK);

  // 中心のハブ
  canvas.fillCircle(cx, cy, 7, COLOR_BLACK);

  drawBatteryOverlay();
  canvas.pushSprite(0, 0);
}

// ---- アラート通信 -------------------------------------------------------
#if ENABLE_LEGACY_EXTRAS
static void pollAlert() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();  // 簡易: 証明書検証なし
  HTTPClient https;
  String url = String(ALERT_WORKER_URL) + "/status";
  if (!https.begin(client, url)) return;
  https.addHeader("Authorization", String("Bearer ") + ALERT_DEVICE_TOKEN);
  https.setTimeout(8000);
  int code = https.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, https.getString()) == DeserializationError::Ok) {
      const long seq = doc["seq"] | 0L;
      const long serverAck = doc["ackSeq"] | 0L;
      JsonArray items = doc["items"].as<JsonArray>();
      // 表示用 String は mutex 保護下で更新
      if (alertMux) xSemaphoreTake(alertMux, portMAX_DELAY);
      int n = 0;
      for (JsonObject it : items) {
        if (n >= MAX_ALERTS) break;
        alId[n] = String((const char *)(it["id"] | ""));
        alMon[n] = String((const char *)(it["monitor"] | ""));
        alPri[n] = String((const char *)(it["priority"] | ""));
        alLink[n] = String((const char *)(it["link"] | ""));
        n++;
      }
      alertCount = n;
      // カレンダー通知：配列の末尾＝最新の予定を採用
      JsonArray rems = doc["reminders"].as<JsonArray>();
      if (rems.size() > 0) {
        JsonObject r = rems[rems.size() - 1];
        remTitle = String((const char *)(r["title"] | ""));
        remStart = (long)((r["start"] | 0LL) / 1000LL);  // ms -> 秒（64bitで割ってから縮める）
        remEnd = (long)((r["end"] | 0LL) / 1000LL);
      }
      if (alertMux) xSemaphoreGive(alertMux);
      // スカラはそのまま反映
      alertSeq = seq;
      if (serverAck > alertAckedSeq) alertAckedSeq = serverAck;
      gAlertActive = (n > 0);
      remSeq = doc["remSeq"] | 0L;
      const long serverRemAck = doc["remAckSeq"] | 0L;
      if (serverRemAck > remAckedSeq) remAckedSeq = serverRemAck;
    }
  } else {
    Serial.printf("pollAlert HTTP %d\n", code);
  }
  https.end();
}

// ポーリング専用タスク（core0）。メインループ(core1)を一切ブロックしない。
static void alertTask(void *pv) {
  for (;;) {
    pollAlert();
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

static void ackAlert() {
  const long seq = alertSeq;
  alertAckedSeq = seq;  // まずローカルで即ミュート
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String(ALERT_WORKER_URL) + "/ack";
  if (!https.begin(client, url)) return;
  https.addHeader("Authorization", String("Bearer ") + ALERT_DEVICE_TOKEN);
  https.addHeader("Content-Type", "application/json");
  https.setTimeout(8000);
  String body = String("{\"seq\":") + seq + "}";
  int code = https.POST(body);
  Serial.printf("ackAlert seq=%ld HTTP %d\n", seq, code);
  https.end();
}

// カレンダー通知を確認済みにする（Worker /ack に remSeq を送る）。
static void ackReminder() {
  const long seq = remSeq;
  remAckedSeq = seq;  // まずローカルで即消音/非表示
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String(ALERT_WORKER_URL) + "/ack";
  if (!https.begin(client, url)) return;
  https.addHeader("Authorization", String("Bearer ") + ALERT_DEVICE_TOKEN);
  https.addHeader("Content-Type", "application/json");
  https.setTimeout(8000);
  String body = String("{\"remSeq\":") + seq + "}";
  int code = https.POST(body);
  Serial.printf("ackReminder remSeq=%ld HTTP %d\n", seq, code);
  https.end();
}

// アラートを1件だけ手動クリア（Worker /clear）。成功したらローカルからも即除去。
static void clearAlert(const String &id) {
  if (id.length() == 0 || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String(ALERT_WORKER_URL) + "/clear";
  if (!https.begin(client, url)) return;
  https.addHeader("Authorization", String("Bearer ") + ALERT_DEVICE_TOKEN);
  https.addHeader("Content-Type", "application/json");
  https.setTimeout(8000);
  String body = String("{\"id\":\"") + id + "\"}";
  int code = https.POST(body);
  Serial.printf("clearAlert id=%s HTTP %d\n", id.c_str(), code);
  https.end();
  if (code != 200) return;
  // ローカル配列からも詰めて除去（次のポーリングを待たず即反映）
  if (alertMux) xSemaphoreTake(alertMux, portMAX_DELAY);
  int n = alertCount, w = 0;
  for (int i = 0; i < n; i++) {
    if (alId[i] == id) continue;
    if (w != i) {
      alId[w] = alId[i];
      alMon[w] = alMon[i];
      alPri[w] = alPri[i];
      alLink[w] = alLink[i];
    }
    w++;
  }
  alertCount = w;
  if (hubIndex >= w) hubIndex = (w > 0) ? w - 1 : 0;
  if (alertMux) xSemaphoreGive(alertMux);
  gAlertActive = (alertCount > 0);
}

// サイレン音：2音を交互に鳴らす（呼び出すたびに継続更新）
static void sirenSound() {
  static uint32_t last = 0;
  static bool hi = false;
  uint32_t ms = millis();
  if (ms - last >= 350) {
    last = ms;
    hi = !hi;
    M5.Speaker.tone(hi ? 990 : 660, 400);
  }
}

// 中心から伸びる光のビーム（くさび形）を1本描く
static void drawBeam(int cx, int cy, float angDeg, float halfDeg, int len, uint16_t color) {
  const float a1 = (angDeg - halfDeg) * DEG_TO_RAD;
  const float a2 = (angDeg + halfDeg) * DEG_TO_RAD;
  const int x1 = cx + cosf(a1) * len;
  const int y1 = cy + sinf(a1) * len;
  const int x2 = cx + cosf(a2) * len;
  const int y2 = cy + sinf(a2) * len;
  canvas.fillTriangle(cx, cy, x1, y1, x2, y2, color);
}

// 0..1 の係数で 2 色を線形補間
static uint16_t lerpRGB(uint8_t r0, uint8_t g0, uint8_t b0,
                        uint8_t r1, uint8_t g1, uint8_t b1, float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  return lgfx::color565((uint8_t)(r0 + (r1 - r0) * t),
                        (uint8_t)(g0 + (g1 - g0) * t),
                        (uint8_t)(b0 + (b1 - b0) * t));
}

// 回転灯（パトランプ）を「横から見た」サイレン画面。
// 黒い土台＋赤いドーム＋内部で左右に回る発光体＋ドームから左右へ振れる光ビーム。
static void renderAlert() {
  const int W = canvas.width();
  const int H = canvas.height();
  const uint32_t ms = millis();

  // 回転角（1周 ≈ 1.05 秒）
  const float th = ms * 0.006f;
  const float s = sinf(th);  // -1(右) .. +1(左) 内部発光体の横位置
  const float c = cosf(th);  // +1 で正面向き → ドーム全体が明るく光る
  const float face = c * 0.5f + 0.5f;          // 0..1 正面度
  const float leftI = s > 0 ? s : 0;           // 左へ抜ける光の強さ
  const float rightI = s < 0 ? -s : 0;         // 右へ抜ける光の強さ

  canvas.fillScreen(lgfx::color565(12, 0, 0));

  const int cx = W / 2;

  // ドーム頂点付近を起点に、左右上方へ振れる光ビーム（部屋を照らす光）
  const int apexX = cx;
  const int apexY = 74;
  if (leftI > 0.03f)
    drawBeam(apexX, apexY, 236, 17, 340,
             lerpRGB(12, 0, 0, 255, 120, 30, leftI));
  if (rightI > 0.03f)
    drawBeam(apexX, apexY, 304, 17, 340,
             lerpRGB(12, 0, 0, 255, 120, 30, rightI));

  // 赤いドーム（円筒の胴 ＋ 上半円のキャップ）。正面向きで明るく発光。
  const int rw = 52;
  const int bodyTop = 100;
  const int bodyBot = 184;
  const uint16_t domeC = lerpRGB(120, 12, 12, 235, 45, 30, face);
  canvas.fillRect(cx - rw, bodyTop, rw * 2, bodyBot - bodyTop, domeC);
  canvas.fillEllipse(cx, bodyTop, rw, 46, domeC);

  // ガラスの縦リブ（暗い縦線）でレンズらしさ
  for (int i = -2; i <= 2; i++) {
    const int lx = cx + i * 21;
    canvas.drawFastVLine(lx, bodyTop - 10, bodyBot - bodyTop + 10,
                         lerpRGB(70, 6, 6, 150, 25, 18, face));
  }

  // 内部の発光体（左右に回りながら移動）。外オレンジ → 内白。
  const int hx = cx + (int)(s * 26.0f);
  const int hy = 138;
  canvas.fillCircle(hx, hy, 26, lerpRGB(170, 35, 8, 255, 150, 50, face));
  canvas.fillCircle(hx, hy, 16, lgfx::color565(255, 225, 150));
  canvas.fillCircle(hx, hy, 8, TFT_WHITE);

  // ドーム左上のハイライト（ガラスの艶）
  canvas.fillEllipse(cx - 26, bodyTop - 6, 8, 16, lgfx::color565(255, 170, 160));

  // 黒い土台
  canvas.fillRoundRect(cx - 60, bodyBot - 2, 120, 30, 7, lgfx::color565(20, 20, 22));
  canvas.fillRoundRect(cx - 60, bodyBot - 2, 120, 6, 3, lgfx::color565(48, 48, 52));

  // テキスト（最前面）
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold24pt7b);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString("ALERT", cx, 24);

  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(gMuted ? "TAP TO ACK (MUTED)" : "TAP TO ACKNOWLEDGE", cx, H - 12);

  drawMuteOverlay();
  canvas.pushSprite(0, 0);
}

// ACK 後の「調査ハブ」画面：モニター名＋QR（Datadog ディープリンク）
static void renderHub() {
  const int W = canvas.width();
  const int H = canvas.height();

  // ポーリングタスクと競合しないよう、表示中アラートをスナップショット
  String monitor, priority, link;
  int count, idx;
  if (alertMux) xSemaphoreTake(alertMux, portMAX_DELAY);
  count = alertCount;
  if (hubIndex >= count) hubIndex = 0;
  idx = hubIndex;
  if (count > 0) {
    monitor = alMon[idx];
    priority = alPri[idx];
    link = alLink[idx];
  }
  if (alertMux) xSemaphoreGive(alertMux);

  canvas.fillScreen(COLOR_BG);

  // 上部：優先度バッジ＋モニター名
  canvas.setTextDatum(top_left);
  if (priority.length()) {
    uint16_t pc = COLOR_RED;
    if (priority == "P3" || priority == "P4" || priority == "P5")
      pc = lgfx::color565(220, 160, 0);
    canvas.fillRoundRect(8, 8, 48, 24, 5, pc);
    canvas.setTextColor(COLOR_BG, pc);
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    canvas.drawString(priority.c_str(), 8 + 24, 8 + 12);
  }

  // 件数インジケータ（複数あるとき「2/3」）
  if (count > 1) {
    char ind[12];
    snprintf(ind, sizeof(ind), "%d/%d", idx + 1, count);
    canvas.setTextDatum(top_right);
    canvas.setTextColor(lgfx::color565(255, 120, 120), COLOR_BG);
    canvas.setFont(&fonts::Font2);
    canvas.drawString(ind, W - 12, 40);
  }

  canvas.setTextDatum(top_left);
  canvas.setTextColor(COLOR_WHITE, COLOR_BG);
  canvas.setFont(&fonts::Font2);
  // モニター名（W に収まる範囲で 2 行まで雑に折り返し）
  canvas.setTextWrap(true);
  canvas.setCursor(64, 12);
  canvas.print(monitor.length() ? monitor : String("Datadog monitor"));
  canvas.setTextWrap(false);

  // QR（右寄せ）。version=1 指定で内部が自動拡張する
  const int qr = 150;
  const int qx = W - qr - 12;
  const int qy = H - qr - 10;
  if (link.length()) {
    canvas.qrcode(link.c_str(), qx, qy, qr, 1, true);
  } else {
    canvas.setTextColor(COLOR_WHITE, COLOR_BG);
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font2);
    canvas.drawString("(no link)", qx + qr / 2, qy + qr / 2);
  }

  // 左側：誘導文
  canvas.setTextDatum(top_left);
  canvas.setTextColor(lgfx::color565(120, 220, 255), COLOR_BG);
  canvas.setFont(&fonts::Font2);
  canvas.drawString("SCAN ->", 16, 70);
  canvas.setTextColor(COLOR_WHITE, COLOR_BG);
  canvas.drawString("Datadog", 16, 92);
  canvas.drawString("app", 16, 112);

  // 操作ヒント（QR側タップで次/戻る）
  canvas.setTextColor(lgfx::color565(150, 150, 150), COLOR_BG);
  canvas.drawString(count > 1 ? "tap: next" : "tap: back", 16, 140);

  // CLEAR ボタン（この1件を手動で消す）
  canvas.drawRoundRect(CLR_X, CLR_Y, CLR_W, CLR_H, 6, COLOR_RED);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(lgfx::color565(255, 90, 90), COLOR_BG);
  canvas.setFont(&fonts::Font2);
  canvas.drawString("CLEAR", CLR_X + CLR_W / 2, CLR_Y + CLR_H / 2);

  drawMuteOverlay();
  canvas.pushSprite(0, 0);
}

// 穏やかなチャイム（上行する2音）。ミュート中は鳴らさない。
static void calmChime() {
  if (gMuted) return;
  M5.Speaker.tone(880, 160);
  delay(170);
  M5.Speaker.tone(1175, 220);
  delay(60);
  M5.Speaker.stop();
}

static bool isAsciiStr(const String &s) {
  for (size_t i = 0; i < s.length(); i++)
    if ((uint8_t)s[i] >= 0x80) return false;
  return true;
}

// 現在のフォントで maxW に収まるよう text を行分割（UTF-8対応）。
// ASCIIは直前の半角スペースで折り返し、日本語は任意位置で折り返す。
// 返り値は行数。out[] に各行を格納（maxLines まで）。
static int wrapTitle(const String &text, int maxW, String out[], int maxLines) {
  int n = 0;
  String cur = "";
  int i = 0;
  const int len = text.length();
  while (i < len) {
    int clen = 1;  // UTF-8 のバイト数
    const uint8_t c = (uint8_t)text[i];
    if (c >= 0xF0) clen = 4;
    else if (c >= 0xE0) clen = 3;
    else if (c >= 0xC0) clen = 2;
    const String ch = text.substring(i, i + clen);
    const String trial = cur + ch;
    if (canvas.textWidth(trial.c_str()) > maxW && cur.length() > 0) {
      const int sp = (c < 0x80) ? cur.lastIndexOf(' ') : -1;
      if (sp > 0) {  // ASCII は単語境界（直前の空白）で折る
        if (n < maxLines) out[n++] = cur.substring(0, sp);
        cur = cur.substring(sp + 1) + ch;
      } else {
        if (n < maxLines) out[n++] = cur;
        cur = ch;
      }
      if (n >= maxLines) {
        cur = "";
        break;
      }
    } else {
      cur = trial;
    }
    i += clen;
  }
  if (cur.length() > 0 && n < maxLines) out[n++] = cur;
  return n;
}

// カレンダー通知画面：落ち着いた青に、時刻レンジ＋予定名を大きく全画面表示。
static void renderReminder() {
  const int W = canvas.width();
  const int H = canvas.height();

  String title;
  long start, end;
  if (alertMux) xSemaphoreTake(alertMux, portMAX_DELAY);
  title = remTitle;
  start = remStart;
  end = remEnd;
  if (alertMux) xSemaphoreGive(alertMux);
  if (title.length() == 0) title = "(no title)";

  canvas.fillScreen(COLOR_CAL);

  // 時刻レンジ「HH:MM - HH:MM」（上部・可愛い筆記体）
  char range[24] = "";
  if (start > 0) {
    time_t ts = (time_t)start;
    struct tm a;
    localtime_r(&ts, &a);
    if (end > start) {
      time_t te = (time_t)end;
      struct tm b;
      localtime_r(&te, &b);
      snprintf(range, sizeof(range), "%02d:%02d - %02d:%02d",
               a.tm_hour, a.tm_min, b.tm_hour, b.tm_min);
    } else {
      snprintf(range, sizeof(range), "%02d:%02d", a.tm_hour, a.tm_min);
    }
  }
  canvas.setTextDatum(top_center);
  canvas.setTextColor(lgfx::color565(225, 240, 255), COLOR_CAL);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextSize(1.0f);
  canvas.drawString(range, W / 2, 10);

  // 予定名（全画面・大きく・ポップに）。
  // 方針: まず大きいフォントのまま折り返し、縦に収まらなければフォントを1段縮小。
  // 英語＝極太サンセリフ、日本語＝ゴシック＋簡易ボールド。
  canvas.setTextColor(TFT_WHITE, COLOR_CAL);
  const bool jp = !isAsciiStr(title);
  const int maxW = W - 24;
  const int titleTop = 52;        // 時刻レンジの下
  const int titleBottom = H - 34;  // カウントダウンの上
  const int availH = titleBottom - titleTop;

  // 大きい順の候補フォント
  const lgfx::IFont *cand[5];
  int nCand = 0;
  if (jp) {
    cand[nCand++] = &fonts::lgfxJapanGothicP_32;
    cand[nCand++] = &fonts::lgfxJapanGothicP_28;
    cand[nCand++] = &fonts::lgfxJapanGothicP_24;
    cand[nCand++] = &fonts::lgfxJapanGothicP_20;
    cand[nCand++] = &fonts::lgfxJapanGothicP_16;
  } else {
    cand[nCand++] = &fonts::FreeSansBold24pt7b;
    cand[nCand++] = &fonts::FreeSansBold18pt7b;
    cand[nCand++] = &fonts::FreeSansBold12pt7b;
    cand[nCand++] = &fonts::FreeSansBold9pt7b;
  }
  canvas.setTextSize(1.0f);

  // 大きい順に「折り返して縦に収まる」最大フォントを採用
  String lines[6];
  int nLines = 0;
  int chosen = nCand - 1;
  for (int i = 0; i < nCand; i++) {
    canvas.setFont(cand[i]);
    String tmp[6];
    const int ln = wrapTitle(title, maxW, tmp, 6);
    const int lineH = canvas.fontHeight();
    const int gap = lineH / 6;
    const int blockH = ln * lineH + (ln - 1) * gap;
    if (blockH <= availH || i == nCand - 1) {  // 収まる or 最小フォント
      chosen = i;
      nLines = ln;
      for (int k = 0; k < ln; k++) lines[k] = tmp[k];
      break;
    }
  }

  // 縦中央に複数行を描画
  canvas.setFont(cand[chosen]);
  const int lineH = canvas.fontHeight();
  const int gap = lineH / 6;
  const int blockH = nLines * lineH + (nLines - 1) * gap;
  int y = titleTop + (availH - blockH) / 2;
  if (y < titleTop) y = titleTop;
  canvas.setTextDatum(top_center);
  for (int k = 0; k < nLines; k++) {
    canvas.drawString(lines[k].c_str(), W / 2, y);
    if (jp) canvas.drawString(lines[k].c_str(), W / 2 + 1, y);  // 簡易ボールド
    y += lineH + gap;
  }

  // カウントダウン＋操作ヒント（下部）
  canvas.setTextSize(1.0f);
  canvas.setTextDatum(bottom_center);
  canvas.setTextColor(lgfx::color565(205, 228, 255), COLOR_CAL);
  canvas.setFont(&fonts::lgfxJapanGothicP_16);
  if (start > 0) {
    long mins = (start - time(nullptr) + 59) / 60;
    if (mins < 0) mins = 0;
    char in[48];
    snprintf(in, sizeof(in), "まもなく開始 (あと%ld分)", mins);
    canvas.drawString(in, W / 2, H - 24);
  }
  canvas.drawString("タップで閉じる", W / 2, H - 6);

  drawMuteOverlay();
  canvas.pushSprite(0, 0);
}

// 「持ち上げたら怒る」用：揺れる怒り顔＋セリフ。
static void renderAngry(uint32_t ms) {
  const int W = canvas.width();
  const int H = canvas.height();
  // プルプル揺れ
  const int sx = (int)(sinf(ms * 0.06f) * 5);
  const int sy = (int)(cosf(ms * 0.05f) * 4);

  canvas.fillScreen(lgfx::color565(150, 0, 0));

  const int cx = W / 2 + sx;
  const int cy = H / 2 - 20 + sy;

  // 目（白目＋黒目）
  const int eyeDx = 60, eyeY = cy - 6, eyeR = 26;
  canvas.fillCircle(cx - eyeDx, eyeY, eyeR, TFT_WHITE);
  canvas.fillCircle(cx + eyeDx, eyeY, eyeR, TFT_WHITE);
  canvas.fillCircle(cx - eyeDx + 4, eyeY + 4, 11, TFT_BLACK);
  canvas.fillCircle(cx + eyeDx - 4, eyeY + 4, 11, TFT_BLACK);

  // 怒り眉（内側が下がる太い斜め線）
  for (int i = -3; i <= 3; i++) {
    canvas.drawLine(cx - eyeDx - 26, eyeY - 34 + i, cx - eyeDx + 22, eyeY - 14 + i,
                    lgfx::color565(20, 0, 0));
    canvas.drawLine(cx + eyeDx + 26, eyeY - 34 + i, cx + eyeDx - 22, eyeY - 14 + i,
                    lgfx::color565(20, 0, 0));
  }

  // 口（への字・開き）
  canvas.fillTriangle(cx - 46, cy + 60, cx + 46, cy + 60, cx, cy + 30,
                      lgfx::color565(20, 0, 0));

  drawMuteOverlay();
  canvas.pushSprite(0, 0);
}
#endif

// ---- 重量モード画面 -----------------------------------------------------
static void renderWeight() {
  AgavWeightView view = {
      .scaleFound = scaleFound,
      .scaleGapOk = scaleGapOk,
      .manualTareActive = manualTareActive,
      .liveWeightG = weightDisplayG,
      .weightHint = weightHint,
  };
  agavRenderWeightScreen(canvas, view, millis());
  canvas.pushSprite(0, 0);
}

static void startScaleSession() {
  initScaleWire();
  scaleFound = miniScaleProbe();
  scaleFiltersReady = false;
  scaleSessionReady = false;
  weightTareG = 0.0f;
  resetWeightState();
  lastWeightRead = 0;
  lastWeightLog = 0;
  if (scaleFound) {
    initScaleFilters();
    beginScaleZero(false, true);
  } else {
    snprintf(weightHint, sizeof(weightHint), "Scale not found");
  }
}

static void enterWeightMode(uint32_t nowMs) {
  if (weightMode) return;
  weightMode = true;
  manualTareActive = false;
  weightLastActivityMs = nowMs;
  agavOnWeightModeEnter();
  agavStartPlantPreload();

  resetWeightState();
  if (!scaleSessionReady) {
    startScaleSession();
  } else {
    pollWeight(nowMs);
  }
  Serial.printf("weight mode -> 1 (scale %s, plants %d)\n",
                scaleFound ? "OK" : "NOT FOUND", agavPlantCount());
}

static void exitWeightMode() {
  if (!weightMode) return;
  weightMode = false;
  scaleZeroPhase = SCALE_ZERO_IDLE;
  manualTareActive = false;
  agavOnWeightModeExit();
  calMode = false;
  Serial.println("weight mode -> 0");
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.BtnB.setHoldThresh(800);  // 長押しでキャリブレーション画面へ

  M5.Display.setRotation(1);
  M5.Display.setBrightness(110);
  M5.Display.fillScreen(COLOR_BG);

  Serial.begin(115200);
  Serial.println("m5deck: clock panel boot");

  canvas.setColorDepth(16);
  canvas.setPsram(true);
  canvas.createSprite(M5.Display.width(), M5.Display.height());

  computeLayout();
  agavNetworkInit();
  syncTime();
  agavStartPlantPreload();

#if ENABLE_LEGACY_EXTRAS
  // ポーリングは別タスク(core0)で常時実行。メインループ(core1)は止めない。
  alertMux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(alertTask, "alert", 16384, nullptr, 1, nullptr, 0);
#endif
}

void loop() {
  M5.update();

  static uint32_t lastDraw = 0;
  static bool colonOn = true;
  static uint32_t lastReSync = 0;
  static bool forceDraw = true;

  const uint32_t ms = millis();

#if ENABLE_LEGACY_EXTRAS
  // 新しい発報 or 復旧でハブの「閉じた」状態をリセット（状態はタスクが更新）
  static long prevSeq = 0;
  static bool prevActive = false;
  const long seqNow = alertSeq;
  const bool activeNow = gAlertActive;
  if (seqNow != prevSeq) {
    prevSeq = seqNow;
    hubDismissed = false;
    hubIndex = 0;
  }
  if (!activeNow) hubDismissed = false;
  if (activeNow != prevActive) {  // 状態が変わったら即再描画
    prevActive = activeNow;
    forceDraw = true;
  }
#endif

  // 物理 C: 時計切替。重量モードでは一覧更新、キャリブ中はキャンセル。
  if (M5.BtnC.wasPressed()) {
    if (calMode) {
      exitCalMode();
    } else if (weightMode) {
      if (!agavIsSelectMode() && !agavIsSentMode()) {
        agavRefreshPlants();
      }
      weightLastActivityMs = ms;
      forceDraw = true;
    } else {
      currentPanel = (currentPanel + 1) % PANEL_COUNT;
      Serial.printf("panel -> %d\n", currentPanel);
    }
    forceDraw = true;
  }

  // 物理 B: 重量モードは短押し=タレ/送信、長押し(0.8s)=キャリブ。
  if (weightMode && !calMode) {
    if (agavIsSelectMode()) {
      if (M5.BtnB.wasClicked()) {
        agavConfirmSend();
        weightLastActivityMs = ms;
        forceDraw = true;
      }
    } else if (!agavIsSentMode() && M5.BtnB.wasHold()) {
      enterCalMode();
      weightLastActivityMs = ms;
      forceDraw = true;
    } else if (!agavIsSentMode() && M5.BtnB.wasClicked()) {
      beginScaleZero(true, false);
      weightLastActivityMs = ms;
      forceDraw = true;
    }
  }

  // 物理 A: 重量モードを終了。選択中は一度キャンセル。
  if (M5.BtnA.wasPressed()) {
    if (calMode) {
      handleCalButtonA();
      weightLastActivityMs = ms;
      forceDraw = true;
    } else if (weightMode && agavIsSelectMode()) {
      agavCancelSelect();
      weightLastActivityMs = ms;
      forceDraw = true;
    } else if (weightMode) {
      exitWeightMode();
      forceDraw = true;
    }
  }

  // タップ検出（座標も保持）
  bool tapped = false;
  int tapX = 0, tapY = 0;
  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail();
    // 画面内(y<240)のみ「画面タップ」扱い。画面下の物理ボタン帯(A/B/C)は除外。
    if (t.wasPressed() && t.y < canvas.height()) {
      tapped = true;
      tapX = t.x;
      tapY = t.y;
    }
  }

#if ENABLE_LEGACY_EXTRAS
  // ---- アラート層 ① サイレン（未確認の ALERT）：時計より優先 ----
  // 5分鳴り続けたら不在とみなして自動ACK（消音して時計＋バッジへ）。
  static uint32_t sirenStartMs = 0;
  static long sirenStartSeq = -1;
  constexpr uint32_t SIREN_TIMEOUT_MS = 5UL * 60UL * 1000UL;
  if (sirenActive()) {
    if (sirenStartSeq != seqNow) {  // 新しい発報でタイマーをリセット
      sirenStartSeq = seqNow;
      sirenStartMs = ms;
    }
    if (tapped) {
      ackAlert();           // 確認 → 即ミュート → 調査ハブへ
      M5.Speaker.stop();
      tapped = false;       // このタップはハブ閉じ/切替に使わない
      forceDraw = true;
    } else if (ms - sirenStartMs >= SIREN_TIMEOUT_MS) {
      ackAlert();           // 5分無応答 → 自動ACK（不在とみなす）
      M5.Speaker.stop();
      hubDismissed = true;  // 調査ハブは開かず時計＋バッジへ
      forceDraw = true;
      Serial.println("auto-ACK: siren timed out (absent)");
    } else {
      if (!gMuted) sirenSound();
      else M5.Speaker.stop();
      renderAlert();
      delay(10);
      return;
    }
  }

  // サイレンが「鳴動→終了」した瞬間だけ停止する。
  // （毎ループ無条件に stop すると、怒りボイス等の単発再生を即切ってしまう）
  static bool sirenWas = false;
  const bool sirenNow = sirenActive();
  if (sirenWas && !sirenNow) M5.Speaker.stop();
  sirenWas = sirenNow;

  // ---- アラート層 ② 調査ハブ（確認済みだが ALERT 継続中）----
  // CLEAR ボタン内タップ＝表示中の1件を消す。それ以外のタップ＝次のQRへ（最後で時計へ）。
  if (activeNow && !hubDismissed) {
    if (tapped) {
      tapped = false;
      const bool hitClear = (tapX >= CLR_X && tapX <= CLR_X + CLR_W &&
                             tapY >= CLR_Y && tapY <= CLR_Y + CLR_H);
      if (hitClear) {
        String id;
        if (alertMux) xSemaphoreTake(alertMux, portMAX_DELAY);
        if (hubIndex < alertCount) id = alId[hubIndex];
        if (alertMux) xSemaphoreGive(alertMux);
        clearAlert(id);
        if (alertCount == 0) {  // 最後の1件を消した → 時計へ
          hubDismissed = true;
          hubIndex = 0;
        }
      } else {
        hubIndex++;
        if (hubIndex >= alertCount) {  // 最後の次 → 閉じる
          hubDismissed = true;
          hubIndex = 0;
        }
      }
      forceDraw = true;
    }
    if (!hubDismissed) {
      if (forceDraw) renderHub();
      delay(10);
      return;
    }
  }

  // ハブを閉じた後でも ALERT 継続中なら、タップでハブ（QR）へ戻れる
  if (tapped && activeNow) {
    hubDismissed = false;
    hubIndex = 0;
    tapped = false;
    forceDraw = true;
    delay(10);
    return;
  }

  // ---- カレンダー通知層（穏やか）：Datadog アラートが無いときだけ表示 ----
  static long chimedRemSeq = -1;
  static bool remDismissed = false;
  static uint32_t remShownMs = 0;
  static long prevRemSeq = 0;
  const long remSeqNow = remSeq;
  if (remSeqNow != prevRemSeq) {  // 新しい通知でリセット
    prevRemSeq = remSeqNow;
    remDismissed = false;
  }
  const bool remPending = (remSeqNow > remAckedSeq) && !activeNow;
  if (remPending && !remDismissed) {
    if (remStart > 0 && time(nullptr) > remStart + 120) {
      ackReminder();              // 開始2分超過は黙って確認済み（遅延表示防止）
    } else {
      if (chimedRemSeq != remSeqNow) {  // 初回だけ穏やかなチャイム
        chimedRemSeq = remSeqNow;
        remShownMs = ms;
        calmChime();
        forceDraw = true;
      }
      if (tapped) {
        ackReminder();
        remDismissed = true;
        tapped = false;
        forceDraw = true;
      } else if (ms - remShownMs >= 60000UL) {  // 60秒で自動的に閉じる
        ackReminder();
        remDismissed = true;
        forceDraw = true;
      } else {
        if (forceDraw || ms - lastDraw >= 1000) {
          lastDraw = ms;
          renderReminder();
        }
        delay(10);
        return;
      }
    }
  }

  // ---- 「持ち上げたら怒る」層（アラート/通知が無いときだけ）----
  static bool angryActive = false;
  static uint32_t angryStart = 0;
  static uint32_t lastAngry = 0;
  static uint32_t lastVib = 0;
  static bool vibOn = false;
  constexpr uint32_t ANGRY_MS = 5000;        // 怒り顔の表示時間（振動も同じ間ずっと）
  constexpr uint32_t ANGRY_COOLDOWN = 5000;  // 連続で怒らないように

  if (!angryActive && !activeNow && !weightMode && remSeqNow <= remAckedSeq &&
      ms - lastAngry > ANGRY_COOLDOWN) {
    float ax, ay, az;
    if (M5.Imu.getAccel(&ax, &ay, &az)) {
      const float mag = sqrtf(ax * ax + ay * ay + az * az);
      if (fabsf(mag - 1.0f) > 0.30f) {  // 静止(約1g)からのズレ＝持ち上げ/移動
        angryActive = true;
        angryStart = ms;
        if (!gMuted) M5.Speaker.playWav(angryWav, angryWavLen);
        M5.Power.setVibration(255);
        vibOn = true;
        lastVib = ms;
      }
    }
  }
  if (angryActive) {
    if (ms - lastVib > 170) {  // 表示中はずっとプルプル振動
      lastVib = ms;
      vibOn = !vibOn;
      M5.Power.setVibration(vibOn ? 255 : 0);
    }
    renderAngry(ms);
    if (ms - angryStart >= ANGRY_MS) {
      angryActive = false;
      M5.Power.setVibration(0);
      lastAngry = ms;
      forceDraw = true;
    }
    delay(10);
    return;
  }
#endif

  // 重量モード: タッチ左右で株選択
  if (tapped && weightMode && !calMode && agavEnabled() && agavIsSelectMode()) {
    if (tapX < canvas.width() / 2) agavPrevPlant();
    else agavNextPlant();
    weightLastActivityMs = ms;
    forceDraw = true;
    tapped = false;
  }

  // 時計画面のタップで重量モードへ。
  if (tapped && !weightMode) {
    enterWeightMode(ms);
    forceDraw = true;
    tapped = false;
  }

  if (tapped && weightMode) {
    weightLastActivityMs = ms;
    tapped = false;
  }

  if (weightMode &&
      (calMode || weightLoadState == WLS_LOADED || agavIsSelectMode() ||
       agavIsSentMode())) {
    weightLastActivityMs = ms;
  }
  if (weightMode && !calMode && weightLoadState == WLS_IDLE &&
      !agavIsSelectMode() && !agavIsSentMode() &&
      ms - weightLastActivityMs >= WEIGHT_MODE_TIMEOUT_MS) {
    exitWeightMode();
    forceDraw = true;
  }

  // 重量モード中は時計の代わりに重量/キャリブ画面を表示
  if (weightMode) {
    if (scaleZeroActive() && !calMode) {
      serviceScaleZero(ms);
      forceDraw = true;
    }
    if (agavEnabled() && !calMode) agavThumbService();
    if (calMode) {
      if (scaleFound) {
        uint32_t adcU = 0;
        float g = 0.0f;
        if (miniScaleReadRaw(&adcU, &g)) {
          calLiveAdc = miniScaleAdcU32ToS32(adcU);
          calLiveG = g;
        }
      }
      forceDraw = true;
    } else if (!scaleZeroActive() && ms - lastWeightRead >= 100) {
      lastWeightRead = ms;
      if (!scaleFound) {
        scaleFound = miniScaleProbe();
        if (scaleFound) {
          scaleFiltersReady = false;
          initScaleFilters();
          beginScaleZero(false, true);
        }
      } else {
        pollWeight(ms);
        agavTick(ms, weightDisplayG, weightLoadState == WLS_LOADED);
      }
      forceDraw = true;
    }
    if (!calMode && ms - lastWeightLog >= 1000) {
      lastWeightLog = ms;
      Serial.printf("weight raw=%.1f disp=%.1f g adc=%lu zeroing=%d\n",
                    weightRawG, weightGrams, (unsigned long)weightAdc,
                    scaleZeroActive() ? 1 : 0);
    }
    if (forceDraw || ms - lastDraw >= 100) {
      lastDraw = ms;
      if (calMode) renderCal();
      else renderWeight();
    }
    forceDraw = false;
    delay(15);
    return;
  }

  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);

  if (currentPanel == PANEL_DIGITAL) {
    const bool nextColon = ((ms / 500) % 2) == 0;
    if (forceDraw || ms - lastDraw >= 100 || nextColon != colonOn) {
      lastDraw = ms;
      colonOn = nextColon;
      renderDigital(tm, colonOn);
    }
  } else {  // PANEL_ANALOG
    if (forceDraw || ms - lastDraw >= 1000) {
      lastDraw = ms;
      renderAnalog(tm);
    }
  }
  forceDraw = false;

  // 1時間ごとに NTP 再同期（WiFi 接続時のみ）
  if (wifiOk && ms - lastReSync >= 3600UL * 1000UL) {
    lastReSync = ms;
    struct tm tms;
    if (getLocalTime(&tms, 5000)) {
      rtcFromSystemTime();
    }
  }

  delay(10);
}
