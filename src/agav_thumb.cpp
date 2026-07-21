#include "agav_thumb.h"

#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "agav_brand.h"
#include "agavydration.h"
#include "secrets.h"

#ifndef AGAV_API_URL
#define AGAV_API_URL ""
#endif
#ifndef AGAV_DEVICE_TOKEN
#define AGAV_DEVICE_TOKEN ""
#endif

static constexpr int kThumbPx = 96;
static constexpr size_t kMaxDownloadBytes = 65536;
static constexpr int kCacheSlots = 6;
static constexpr uint32_t kRequestDebounceMs = 250;
static constexpr uint32_t kDownloadTimeoutMs = 4000;

struct ThumbSlot {
  int index = -1;
  bool ready = false;
  M5Canvas *sprite = nullptr;
};

static ThumbSlot gSlots[kCacheSlots];
static uint8_t gSlotAge[kCacheSlots] = {};
static uint8_t gSlotTick = 0;

static int gThumbPending = -1;
static uint32_t gThumbPendingSinceMs = 0;
static bool gThumbLoading = false;

static bool httpBegin(HTTPClient &http, const String &url, WiFiClient &plain,
                      WiFiClientSecure &secure) {
  if (url.startsWith("https://")) {
    secure.setInsecure();
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

static void httpAuth(HTTPClient &http) {
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

static bool loadThumbForIndex(int index) {
  char path[192];
  if (!agavPlantThumbPath(index, path, sizeof(path))) return false;

  uint8_t *buf = nullptr;
  size_t len = 0;
  if (!downloadPathToBuffer(path, &buf, &len) || !buf) return false;

  const int slot = cacheAlloc(index);
  M5Canvas *sprite = ensureSlotSprite(gSlots[slot]);
  const bool ok = sprite && decodeToSprite(*sprite, buf, len);
  free(buf);

  if (!ok) return false;
  gSlots[slot].index = index;
  gSlots[slot].ready = true;
  gSlotAge[slot] = ++gSlotTick;
  return true;
}

void agavThumbInit() {}

void agavThumbShutdown() {
  gThumbPending = -1;
  gThumbPendingSinceMs = 0;
  gThumbLoading = false;
  cacheFreeAll();
}

void agavThumbRequest(int plantIndex) {
  if (plantIndex < 0) return;
  if (cacheFind(plantIndex) >= 0) return;
  gThumbPending = plantIndex;
  gThumbPendingSinceMs = millis();
}

void agavThumbService() {
  if (gThumbLoading || gThumbPending < 0) return;
  if (millis() - gThumbPendingSinceMs < kRequestDebounceMs) return;
  const int idx = gThumbPending;
  gThumbPending = -1;
  gThumbLoading = true;
  loadThumbForIndex(idx);
  gThumbLoading = false;
}

bool agavThumbLoading() { return gThumbLoading || gThumbPending >= 0; }

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
