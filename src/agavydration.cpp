#include "agavydration.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <math.h>

#include "agav_thumb.h"
#include "secrets.h"

#ifndef AGAV_API_URL
#define AGAV_API_URL ""
#endif
#ifndef AGAV_DEVICE_TOKEN
#define AGAV_DEVICE_TOKEN ""
#endif

struct AgavPlantEntry {
  String id;
  String nickname;
  String label;
  String status;
  String thumbPath;
  int latestWeight;
};

static AgavPlantEntry agavPlants[AGAV_MAX_PLANTS];
static int agavPlantCountN = 0;
static int agavPlantIndexN = 0;
static AgavApiState agavApiStateN = AGAV_API_IDLE;
static AgavSendState agavSendStateN = AGAV_SEND_NONE;
static AgavUiState agavUiStateN = AGAV_UI_MEASURING;
static char agavStatusBuf[64] = "";
static uint32_t agavStableSinceMs = 0;
static float agavStableAnchorG = 0.0f;
static float agavFrozenWeightG = 0.0f;
static int agavSentBannerWeightG = 0;
static char agavSentPlantLabel[48] = "";
static int agavSentPlantIndex = -1;

static bool agavUrlConfigured() {
  return AGAV_API_URL[0] != '\0';
}

bool agavEnabled() { return agavUrlConfigured(); }

static void agavSetStatus(const char *msg) {
  snprintf(agavStatusBuf, sizeof(agavStatusBuf), "%s", msg ? msg : "");
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
  agavUiStateN = AGAV_UI_SENT;
  agavFrozenWeightG = 0.0f;
  agavStableSinceMs = 0;
  agavStableAnchorG = 0.0f;
  agavSentBannerWeightG = weightG;
  snprintf(agavSentPlantLabel, sizeof(agavSentPlantLabel), "%s",
           agavPlantDisplayName(agavPlants[plantIndex]));
  agavSentPlantIndex = plantIndex;
  snprintf(agavStatusBuf, sizeof(agavStatusBuf), "sent %dg", weightG);
}

static void agavEnterSelect(float weightG) {
  agavUiStateN = AGAV_UI_SELECT_PLANT;
  agavFrozenWeightG = weightG;
  agavSendStateN = AGAV_SEND_NONE;
  agavSetStatus("pick plant");
  agavThumbRequest(agavPlantIndexN);
  Serial.printf("agav ready to record %.0fg\n", weightG);
}

