#include "agav_thumb.h"

#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "agav_brand.h"
#include "agav_tls.h"
#include "agavydration.h"
#include "secrets.h"

#ifndef AGAV_API_URL
#define AGAV_API_URL ""
#endif
#ifndef AGAV_DEVICE_TOKEN
#define AGAV_DEVICE_TOKEN ""
#endif
#ifndef AGAV_CF_ACCESS_CLIENT_ID
#define AGAV_CF_ACCESS_CLIENT_ID ""
#endif
#ifndef AGAV_CF_ACCESS_CLIENT_SECRET
#define AGAV_CF_ACCESS_CLIENT_SECRET ""
#endif

static constexpr int kThumbPx = 96;
static constexpr size_t kMaxDownloadBytes = 65536;
static constexpr int kCacheSlots = 6;
static constexpr uint32_t kRequestDebounceMs = 80;
static constexpr uint32_t kDownloadTimeoutMs = 4000;
static constexpr int kPrefetchCount = 5;

struct ThumbSlot {
  int index = -1;
  bool ready = false;
  M5Canvas *sprite = nullptr;
};

static ThumbSlot gSlots[kCacheSlots];
static uint8_t gSlotAge[kCacheSlots] = {};
static uint8_t gSlotTick = 0;

struct ThumbDownloadJob {
  int index = -1;
  uint32_t generation = 0;
  char path[192] = "";
};

static portMUX_TYPE gThumbMux = portMUX_INITIALIZER_UNLOCKED;
static int gThumbPending = -1;
static uint32_t gThumbPendingSinceMs = 0;
static bool gThumbWorkerRunning = false;
static ThumbDownloadJob gThumbJob;
static uint8_t *gThumbResultBuf = nullptr;
static size_t gThumbResultLen = 0;
static int gThumbResultIndex = -1;
static uint32_t gThumbResultGeneration = 0;
static bool gCacheResetRequested = false;
static uint32_t gCacheGeneration = 0;
static int gPrefetchNext = -1;
static int gPrefetchRemaining = 0;

