#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"

// ---- 見た目の設定 -------------------------------------------------------
static constexpr uint16_t COLOR_BG = TFT_BLACK;
// ターミナル風のフォスファーグリーン（RGB565）
static constexpr uint16_t COLOR_GREEN = lgfx::color565(0, 255, 70);
static constexpr uint16_t COLOR_DIM = lgfx::color565(0, 110, 30);

// 320x240（横向き）に描くためのオフスクリーンキャンバス（ちらつき防止）
static M5Canvas canvas(&M5.Display);

// 時刻文字（HH:MM）のレイアウト
static float timeScale = 1.0f;
static int hhX = 0, colonX = 0, mmX = 0, timeY = 0;

static bool wifiOk = false;
static bool ntpOk = false;

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

  canvas.setFont(&fonts::Font7);
  canvas.setTextSize(1.0f);
  const int baseDigits = canvas.textWidth("00");
  const int baseColon = canvas.textWidth(":");
  const int baseTotal = baseDigits * 2 + baseColon;

  // 画面幅の約 88% に収まるようスケールを決める
  timeScale = (float)(W * 0.88f) / (float)baseTotal;
  canvas.setTextSize(timeScale);

  const int hhW = canvas.textWidth("00");
  const int colonW = canvas.textWidth(":");
  const int total = hhW * 2 + colonW;
  const int startX = (W - total) / 2;
  hhX = startX;
  colonX = startX + hhW;
  mmX = startX + hhW + colonW;
  timeY = H / 2 - 18;  // 少し上寄せして下に日付/秒の余白を作る
}

// ---- 描画 ---------------------------------------------------------------
static void render(const struct tm &tm, bool colonOn) {
  canvas.fillScreen(COLOR_BG);

  char hh[3], mm[3];
  snprintf(hh, sizeof(hh), "%02d", tm.tm_hour);
  snprintf(mm, sizeof(mm), "%02d", tm.tm_min);

  // 大きな HH:MM（7セグ・緑）
  canvas.setFont(&fonts::Font7);
  canvas.setTextSize(timeScale);
  canvas.setTextColor(COLOR_GREEN, COLOR_BG);
  canvas.setTextDatum(middle_left);
  canvas.drawString(hh, hhX, timeY);
  canvas.drawString(mm, mmX, timeY);
  if (colonOn) {
    canvas.drawString(":", colonX, timeY);
  }

  // 秒（小さめ・7セグ・右下）
  char ss[3];
  snprintf(ss, sizeof(ss), "%02d", tm.tm_sec);
  canvas.setTextSize(timeScale * 0.42f);
  canvas.setTextColor(COLOR_DIM, COLOR_BG);
  canvas.setTextDatum(bottom_right);
  canvas.drawString(ss, canvas.width() - 8, canvas.height() - 6);

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

  // 約 200ms ごとに描画判定（コロンは 500ms 周期で点滅）
  uint32_t ms = millis();
  bool nextColon = ((ms / 500) % 2) == 0;
  if (ms - lastDraw >= 100 || nextColon != colonOn) {
    lastDraw = ms;
    colonOn = nextColon;
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    render(tm, colonOn);
  }

  // 1時間ごとに NTP 再同期（WiFi 接続時のみ）
  if (wifiOk && ms - lastReSync >= 3600UL * 1000UL) {
    lastReSync = ms;
    struct tm tm;
    if (getLocalTime(&tm, 5000)) {
      rtcFromSystemTime();
    }
  }

  delay(10);
}
