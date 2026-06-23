#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"
#include "MujiNum.h"  // 無印 駅の時計用の数字フォント(Jost*)
#include "Seg7.h"     // デジタル時計用 7セグフォント(DSEG7 Classic Bold)

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

  canvas.pushSprite(0, 0);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(110);
  M5.Display.fillScreen(COLOR_BG);

  Serial.begin(115200);
  Serial.println("m5deck: clock panel boot");

  canvas.setColorDepth(16);
  canvas.setPsram(true);
  canvas.createSprite(M5.Display.width(), M5.Display.height());

  computeLayout();
  syncTime();
}

void loop() {
  M5.update();

  static uint32_t lastDraw = 0;
  static bool colonOn = true;
  static uint32_t lastReSync = 0;
  static bool forceDraw = true;

  const uint32_t ms = millis();

  // 画面タップでパネルを切り替え
  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) {
      currentPanel = (currentPanel + 1) % PANEL_COUNT;
      forceDraw = true;
      Serial.printf("panel -> %d\n", currentPanel);
    }
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
