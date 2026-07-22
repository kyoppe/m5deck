#include "agav_thumb.h"

#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include "agav_brand.h"
#include "agav_network.h"
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
static constexpr size_t kMaxBatchBytes =
    6 + 8 * (8 + kMaxDownloadBytes);
static constexpr int kCacheSlots = 12;
static constexpr uint32_t kRequestDebounceMs = 80;
static constexpr uint32_t kDownloadTimeoutMs = 4000;
static constexpr int kBatchSize = 8;
static constexpr int kMinCachedAhead = 4;

struct ThumbSlot {
  int index = -1;
  bool ready = false;
  M5Canvas *sprite = nullptr;
};

static ThumbSlot gSlots[kCacheSlots];
static uint32_t gSlotAge[kCacheSlots] = {};
static uint32_t gSlotTick = 0;

struct ThumbDownloadJob {
  uint32_t generation = 0;
  int count = 0;
  int indexes[kBatchSize] = {};
  char paths[kBatchSize][192] = {};
};

static portMUX_TYPE gThumbMux = portMUX_INITIALIZER_UNLOCKED;
static int gThumbPending = -1;
static uint32_t gThumbPendingSinceMs = 0;
static bool gThumbWorkerRunning = false;
static ThumbDownloadJob gThumbJob;
static uint8_t *gThumbResultBuf = nullptr;
static size_t gThumbResultLen = 0;
static int gThumbResultCount = 0;
static int gThumbResultIndexes[kBatchSize] = {};
static uint32_t gThumbResultGeneration = 0;
static bool gCacheResetRequested = false;
static uint32_t gCacheGeneration = 0;
static bool gUnavailable[AGAV_MAX_PLANTS] = {};
static bool gPrefetchEnabled = true;
static bool gThumbPendingRequired = false;

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

static bool httpReadBody(HTTPClient &http, uint8_t **outBuf, size_t *outLen,
                         size_t maxBytes, bool requireImage) {
  WiFiClient *stream = http.getStreamPtr();
  const int contentLength = http.getSize();
  if (contentLength > (int)maxBytes) return false;

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
      if (cap >= maxBytes) break;
      const size_t newCap = min(cap * 2, maxBytes);
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
    if (len >= maxBytes) break;
  }

  if (contentLength > 0 && len != (size_t)contentLength) {
    free(buf);
    return false;
  }
  if (len < 4) {
    free(buf);
    return false;
  }
  if (requireImage) {
    const bool isJpeg = buf[0] == 0xFF && buf[1] == 0xD8;
    const bool isPng = buf[0] == 0x89 && buf[1] == 0x50;
    if (!isJpeg && !isPng) {
      free(buf);
      return false;
    }
  }
  *outBuf = buf;
  *outLen = len;
  return true;
}