static bool httpBegin(HTTPClient &http, const String &url, WiFiClient &plain,
                      WiFiClientSecure &secure) {
  if (url.startsWith("https://")) {
    secure.setCACert(AGAV_ROOT_CA);
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

static void httpAuth(HTTPClient &http) {
  const bool accessConfigured =
      AGAV_CF_ACCESS_CLIENT_ID[0] != '\0' &&
      AGAV_CF_ACCESS_CLIENT_SECRET[0] != '\0';
  if (accessConfigured && strncmp(AGAV_API_URL, "https://", 8) != 0) {
    return;
  }
  if (accessConfigured) {
    http.addHeader("CF-Access-Client-Id", AGAV_CF_ACCESS_CLIENT_ID);
    http.addHeader("CF-Access-Client-Secret",
                   AGAV_CF_ACCESS_CLIENT_SECRET);
  }
  if (AGAV_DEVICE_TOKEN[0] != '\0') {
    http.addHeader("Authorization", String("Bearer ") + AGAV_DEVICE_TOKEN);
  }
}

static bool httpReadBody(HTTPClient &http, uint8_t **outBuf, size_t *outLen) {
  WiFiClient *stream = http.getStreamPtr();
  const int contentLength = http.getSize();
  if (contentLength > (int)kMaxDownloadBytes) return false;

  size_t cap =
      contentLength > 0 ? (size_t)contentLength : (size_t)8192;
  size_t len = 0;
  uint8_t *buf = (uint8_t *)ps_malloc(cap);
  if (!buf) return false;

  const uint32_t deadline = millis() + kDownloadTimeoutMs;
  while ((contentLength >= 0 && len < (size_t)contentLength) ||
         (contentLength < 0 &&
          (http.connected() || (stream && stream->available())))) {
    if ((int32_t)(millis() - deadline) > 0) break;
    if (!stream) break;

    const int available = stream->available();
    if (available <= 0) {
      delay(1);
      continue;
    }

    if (contentLength < 0 && len == cap) {
      if (cap >= kMaxDownloadBytes) break;
      const size_t newCap = min(cap * 2, kMaxDownloadBytes);
      uint8_t *grown = (uint8_t *)ps_realloc(buf, newCap);
      if (!grown) break;
      buf = grown;
      cap = newCap;
    }

    const size_t remaining =
        contentLength >= 0 ? (size_t)contentLength - len : cap - len;
    const size_t toRead = min((size_t)available, remaining);
    const int n = stream->read(buf + len, toRead);
    if (n <= 0) {
      delay(1);
      continue;
    }
    len += (size_t)n;
    if (contentLength >= 0 && len >= (size_t)contentLength) break;
    if (len >= kMaxDownloadBytes) break;
  }

  if (contentLength > 0 && len != (size_t)contentLength) {
    free(buf);
    return false;
  }
  if (len < 4) {
    free(buf);
    return false;
  }
  const bool isJpeg = buf[0] == 0xFF && buf[1] == 0xD8;
  const bool isPng = buf[0] == 0x89 && buf[1] == 0x50;
  if (!isJpeg && !isPng) {
    free(buf);
    return false;
  }
  *outBuf = buf;
  *outLen = len;
  return true;
}

static bool downloadPathToBuffer(const char *path, uint8_t **outBuf,
                                 size_t *outLen) {
  if (!path || path[0] == '\0') return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const String url = String(AGAV_API_URL) + path;
  if (!httpBegin(http, url, plain, secure)) return false;
  httpAuth(http);
  http.useHTTP10(true);
  http.setTimeout(kDownloadTimeoutMs);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  const bool ok = httpReadBody(http, outBuf, outLen);
  http.end();
  return ok;
}

static M5Canvas *ensureSlotSprite(ThumbSlot &slot) {
  if (!slot.sprite) {
    slot.sprite = new M5Canvas(&M5.Display);
    slot.sprite->setColorDepth(16);
    slot.sprite->setPsram(true);
    slot.sprite->createSprite(kThumbPx, kThumbPx);
  }
  return slot.sprite;
}

static bool decodeToSprite(M5Canvas &sprite, const uint8_t *buf, size_t len) {
  sprite.fillScreen(AGAV_PLACEHOLDER);
  const bool isPng = len >= 2 && buf[0] == 0x89 && buf[1] == 0x50;
  const bool isJpeg = len >= 2 && buf[0] == 0xFF && buf[1] == 0xD8;
  if (!isPng && !isJpeg) return false;
  if (isPng) {
    return sprite.drawPng(buf, ~0u, 0, 0, kThumbPx, kThumbPx);
  }
  return sprite.drawJpg(buf, ~0u, 0, 0, kThumbPx, kThumbPx);
}

static int cacheFind(int index) {
  for (int i = 0; i < kCacheSlots; i++) {
    if (gSlots[i].ready && gSlots[i].index == index) return i;
  }
  return -1;
}

static int cacheAlloc(int index) {
  int slot = cacheFind(index);
  if (slot >= 0) return slot;
  slot = 0;
  uint8_t oldest = 255;
  for (int i = 0; i < kCacheSlots; i++) {
    if (!gSlots[i].ready) {
      slot = i;
      break;
    }
    if (gSlotAge[i] < oldest) {
      oldest = gSlotAge[i];
      slot = i;
    }
  }
  gSlots[slot].index = index;
  gSlots[slot].ready = false;
  return slot;
}

static void cacheFreeAll() {
  for (int i = 0; i < kCacheSlots; i++) {
    if (gSlots[i].sprite) {
      delete gSlots[i].sprite;
      gSlots[i].sprite = nullptr;
    }
    gSlots[i].index = -1;
    gSlots[i].ready = false;
    gSlotAge[i] = 0;
  }
  gSlotTick = 0;
}

static bool decodeBufferToCache(int index, const uint8_t *buf, size_t len) {
  const int slot = cacheAlloc(index);
  M5Canvas *sprite = ensureSlotSprite(gSlots[slot]);
  const bool ok = sprite && decodeToSprite(*sprite, buf, len);
  if (!ok) return false;
  gSlots[slot].index = index;
  gSlots[slot].ready = true;
  gSlotAge[slot] = ++gSlotTick;
  return true;
}

static void thumbDownloadTask(void *) {
  uint8_t *buf = nullptr;
  size_t len = 0;
  const bool ok =
      downloadPathToBuffer(gThumbJob.path, &buf, &len) && buf != nullptr;

  portENTER_CRITICAL(&gThumbMux);
  if (ok && gThumbResultBuf == nullptr) {
    gThumbResultBuf = buf;
    gThumbResultLen = len;
    gThumbResultIndex = gThumbJob.index;
    gThumbResultGeneration = gThumbJob.generation;
    buf = nullptr;
  }
  gThumbWorkerRunning = false;
  portEXIT_CRITICAL(&gThumbMux);

  if (buf) free(buf);
  vTaskDelete(nullptr);
}

static bool startThumbDownload(int index) {
  char path[sizeof(gThumbJob.path)];
  if (!agavPlantThumbPath(index, path, sizeof(path))) return false;

  portENTER_CRITICAL(&gThumbMux);
  if (gThumbWorkerRunning || gThumbResultBuf != nullptr) {
    portEXIT_CRITICAL(&gThumbMux);
    return false;
  }
  gThumbJob.index = index;
  gThumbJob.generation = gCacheGeneration;
  snprintf(gThumbJob.path, sizeof(gThumbJob.path), "%s", path);
  gThumbWorkerRunning = true;
  portEXIT_CRITICAL(&gThumbMux);

  const BaseType_t created = xTaskCreatePinnedToCore(
      thumbDownloadTask, "agav-thumb", 12288, nullptr, 1, nullptr, 0);
  if (created == pdPASS) return true;

  portENTER_CRITICAL(&gThumbMux);
  gThumbWorkerRunning = false;
  portEXIT_CRITICAL(&gThumbMux);
  return false;
}

static void schedulePrefetchFrom(int plantIndex) {
  const int count = agavPlantCount();
  if (count <= 1) {
    gPrefetchNext = -1;
    gPrefetchRemaining = 0;
    return;
  }
  gPrefetchNext = (plantIndex + 1) % count;
  gPrefetchRemaining = min(kPrefetchCount, count - 1);
}

static void requestImmediate(int plantIndex) {
  if (plantIndex < 0) return;
  if (cacheFind(plantIndex) >= 0) return;
  gThumbPending = plantIndex;
  gThumbPendingSinceMs = millis() - kRequestDebounceMs;
}

void agavThumbInit() {
  if (agavPlantCount() <= 0) return;
  const int index = agavPlantIndex();
  requestImmediate(index);
  schedulePrefetchFrom(index);
}

void agavThumbShutdown() {
  gThumbPending = -1;
  gThumbPendingSinceMs = 0;
  gPrefetchNext = -1;
  gPrefetchRemaining = 0;
}

void agavThumbRequest(int plantIndex) {
  if (plantIndex < 0) return;
  if (cacheFind(plantIndex) >= 0) {
    const int slot = cacheFind(plantIndex);
    if (slot >= 0) gSlotAge[slot] = ++gSlotTick;
    gThumbPending = -1;
    gThumbPendingSinceMs = 0;
    schedulePrefetchFrom(plantIndex);
    return;
  }
  gThumbPending = plantIndex;
  gThumbPendingSinceMs = millis();
  schedulePrefetchFrom(plantIndex);
}

void agavThumbRequestCacheReset() {
  portENTER_CRITICAL(&gThumbMux);
  gCacheResetRequested = true;
  portEXIT_CRITICAL(&gThumbMux);
}

void agavThumbService() {
  bool resetRequested = false;
  uint8_t *resultBuf = nullptr;
  size_t resultLen = 0;
  int resultIndex = -1;
  uint32_t resultGeneration = 0;
  bool workerRunning = false;

  portENTER_CRITICAL(&gThumbMux);
  resetRequested = gCacheResetRequested;
  gCacheResetRequested = false;
  if (gThumbResultBuf != nullptr) {
    resultBuf = gThumbResultBuf;
    resultLen = gThumbResultLen;
    resultIndex = gThumbResultIndex;
    resultGeneration = gThumbResultGeneration;
    gThumbResultBuf = nullptr;
    gThumbResultLen = 0;
    gThumbResultIndex = -1;
  }
  workerRunning = gThumbWorkerRunning;
  portEXIT_CRITICAL(&gThumbMux);

  if (resetRequested) {
    gCacheGeneration++;
    cacheFreeAll();
    gThumbPending = -1;
    gPrefetchNext = -1;
    gPrefetchRemaining = 0;
    if (agavPlantCount() > 0) {
      const int index = agavPlantIndex();
      requestImmediate(index);
      schedulePrefetchFrom(index);
    }
  }

  if (resultBuf) {
    if (resultGeneration == gCacheGeneration) {
      if (decodeBufferToCache(resultIndex, resultBuf, resultLen) &&
          gThumbPending == resultIndex) {
        gThumbPending = -1;
        gThumbPendingSinceMs = 0;
      }
    }
    free(resultBuf);
  }

  if (workerRunning) return;
  if (gThumbPending >= 0) {
    if (millis() - gThumbPendingSinceMs < kRequestDebounceMs) return;
    const int index = gThumbPending;
    gThumbPending = -1;
    startThumbDownload(index);
    return;
  }

  while (gPrefetchRemaining > 0 && gPrefetchNext >= 0) {
    const int index = gPrefetchNext;
    const int count = agavPlantCount();
    gPrefetchNext = count > 0 ? (gPrefetchNext + 1) % count : -1;
    gPrefetchRemaining--;
    if (cacheFind(index) >= 0) continue;
    if (startThumbDownload(index)) return;
  }
}

bool agavThumbLoading() {
  portENTER_CRITICAL(&gThumbMux);
  const bool workerRunning =
      gThumbWorkerRunning || gThumbResultBuf != nullptr;
  portEXIT_CRITICAL(&gThumbMux);
  return workerRunning || gThumbPending >= 0;
}

bool agavThumbDraw(M5Canvas &canvas, int x, int y, int size, const char *label,
                   int plantIndex) {
  if (plantIndex < 0) plantIndex = agavPlantIndex();

  canvas.fillRoundRect(x, y, size, size, 10, AGAV_PLACEHOLDER);
  canvas.drawRoundRect(x, y, size, size, 10, AGAV_HAIRLINE);

  const int slot = cacheFind(plantIndex);
  if (slot >= 0 && gSlots[slot].sprite) {
    gSlots[slot].sprite->pushSprite(&canvas, x, y);
    canvas.drawRoundRect(x, y, size, size, 10, AGAV_SAGE);
    return true;
  }

  char initial[4] = "?";
  if (label && label[0]) {
    initial[0] = label[0];
    initial[1] = '\0';
  }
  canvas.setFont(&fonts::Font4);
  canvas.setTextSize(1.5f);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(AGAV_SAGE_GLOW, AGAV_PLACEHOLDER);
  canvas.drawString(initial, x + size / 2, y + size / 2 - 4);
  if (agavThumbLoading()) {
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1.0f);
    canvas.setTextColor(AGAV_INK_MUTED, AGAV_PLACEHOLDER);
    canvas.drawString("...", x + size / 2, y + size - 12);
  }
  return false;
}