static bool agavBeginHttp(HTTPClient &http, const String &url, WiFiClient &plain,
                          WiFiClientSecure &secure) {
  if (url.startsWith("https://")) {
    secure.setInsecure();
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

static void agavAddAuthHeader(HTTPClient &http) {
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

static bool agavFetchPlants() {
  if (!agavUrlConfigured()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    agavApiStateN = AGAV_API_ERROR;
    agavSetStatus("WiFi offline");
    return false;
  }

  agavApiStateN = AGAV_API_LOADING;
  agavSetStatus("loading plants...");

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
    snprintf(agavStatusBuf, sizeof(agavStatusBuf), "GET %d", code);
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
  int n = 0;
  for (JsonObject plant : plants) {
    if (n >= AGAV_MAX_PLANTS) break;
    agavPlants[n].id = String((const char *)(plant["id"] | ""));
    agavPlants[n].nickname = String((const char *)(plant["nickname"] | ""));
    agavPlants[n].label = String((const char *)(plant["label"] | ""));
    agavPlants[n].status = String(agavPickStatus(plant));
    agavPlants[n].thumbPath =
        String((const char *)(plant["photo_thumb_public_url"] | ""));
    agavPlants[n].latestWeight = plant["latest_weight"] | 0;
    n++;
  }
  agavPlantCountN = n;
  if (agavPlantIndexN >= agavPlantCountN) agavPlantIndexN = 0;

  if (agavPlantCountN == 0) {
    agavApiStateN = AGAV_API_ERROR;
    snprintf(agavStatusBuf, sizeof(agavStatusBuf), "empty (%ub)",
             (unsigned)body.length());
    return false;
  }

  agavApiStateN = AGAV_API_READY;
  if (agavUiStateN == AGAV_UI_MEASURING) {
    snprintf(agavStatusBuf, sizeof(agavStatusBuf), "%d plants ready",
             agavPlantCountN);
  }
  Serial.printf("agav loaded %d plants\n", agavPlantCountN);
  return true;
}

static bool agavPostReading(int weightG, int plantIndex) {
  if (!agavUrlConfigured() || agavPlantCountN == 0) return false;
  if (plantIndex < 0 || plantIndex >= agavPlantCountN) return false;
  if (WiFi.status() != WL_CONNECTED) {
    agavSendStateN = AGAV_SEND_FAIL;
    agavSetStatus("WiFi offline");
    return false;
  }

  const String plantId = agavPlants[plantIndex].id;
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
                  agavPlantDisplayName(agavPlants[plantIndex]));
    return true;
  }

  agavSendStateN = AGAV_SEND_FAIL;
  snprintf(agavStatusBuf, sizeof(agavStatusBuf), "POST %d", code);
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
  agavRefreshPlants();
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
  agavFetchPlants();
}

void agavPrevPlant() {
  if (agavPlantCountN <= 1) return;
  agavPlantIndexN = (agavPlantIndexN - 1 + agavPlantCountN) % agavPlantCountN;
  agavThumbRequest(agavPlantIndexN);
}

void agavNextPlant() {
  if (agavPlantCountN <= 1) return;
  agavPlantIndexN = (agavPlantIndexN + 1) % agavPlantCountN;
  agavThumbRequest(agavPlantIndexN);
}

void agavConfirmSend() {
  if (agavUiStateN != AGAV_UI_SELECT_PLANT) return;
  if (agavPlantCountN == 0) return;
  const int weightG = (int)lroundf(agavFrozenWeightG);
  if (weightG <= 0) return;
  if (agavPostReading(weightG, agavPlantIndexN)) {
    agavEnterSent(weightG, agavPlantIndexN);
  }
}

void agavCancelSelect() {
  if (agavUiStateN != AGAV_UI_SELECT_PLANT) return;
  agavEnterMeasuring();
  agavSendStateN = AGAV_SEND_NONE;
  if (agavPlantCountN > 0) {
    snprintf(agavStatusBuf, sizeof(agavStatusBuf), "%d plants ready",
             agavPlantCountN);
  }
}

bool agavPlantThumbPath(int index, char *out, size_t outLen) {
  if (!out || outLen == 0) return false;
  if (index < 0 || index >= agavPlantCountN) return false;
  const String &path = agavPlants[index].thumbPath;
  if (path.length() == 0) return false;
  snprintf(out, outLen, "%s", path.c_str());
  return true;
}

void agavTick(uint32_t nowMs, float displayG, bool loaded) {
  if (!agavUrlConfigured() || agavPlantCountN == 0) return;

  if (agavUiStateN == AGAV_UI_SENT) {
    if (!loaded) {
      agavClearSent();
      agavEnterMeasuring();
      snprintf(agavStatusBuf, sizeof(agavStatusBuf), "%d plants ready",
               agavPlantCountN);
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
      snprintf(agavStatusBuf, sizeof(agavStatusBuf), "%d plants ready",
               agavPlantCountN);
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

bool agavHasPlants() { return agavPlantCountN > 0; }
bool agavIsSelectMode() { return agavUiStateN == AGAV_UI_SELECT_PLANT; }
bool agavIsSentMode() { return agavUiStateN == AGAV_UI_SENT; }
bool agavIsStabilizing() {
  return agavUiStateN == AGAV_UI_MEASURING && agavStableSinceMs != 0;
}
int agavPlantCount() { return agavPlantCountN; }
int agavPlantIndex() { return agavPlantIndexN; }
float agavPendingWeightG() { return agavFrozenWeightG; }
AgavApiState agavApiState() { return agavApiStateN; }
AgavSendState agavSendState() { return agavSendStateN; }

const char *agavSelectedLabel() {
  if (agavPlantCountN == 0) return "—";
  return agavPlantDisplayName(agavPlants[agavPlantIndexN]);
}

const char *agavSelectedStatus() {
  if (agavPlantCountN == 0) return "";
  return agavPlants[agavPlantIndexN].status.c_str();
}

const char *agavStatusLine() { return agavStatusBuf; }

bool agavSentBannerActive(uint32_t nowMs) {
  (void)nowMs;
  return agavUiStateN == AGAV_UI_SENT;
}

int agavSentBannerWeight() { return agavSentBannerWeightG; }
const char *agavSentBannerLabel() { return agavSentPlantLabel; }
int agavSentBannerPlantIndex() { return agavSentPlantIndex; }
