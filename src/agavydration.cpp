#include "agavydration.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <math.h>
#include <stdarg.h>

#include "agav_network.h"
#include "agav_tls.h"
#include "agav_thumb.h"
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

struct AgavPlantEntry {
  String id;
  String nickname;
  String label;
  String status;
  String thumbPath;
  int latestWeight;
};

static AgavPlantEntry agavPlantBanks[2][AGAV_MAX_PLANTS];
static int agavActivePlantBankN = 0;
static int agavPlantCountN = 0;
static int agavPlantIndexN = 0;
static volatile AgavApiState agavApiStateN = AGAV_API_IDLE;
static AgavSendState agavSendStateN = AGAV_SEND_NONE;
static AgavUiState agavUiStateN = AGAV_UI_MEASURING;
static portMUX_TYPE agavFetchMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE agavStatusMux = portMUX_INITIALIZER_UNLOCKED;
static bool agavFetchInProgressN = false;
static bool agavFetchIsReloadN = false;
static uint32_t agavStatusHoldUntilN = 0;
static char agavStatusBuf[64] = "";
static char agavStatusReadBuf[64] = "";
static uint32_t agavStableSinceMs = 0;
static float agavStableAnchorG = 0.0f;
static float agavFrozenWeightG = 0.0f;
static int agavSentBannerWeightG = 0;
static char agavSentPlantLabel[48] = "";
static int agavSentPlantIndex = -1;

struct AgavPlantSnapshot {
  AgavPlantEntry *plants;
  int count;
  int index;
};

static AgavPlantSnapshot agavPlantSnapshot() {
  portENTER_CRITICAL(&agavFetchMux);
  const AgavPlantSnapshot snapshot = {
      .plants = agavPlantBanks[agavActivePlantBankN],
      .count = agavPlantCountN,
      .index = agavPlantIndexN,
  };
  portEXIT_CRITICAL(&agavFetchMux);
  return snapshot;
}

static AgavPlantEntry *agavActivePlants() {
  return agavPlantSnapshot().plants;
}

static bool agavUrlConfigured() {
  return AGAV_API_URL[0] != '\0';
}

bool agavEnabled() { return agavUrlConfigured(); }

static void agavSetStatus(const char *msg) {
  portENTER_CRITICAL(&agavStatusMux);
  if ((int32_t)(agavStatusHoldUntilN - millis()) <= 0) {
    snprintf(agavStatusBuf, sizeof(agavStatusBuf), "%s", msg ? msg : "");
  }
  portEXIT_CRITICAL(&agavStatusMux);
}

static void agavSetStatusf(const char *format, ...) {
  portENTER_CRITICAL(&agavStatusMux);
  if ((int32_t)(agavStatusHoldUntilN - millis()) <= 0) {
    va_list args;
    va_start(args, format);
    vsnprintf(agavStatusBuf, sizeof(agavStatusBuf), format, args);
    va_end(args);
  }
  portEXIT_CRITICAL(&agavStatusMux);
}

static void agavSetTransientStatus(const char *msg, uint32_t durationMs) {
  portENTER_CRITICAL(&agavStatusMux);
  agavStatusHoldUntilN = millis() + durationMs;
  snprintf(agavStatusBuf, sizeof(agavStatusBuf), "%s", msg ? msg : "");
  portEXIT_CRITICAL(&agavStatusMux);
}

static void agavSetReadyStatus() {
  portENTER_CRITICAL(&agavStatusMux);
  if ((int32_t)(agavStatusHoldUntilN - millis()) <= 0) {
    snprintf(agavStatusBuf, sizeof(agavStatusBuf), "%d plants ready",
             agavPlantCountN);
  }
  portEXIT_CRITICAL(&agavStatusMux);
}

static void agavEnterMeasuring() {
  agavUiStateN = AGAV_UI_MEASURING;
  agavFrozenWeightG = 0.0f;
  agavStableSinceMs = 0;
  agavStableAnchorG = 0.0f;
}

static const char *agavPlantDisplayName(const AgavPlantEntry &plant) {
  if (plant.nickname.length() > 0) return plant.nickname.c_str();
  if (plant.label.length() > 0) return plant.label.c_str();
  return plant.id.c_str();
}

