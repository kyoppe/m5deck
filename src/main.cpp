#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
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

// ハブ画面の「CLEAR」ボタン領域（画面座標）。この中をタップ＝表示中の1件を消す。
static constexpr int CLR_X = 16;
static constexpr int CLR_Y = 192;
static constexpr int CLR_W = 96;
static constexpr int CLR_H = 30;

// サイレン鳴動中か（未確認の ALERT がある）
static inline bool sirenActive() {
  return gAlertActive && alertSeq > alertAckedSeq;
}

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

  drawAlertBadge();
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

  drawAlertBadge();
  canvas.pushSprite(0, 0);
}

// ---- アラート通信 -------------------------------------------------------
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
      if (alertMux) xSemaphoreGive(alertMux);
      // スカラはそのまま反映
      alertSeq = seq;
      if (serverAck > alertAckedSeq) alertAckedSeq = serverAck;
      gAlertActive = (n > 0);
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
  canvas.drawString("TAP TO ACKNOWLEDGE", cx, H - 12);

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

  M5.Speaker.setVolume(180);

  computeLayout();
  syncTime();

  // ポーリングは別タスク(core0)で常時実行。メインループ(core1)は止めない。
  alertMux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(alertTask, "alert", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  M5.update();

  static uint32_t lastDraw = 0;
  static bool colonOn = true;
  static uint32_t lastReSync = 0;
  static bool forceDraw = true;

  const uint32_t ms = millis();

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

  // タップ検出（座標も保持）
  bool tapped = false;
  int tapX = 0, tapY = 0;
  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) {
      tapped = true;
      tapX = t.x;
      tapY = t.y;
    }
  }

  // ---- アラート層 ① サイレン（未確認の ALERT）：時計より優先 ----
  if (sirenActive()) {
    if (tapped) {
      ackAlert();           // 確認 → 即ミュート → 調査ハブへ
      M5.Speaker.stop();
      tapped = false;       // このタップはハブ閉じ/切替に使わない
      forceDraw = true;
    } else {
      sirenSound();
      renderAlert();
      delay(10);
      return;
    }
  }

  // サイレンでなければスピーカーは止めておく
  M5.Speaker.stop();

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

  // 画面タップでパネルを切り替え（アラート無し時のみ）
  if (tapped) {
    currentPanel = (currentPanel + 1) % PANEL_COUNT;
    forceDraw = true;
    Serial.printf("panel -> %d\n", currentPanel);
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
    // 通常は1秒毎。未解決アラートのバッジ点滅中は速めに再描画。
    const uint32_t interval = activeNow ? 250 : 1000;
    if (forceDraw || ms - lastDraw >= interval) {
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