static bool downloadBatchToBuffer(const ThumbDownloadJob &job,
                                  uint8_t **outBuf, size_t *outLen) {
  if (job.count <= 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  AgavNetworkGuard network(25000);
  if (!network) return false;

  JsonDocument requestDoc;
  JsonArray paths = requestDoc["paths"].to<JsonArray>();
  for (int i = 0; i < job.count; i++) {
    paths.add(job.paths[i]);
  }
  String requestBody;
  serializeJson(requestDoc, requestBody);

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const String url = String(AGAV_API_URL) + "/api/device/photos/batch";
  if (!httpBegin(http, url, plain, secure)) return false;
  httpAuth(http);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(kDownloadTimeoutMs);
  const int code =
      http.POST((uint8_t *)requestBody.c_str(), requestBody.length());
  if (code != 200) {
    http.end();
    return false;
  }
  const bool ok =
      httpReadBody(http, outBuf, outLen, kMaxBatchBytes, false);
  http.end();
  return ok && *outLen >= 6 && (*outBuf)[0] == 'A' &&
         (*outBuf)[1] == 'G' && (*outBuf)[2] == 'B' &&
         (*outBuf)[3] == '1';
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
  uint32_t oldest = UINT32_MAX;
  bool foundUnprotected = false;
  const int current = agavPlantIndex();
  const int plantCount = agavPlantCount();
  for (int i = 0; i < kCacheSlots; i++) {
    if (!gSlots[i].ready) {
      slot = i;
      break;
    }
    const int forwardDistance =
        plantCount > 0
            ? (gSlots[i].index - current + plantCount) % plantCount
            : kMinCachedAhead + 1;
    const bool protectedSlot =
        forwardDistance >= 0 && forwardDistance <= kMinCachedAhead;
    if (!protectedSlot && (!foundUnprotected || gSlotAge[i] < oldest)) {
      foundUnprotected = true;
      oldest = gSlotAge[i];
      slot = i;
    } else if (!foundUnprotected && gSlotAge[i] < oldest) {
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
      downloadBatchToBuffer(gThumbJob, &buf, &len) && buf != nullptr;

  portENTER_CRITICAL(&gThumbMux);
  if (ok && gThumbResultBuf == nullptr) {
    gThumbResultBuf = buf;
    gThumbResultLen = len;
    gThumbResultCount = gThumbJob.count;
    for (int i = 0; i < gThumbJob.count; i++) {
      gThumbResultIndexes[i] = gThumbJob.indexes[i];
    }
    gThumbResultGeneration = gThumbJob.generation;
    buf = nullptr;
  }
  gThumbWorkerRunning = false;
  portEXIT_CRITICAL(&gThumbMux);

  if (buf) free(buf);
  vTaskDelete(nullptr);
}

static bool startThumbBatch(int startIndex) {
  const int plantCount = agavPlantCount();
  if (plantCount <= 0) return false;

  ThumbDownloadJob job;
  job.generation = gCacheGeneration;
  for (int offset = 0;
       offset < plantCount && job.count < kBatchSize; offset++) {
    const int index = (startIndex + offset) % plantCount;
    if (cacheFind(index) >= 0 || gUnavailable[index]) continue;
    char path[sizeof(job.paths[0])];
    if (!agavPlantThumbPath(index, path, sizeof(path))) {
      gUnavailable[index] = true;
      continue;
    }
    job.indexes[job.count] = index;
    snprintf(job.paths[job.count], sizeof(job.paths[job.count]), "%s", path);
    job.count++;
  }
  if (job.count == 0) return false;

  portENTER_CRITICAL(&gThumbMux);
  if (gThumbWorkerRunning || gThumbResultBuf != nullptr) {
    portEXIT_CRITICAL(&gThumbMux);
    return false;
  }
  gThumbJob = job;
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

static void setBatchPending(int startIndex, bool required, bool immediate) {
  if (startIndex < 0) return;
  if (gThumbPending >= 0 && gThumbPendingRequired && !required) return;
  gThumbPending = startIndex;
  gThumbPendingRequired = required;
  gThumbPendingSinceMs =
      immediate ? millis() - kRequestDebounceMs : millis();
}

static void scheduleRollingPrefetch(int plantIndex) {
  const int count = agavPlantCount();
  if (count <= 0 || plantIndex < 0) return;
  if (cacheFind(plantIndex) < 0 && !gUnavailable[plantIndex]) {
    setBatchPending(plantIndex, true, false);
    return;
  }
  if (!gPrefetchEnabled) return;
  for (int offset = 1; offset <= min(kMinCachedAhead, count - 1); offset++) {
    const int index = (plantIndex + offset) % count;
    if (cacheFind(index) < 0 && !gUnavailable[index]) {
      setBatchPending(index, false, false);
      return;
    }
  }
}

static void requestImmediate(int plantIndex) {
  if (plantIndex < 0) return;
  gPrefetchEnabled = true;
  if (cacheFind(plantIndex) < 0 && !gUnavailable[plantIndex]) {
    setBatchPending(plantIndex, true, true);
  } else {
    scheduleRollingPrefetch(plantIndex);
  }
}

void agavThumbInit() {
  if (agavPlantCount() <= 0) return;
  const int index = agavPlantIndex();
  requestImmediate(index);
}

void agavThumbShutdown() {
  gThumbPending = -1;
  gThumbPendingRequired = false;
  gThumbPendingSinceMs = 0;
  gPrefetchEnabled = false;
}

void agavThumbRequest(int plantIndex) {
  if (plantIndex < 0) return;
  gPrefetchEnabled = true;
  if (cacheFind(plantIndex) >= 0) {
    const int slot = cacheFind(plantIndex);
    if (slot >= 0) gSlotAge[slot] = ++gSlotTick;
    gThumbPending = -1;
    gThumbPendingRequired = false;
    gThumbPendingSinceMs = 0;
    scheduleRollingPrefetch(plantIndex);
    return;
  }
  if (!gUnavailable[plantIndex]) {
    setBatchPending(plantIndex, true, false);
  }
}

void agavThumbStopPrefetch() {
  gPrefetchEnabled = false;
  if (!gThumbPendingRequired) {
    gThumbPending = -1;
    gThumbPendingSinceMs = 0;
  }
}

void agavThumbRequestCacheReset() {
  portENTER_CRITICAL(&gThumbMux);
  gCacheResetRequested = true;
  portEXIT_CRITICAL(&gThumbMux);
}

static uint16_t readU16(const uint8_t *buf) {
  return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t readU32(const uint8_t *buf) {
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static bool decodeBatchToCache(const uint8_t *buf, size_t len,
                               const int *indexes, int indexCount) {
  if (!buf || len < 6 || memcmp(buf, "AGB1", 4) != 0) return false;
  const uint16_t recordCount = readU16(buf + 4);
  if (recordCount > kBatchSize || recordCount > indexCount) return false;
  size_t offset = 6;
  for (uint16_t i = 0; i < recordCount; i++) {
    if (offset + 8 > len) return false;
    const uint16_t requestIndex = readU16(buf + offset);
    const uint16_t status = readU16(buf + offset + 2);
    const uint32_t bodyLen = readU32(buf + offset + 4);
    offset += 8;
    if (requestIndex >= indexCount || bodyLen > kMaxDownloadBytes ||
        offset + bodyLen > len) {
      return false;
    }
    offset += bodyLen;
  }
  if (offset != len) return false;

  offset = 6;
  for (uint16_t i = 0; i < recordCount; i++) {
    const uint16_t requestIndex = readU16(buf + offset);
    const uint16_t status = readU16(buf + offset + 2);
    const uint32_t bodyLen = readU32(buf + offset + 4);
    offset += 8;
    const int plantIndex = indexes[requestIndex];
    if (status == 200 && bodyLen > 0) {
      if (!decodeBufferToCache(plantIndex, buf + offset, bodyLen)) {
        gUnavailable[plantIndex] = true;
      }
    } else {
      gUnavailable[plantIndex] = true;
    }
    offset += bodyLen;
  }
  return true;
}

void agavThumbService() {
  bool resetRequested = false;
  uint8_t *resultBuf = nullptr;
  size_t resultLen = 0;
  int resultCount = 0;
  int resultIndexes[kBatchSize] = {};
  uint32_t resultGeneration = 0;
  bool workerRunning = false;

  portENTER_CRITICAL(&gThumbMux);
  resetRequested = gCacheResetRequested;
  gCacheResetRequested = false;
  if (gThumbResultBuf != nullptr) {
    resultBuf = gThumbResultBuf;
    resultLen = gThumbResultLen;
    resultCount = gThumbResultCount;
    for (int i = 0; i < resultCount; i++) {
      resultIndexes[i] = gThumbResultIndexes[i];
    }
    resultGeneration = gThumbResultGeneration;
    gThumbResultBuf = nullptr;
    gThumbResultLen = 0;
    gThumbResultCount = 0;
  }
  workerRunning = gThumbWorkerRunning;
  portEXIT_CRITICAL(&gThumbMux);

  if (resetRequested) {
    gCacheGeneration++;
    cacheFreeAll();
    memset(gUnavailable, 0, sizeof(gUnavailable));
    gThumbPending = -1;
    gThumbPendingRequired = false;
    gPrefetchEnabled = true;
    if (agavPlantCount() > 0) {
      const int index = agavPlantIndex();
      requestImmediate(index);
    }
  }

  if (resultBuf) {
    if (resultGeneration == gCacheGeneration) {
      decodeBatchToCache(resultBuf, resultLen, resultIndexes, resultCount);
    }
    free(resultBuf);
    scheduleRollingPrefetch(agavPlantIndex());
  }

  if (workerRunning) return;
  if (gThumbPending >= 0) {
    if (millis() - gThumbPendingSinceMs < kRequestDebounceMs) return;
    const int index = gThumbPending;
    gThumbPending = -1;
    gThumbPendingRequired = false;
    startThumbBatch(index);
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