static void agavEnterSent(int weightG, int plantIndex) {
  const AgavPlantSnapshot snapshot = agavPlantSnapshot();
  agavUiStateN = AGAV_UI_SENT;
  agavFrozenWeightG = 0.0f;
  agavStableSinceMs = 0;
  agavStableAnchorG = 0.0f;
  agavSentBannerWeightG = weightG;
  snprintf(agavSentPlantLabel, sizeof(agavSentPlantLabel), "%s",
           plantIndex >= 0 && plantIndex < snapshot.count
               ? agavPlantDisplayName(snapshot.plants[plantIndex])
               : "");
  agavSentPlantIndex = plantIndex;
  agavSetStatusf("sent %dg", weightG);
}

static void agavEnterSelect(float weightG) {
  agavUiStateN = AGAV_UI_SELECT_PLANT;
  agavFrozenWeightG = weightG;
  agavSendStateN = AGAV_SEND_NONE;
  agavSetStatus("pick plant");
  agavThumbRequest(agavPlantIndexN);
  agavThumbStopPrefetch();
  Serial.printf("agav ready to record %.0fg\n", weightG);
}

static bool agavBeginHttp(HTTPClient &http, const String &url, WiFiClient &plain,
                          WiFiClientSecure &secure) {
  if (url.startsWith("https://")) {
    secure.setCACert(AGAV_ROOT_CA);
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

static void agavAddAuthHeader(HTTPClient &http) {
  const bool accessConfigured =
      AGAV_CF_ACCESS_CLIENT_ID[0] != '\0' &&
      AGAV_CF_ACCESS_CLIENT_SECRET[0] != '\0';
  if (accessConfigured && strncmp(AGAV_API_URL, "https://", 8) != 0) {
    return;
  }
  if (accessConfigured) {
    http.addHeader("CF-Access-Client-Id", AGAV_CF_ACCESS_CLIENT_ID);
    http.addHeader("CF-Access-Client-Secret", AGAV_CF_ACCESS_CLIENT_SECRET);
  }
  if (AGAV_DEVICE_TOKEN[0] != '\0') {
    http.addHeader("Authorization", String("Bearer ") + AGAV_DEVICE_TOKEN);
  }
}

static const char *agavPickStatus(JsonObject plant) {
  const char *status = plant["hydration_status"] | "";
  if (status[0] != '\0') return status;
  JsonObject forecast = plant["forecast"].as<JsonObject>();
  status = forecast["hydration_status"] | "";
  if (status[0] != '\0') return status;
  return "—";
}

static bool agavFetchPlants(bool reload) {
  if (!agavUrlConfigured()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    agavApiStateN = AGAV_API_ERROR;
    agavSetStatus("WiFi offline");
    return false;
  }
  AgavNetworkGuard network(20000);
  if (!network) {
    agavApiStateN = AGAV_API_ERROR;
    agavSetStatus("Network busy");
    return false;
  }

  agavApiStateN = AGAV_API_LOADING;
  agavSetStatus(reload ? "Reloading..." : "loading plants...");

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const String url = String(AGAV_API_URL) + "/api/device/plants";
  if (!agavBeginHttp(http, url, plain, secure)) {
    agavApiStateN = AGAV_API_ERROR;
    agavSetStatus("HTTP begin fail");
    return false;
  }
  agavAddAuthHeader(http);
  http.setTimeout(15000);
  const int code = http.GET();
  String body = http.getString();
  http.end();

  if (code != 200) {
    agavApiStateN = AGAV_API_ERROR;
    agavSetStatusf("GET %d", code);
    Serial.printf("agav fetch HTTP %d (%u bytes): %.200s\n", code,
                  (unsigned)body.length(), body.c_str());
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    agavApiStateN = AGAV_API_ERROR;
    agavSetStatus("JSON parse fail");
    Serial.printf("agav fetch JSON err: %s (%u bytes)\n", err.c_str(),
                  (unsigned)body.length());
    return false;
  }

  JsonArray plants = doc["plants"].as<JsonArray>();
  const AgavPlantSnapshot currentSnapshot = agavPlantSnapshot();
  const String selectedId =
      currentSnapshot.count > 0
          ? currentSnapshot.plants[currentSnapshot.index].id
          : "";
  const int targetBank = 1 - agavActivePlantBankN;
  AgavPlantEntry *targetPlants = agavPlantBanks[targetBank];
  int n = 0;
  for (JsonObject plant : plants) {
    if (n >= AGAV_MAX_PLANTS) break;
    targetPlants[n].id = String((const char *)(plant["id"] | ""));
    targetPlants[n].nickname =
        String((const char *)(plant["nickname"] | ""));
    targetPlants[n].label = String((const char *)(plant["label"] | ""));
    targetPlants[n].status = String(agavPickStatus(plant));
    targetPlants[n].thumbPath =
        String((const char *)(plant["photo_thumb_public_url"] | ""));
    targetPlants[n].latestWeight = plant["latest_weight"] | 0;
    n++;
  }

  if (n == 0) {
    agavApiStateN = AGAV_API_ERROR;
    agavSetStatusf("empty (%ub)", (unsigned)body.length());
    return false;
  }

  int newPlantIndex = 0;
  if (selectedId.length() > 0) {
    for (int i = 0; i < n; i++) {
      if (targetPlants[i].id == selectedId) {
        newPlantIndex = i;
        break;
      }
    }
  }
  portENTER_CRITICAL(&agavFetchMux);
  agavActivePlantBankN = targetBank;
  agavPlantCountN = n;
  agavPlantIndexN = newPlantIndex;
  portEXIT_CRITICAL(&agavFetchMux);
  agavThumbRequestCacheReset();

  agavApiStateN = AGAV_API_READY;
  if (agavUiStateN == AGAV_UI_MEASURING) {
    agavSetReadyStatus();
  }
  Serial.printf("agav loaded %d plants\n", agavPlantCountN);
  return true;
}

static bool agavClaimPlantFetch() {
  portENTER_CRITICAL(&agavFetchMux);
  const bool claimed = !agavFetchInProgressN;
  if (claimed) agavFetchInProgressN = true;
  portEXIT_CRITICAL(&agavFetchMux);
  return claimed;
}

static void agavReleasePlantFetch() {
  portENTER_CRITICAL(&agavFetchMux);
  agavFetchInProgressN = false;
  portEXIT_CRITICAL(&agavFetchMux);
}

static bool agavPlantFetchInProgress() {
  portENTER_CRITICAL(&agavFetchMux);
  const bool inProgress = agavFetchInProgressN;
  portEXIT_CRITICAL(&agavFetchMux);
  return inProgress;
}

static void agavPlantFetchTask(void *) {
  const bool reload = agavFetchIsReloadN;
  const bool ok = agavFetchPlants(reload);
  if (reload) {
    if (ok) {
      char message[64];
      snprintf(message, sizeof(message), "Reloaded %d plants",
               agavPlantCountN);
      agavSetTransientStatus(message, 2000);
    } else {
      agavSetTransientStatus("Reload failed", 2000);
    }
  }
  agavReleasePlantFetch();
  vTaskDelete(nullptr);
}

static bool agavStartPlantFetchTask(bool reload) {
  if (!agavClaimPlantFetch()) return false;
  agavFetchIsReloadN = reload;
  agavApiStateN = AGAV_API_LOADING;
  if (reload) {
    agavSetTransientStatus("Reloading...", 30000);
  } else {
    agavSetStatus("loading plants...");
  }
  const BaseType_t created = xTaskCreatePinnedToCore(
      agavPlantFetchTask, "agav-plants", 16384, nullptr, 1, nullptr, 0);
  if (created == pdPASS) return true;
  agavReleasePlantFetch();
  agavApiStateN = AGAV_API_ERROR;
  if (reload) {
    agavSetTransientStatus("Reload failed", 2000);
  } else {
    agavSetStatus("plant task fail");
  }
  return false;
}

static bool agavPostReading(int weightG, int plantIndex) {
  const AgavPlantSnapshot snapshot = agavPlantSnapshot();
  if (!agavUrlConfigured() || snapshot.count == 0) return false;
  if (plantIndex < 0 || plantIndex >= snapshot.count) return false;
  if (WiFi.status() != WL_CONNECTED) {
    agavSendStateN = AGAV_SEND_FAIL;
    agavSetStatus("WiFi offline");
    return false;
  }
  AgavNetworkGuard network(5000);
  if (!network) {
    agavSendStateN = AGAV_SEND_FAIL;
    agavSetStatus("Network busy");
    return false;
  }

  const String plantId = snapshot.plants[plantIndex].id;
  const String plantName = agavPlantDisplayName(snapshot.plants[plantIndex]);
  char body[128];
  snprintf(body, sizeof(body), "{\"plant_id\":\"%s\",\"weight\":%d}",
           plantId.c_str(), weightG);

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const String url = String(AGAV_API_URL) + "/api/device/readings";
  if (!agavBeginHttp(http, url, plain, secure)) {
    agavSendStateN = AGAV_SEND_FAIL;
    agavSetStatus("POST begin fail");
    return false;
  }
  agavAddAuthHeader(http);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  agavSendStateN = AGAV_SEND_PENDING;
  agavSetStatus("sending...");

  const int code = http.POST((uint8_t *)body, strlen(body));
  String resp = http.getString();
  http.end();

  if (code == 201 || code == 200) {
    agavSendStateN = AGAV_SEND_OK;
    Serial.printf("agav POST OK %dg plant=%s (%s)\n", weightG, plantId.c_str(),
                  plantName.c_str());
    return true;
  }

  agavSendStateN = AGAV_SEND_FAIL;
  agavSetStatusf("POST %d", code);
  Serial.printf("agav POST HTTP %d: %s\n", code, resp.c_str());
  return false;
}

void agavOnWeightModeEnter() {
  agavEnterMeasuring();
  agavSendStateN = AGAV_SEND_NONE;
  agavThumbInit();
  if (!agavUrlConfigured()) {
    agavApiStateN = AGAV_API_DISABLED;
    agavSetStatus("AGAV_API_URL unset");
    return;
  }
  if (agavPlantCountN > 0) {
    agavApiStateN = AGAV_API_READY;
    agavSetReadyStatus();
  } else {
    agavApiStateN = AGAV_API_IDLE;
    agavSetStatus("loading plants...");
  }
}

static void agavClearSent() {
  agavSentBannerWeightG = 0;
  agavSentPlantIndex = -1;
  agavSentPlantLabel[0] = '\0';
}

void agavOnWeightModeExit() {
  agavEnterMeasuring();
  agavClearSent();
  agavSendStateN = AGAV_SEND_NONE;
  agavThumbShutdown();
}

void agavRefreshPlants() {
  if (!agavUrlConfigured()) return;
  if (agavPlantFetchInProgress()) {
    agavSetTransientStatus("Reload busy", 2000);
    return;
  }
  agavStartPlantFetchTask(true);
}

void agavStartPlantPreload() {
  if (!agavUrlConfigured() || agavPlantCountN > 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  agavStartPlantFetchTask(false);
}

void agavPrevPlant() {
  portENTER_CRITICAL(&agavFetchMux);
  if (agavPlantCountN <= 1) {
    portEXIT_CRITICAL(&agavFetchMux);
    return;
  }
  agavPlantIndexN =
      (agavPlantIndexN - 1 + agavPlantCountN) % agavPlantCountN;
  const int index = agavPlantIndexN;
  portEXIT_CRITICAL(&agavFetchMux);
  agavThumbRequest(index);
}

void agavNextPlant() {
  portENTER_CRITICAL(&agavFetchMux);
  if (agavPlantCountN <= 1) {
    portEXIT_CRITICAL(&agavFetchMux);
    return;
  }
  agavPlantIndexN = (agavPlantIndexN + 1) % agavPlantCountN;
  const int index = agavPlantIndexN;
  portEXIT_CRITICAL(&agavFetchMux);
  agavThumbRequest(index);
}

void agavConfirmSend() {
  if (agavUiStateN != AGAV_UI_SELECT_PLANT) return;
  const AgavPlantSnapshot snapshot = agavPlantSnapshot();
  if (snapshot.count == 0) return;
  const int weightG = (int)lroundf(agavFrozenWeightG);
  if (weightG <= 0) return;
  if (agavPostReading(weightG, snapshot.index)) {
    agavEnterSent(weightG, snapshot.index);
  }
}

void agavCancelSelect() {
  if (agavUiStateN != AGAV_UI_SELECT_PLANT) return;
  agavEnterMeasuring();
  agavSendStateN = AGAV_SEND_NONE;
  if (agavPlantCountN > 0) {
    agavSetReadyStatus();
  }
}

bool agavPlantThumbPath(int index, char *out, size_t outLen) {
  if (!out || outLen == 0) return false;
  const AgavPlantSnapshot snapshot = agavPlantSnapshot();
  if (index < 0 || index >= snapshot.count) return false;
  const String &path = snapshot.plants[index].thumbPath;
  if (path.length() == 0) return false;
  snprintf(out, outLen, "%s", path.c_str());
  return true;
}

void agavTick(uint32_t nowMs, float displayG, bool loaded) {
  if (!agavUrlConfigured() || agavPlantCountN == 0) return;
  if (agavPlantFetchInProgress()) return;

  if (agavUiStateN == AGAV_UI_SENT) {
    if (!loaded) {
      agavClearSent();
      agavEnterMeasuring();
      agavSetReadyStatus();
    }
    return;
  }

  if (agavUiStateN == AGAV_UI_SELECT_PLANT) {
    if (!loaded) agavCancelSelect();
    return;
  }

  if (!loaded) {
    agavStableSinceMs = 0;
    if (agavPlantCountN > 0 && agavSendStateN != AGAV_SEND_PENDING) {
      agavSetReadyStatus();
    }
    return;
  }
  if (displayG < AGAV_MIN_SEND_G) {
    agavStableSinceMs = 0;
    return;
  }

  if (agavStableSinceMs == 0) {
    agavStableAnchorG = displayG;
    agavStableSinceMs = nowMs;
    agavSetStatus("hold steady...");
    return;
  }
  if (fabsf(displayG - agavStableAnchorG) > AGAV_STABLE_BAND_G) {
    agavStableAnchorG = displayG;
    agavStableSinceMs = nowMs;
    agavSetStatus("hold steady...");
    return;
  }
  if (nowMs - agavStableSinceMs < AGAV_STABLE_MS) return;

  const float weightG = roundf(displayG * 10.0f) / 10.0f;
  if (weightG <= 0.0f) return;
  agavEnterSelect(weightG);
}

bool agavHasPlants() { return agavPlantSnapshot().count > 0; }
bool agavIsSelectMode() { return agavUiStateN == AGAV_UI_SELECT_PLANT; }
bool agavIsSentMode() { return agavUiStateN == AGAV_UI_SENT; }
bool agavIsStabilizing() {
  return agavUiStateN == AGAV_UI_MEASURING && agavStableSinceMs != 0;
}
int agavPlantCount() { return agavPlantSnapshot().count; }
int agavPlantIndex() { return agavPlantSnapshot().index; }
float agavPendingWeightG() { return agavFrozenWeightG; }
AgavApiState agavApiState() { return agavApiStateN; }
AgavSendState agavSendState() { return agavSendStateN; }

const char *agavSelectedLabel() {
  const AgavPlantSnapshot snapshot = agavPlantSnapshot();
  if (snapshot.count == 0) return "—";
  return agavPlantDisplayName(snapshot.plants[snapshot.index]);
}

const char *agavSelectedStatus() {
  const AgavPlantSnapshot snapshot = agavPlantSnapshot();
  if (snapshot.count == 0) return "";
  return snapshot.plants[snapshot.index].status.c_str();
}

const char *agavStatusLine() {
  portENTER_CRITICAL(&agavStatusMux);
  snprintf(agavStatusReadBuf, sizeof(agavStatusReadBuf), "%s", agavStatusBuf);
  portEXIT_CRITICAL(&agavStatusMux);
  return agavStatusReadBuf;
}

bool agavSentBannerActive(uint32_t nowMs) {
  (void)nowMs;
  return agavUiStateN == AGAV_UI_SENT;
}

int agavSentBannerWeight() { return agavSentBannerWeightG; }
const char *agavSentBannerLabel() { return agavSentPlantLabel; }
int agavSentBannerPlantIndex() { return agavSentPlantIndex; }
