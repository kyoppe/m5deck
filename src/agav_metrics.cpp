#include "agav_metrics.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_freertos_hooks.h>
#include <esp_timer.h>
#include <time.h>

#include "agav_network.h"
#include "agav_tls.h"
#include "secrets.h"

#ifndef DD_API_KEY
#define DD_API_KEY ""
#endif
#ifndef DD_SITE
#define DD_SITE "datadoghq.com"
#endif
#ifndef AGAV_METRIC_PREFIX
#define AGAV_METRIC_PREFIX "test.kyouhei.iot"
#endif
#ifndef M5DECK_VERSION
#define M5DECK_VERSION "dev"
#endif

static constexpr uint32_t kMetricsIntervalMs = 15000;
static constexpr uint64_t kIdleSampleMinUs = 100;
static constexpr const char *kDeviceTag = "device:m5deck";

static portMUX_TYPE g_idleMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint64_t g_idleUs[2] = {0, 0};
static volatile uint64_t g_idleLastUs[2] = {0, 0};
static char g_versionTag[40];

static bool idleHookCore0() {
  const uint64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&g_idleMux);
  const uint64_t last = g_idleLastUs[0];
  if (last != 0 && now - last >= kIdleSampleMinUs) {
    g_idleUs[0] += now - last;
  }
  g_idleLastUs[0] = now;
  portEXIT_CRITICAL(&g_idleMux);
  return false;
}

static bool idleHookCore1() {
  const uint64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&g_idleMux);
  const uint64_t last = g_idleLastUs[1];
  if (last != 0 && now - last >= kIdleSampleMinUs) {
    g_idleUs[1] += now - last;
  }
  g_idleLastUs[1] = now;
  portEXIT_CRITICAL(&g_idleMux);
  return false;
}

static bool agavMetricsConfigured() { return DD_API_KEY[0] != '\0'; }

static void agavMetricsRegisterIdleHooks() {
  static bool registered = false;
  if (registered) return;
  registered = true;
  esp_register_freertos_idle_hook_for_cpu(idleHookCore0, 0);
  esp_register_freertos_idle_hook_for_cpu(idleHookCore1, 1);
}

static float agavMetricsCpuUserPct(uint64_t windowUs) {
  if (windowUs == 0) return 0.0f;

  uint64_t idle0 = 0;
  uint64_t idle1 = 0;
  const uint64_t now = esp_timer_get_time();

  portENTER_CRITICAL(&g_idleMux);
  idle0 = g_idleUs[0];
  idle1 = g_idleUs[1];
  g_idleUs[0] = 0;
  g_idleUs[1] = 0;
  g_idleLastUs[0] = now;
  g_idleLastUs[1] = now;
  portEXIT_CRITICAL(&g_idleMux);

  float idle0Pct = 100.0f * static_cast<float>(idle0) / static_cast<float>(windowUs);
  float idle1Pct = 100.0f * static_cast<float>(idle1) / static_cast<float>(windowUs);
  if (idle0Pct > 100.0f) idle0Pct = 100.0f;
  if (idle1Pct > 100.0f) idle1Pct = 100.0f;

  float cpuUser = 100.0f - (idle0Pct + idle1Pct) * 0.5f;
  if (cpuUser < 0.0f) cpuUser = 0.0f;
  if (cpuUser > 100.0f) cpuUser = 100.0f;
  return cpuUser;
}

static float agavMetricsMemoryPctUsable() {
  const uint32_t heapSize = ESP.getHeapSize();
  if (heapSize == 0) return 0.0f;
  return 100.0f * static_cast<float>(ESP.getFreeHeap()) /
         static_cast<float>(heapSize);
}

static bool agavMetricsPostPayload(const String &payload) {
  AgavNetworkGuard network(20000);
  if (!network) {
    Serial.println("metrics: network busy");
    return false;
  }

  WiFiClientSecure secure;
  secure.setCACert(DD_API_ROOT_CA);
  HTTPClient http;
  const String url = String("https://api.") + DD_SITE + "/api/v2/series";
  if (!http.begin(secure, url)) {
    Serial.println("metrics: http begin failed");
    return false;
  }

  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("DD-API-KEY", DD_API_KEY);

  const int code = http.POST(payload);
  const bool ok = code >= 200 && code < 300;
  if (!ok) {
    Serial.printf("metrics: POST failed code=%d\n", code);
    if (http.getSize() > 0 || http.getStreamPtr() != nullptr) {
      const String body = http.getString();
      if (body.length() > 0) {
        Serial.printf("metrics: %s\n", body.c_str());
      }
    }
  } else {
    Serial.println("metrics: sent");
  }

  http.end();
  return ok;
}

static bool agavMetricsSend(uint64_t windowUs) {
  const time_t nowTs = time(nullptr);
  if (nowTs < 1700000000) {
    Serial.println("metrics: skip (clock not synced)");
    return false;
  }

  const float cpuUser = agavMetricsCpuUserPct(windowUs);
  const float memoryPct = agavMetricsMemoryPctUsable();
  const int32_t batteryLevel = M5.Power.getBatteryLevel();
  const float batteryPct =
      batteryLevel < 0 ? 0.0f : static_cast<float>(batteryLevel);
  const bool charging = M5.Power.isCharging();
  const char *chargingTag = charging ? "charging:true" : "charging:false";

  char metricRunning[64];
  char metricCpu[64];
  char metricMemory[64];
  char metricBattery[64];
  snprintf(metricRunning, sizeof(metricRunning), "%s.device.running",
           AGAV_METRIC_PREFIX);
  snprintf(metricCpu, sizeof(metricCpu), "%s.cpu.user", AGAV_METRIC_PREFIX);
  snprintf(metricMemory, sizeof(metricMemory), "%s.memory.pct_usable",
           AGAV_METRIC_PREFIX);
  snprintf(metricBattery, sizeof(metricBattery), "%s.battery.pct",
           AGAV_METRIC_PREFIX);

  JsonDocument doc;
  JsonArray series = doc["series"].to<JsonArray>();

  {
    JsonObject item = series.add<JsonObject>();
    item["metric"] = metricRunning;
    item["type"] = 3;
    JsonArray points = item["points"].to<JsonArray>();
    JsonObject point = points.add<JsonObject>();
    point["timestamp"] = nowTs;
    point["value"] = 1;
    JsonArray tags = item["tags"].to<JsonArray>();
    tags.add(kDeviceTag);
    tags.add(g_versionTag);
  }

  {
    JsonObject item = series.add<JsonObject>();
    item["metric"] = metricCpu;
    item["type"] = 3;
    JsonArray points = item["points"].to<JsonArray>();
    JsonObject point = points.add<JsonObject>();
    point["timestamp"] = nowTs;
    point["value"] = cpuUser;
    JsonArray tags = item["tags"].to<JsonArray>();
    tags.add(kDeviceTag);
    tags.add(g_versionTag);
    tags.add("num_cores:2");
  }

  {
    JsonObject item = series.add<JsonObject>();
    item["metric"] = metricMemory;
    item["type"] = 3;
    JsonArray points = item["points"].to<JsonArray>();
    JsonObject point = points.add<JsonObject>();
    point["timestamp"] = nowTs;
    point["value"] = memoryPct;
    JsonArray tags = item["tags"].to<JsonArray>();
    tags.add(kDeviceTag);
    tags.add(g_versionTag);
  }

  {
    JsonObject item = series.add<JsonObject>();
    item["metric"] = metricBattery;
    item["type"] = 3;
    JsonArray points = item["points"].to<JsonArray>();
    JsonObject point = points.add<JsonObject>();
    point["timestamp"] = nowTs;
    point["value"] = batteryPct;
    JsonArray tags = item["tags"].to<JsonArray>();
    tags.add(kDeviceTag);
    tags.add(g_versionTag);
    tags.add(chargingTag);
  }

  String payload;
  serializeJson(doc, payload);
  return agavMetricsPostPayload(payload);
}

static void agavMetricsTask(void *param) {
  (void)param;
  uint64_t lastWindowUs = esp_timer_get_time();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kMetricsIntervalMs));

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("metrics: WiFi offline, reconnecting");
      WiFi.reconnect();
      lastWindowUs = esp_timer_get_time();
      continue;
    }

    const uint64_t nowUs = esp_timer_get_time();
    const uint64_t windowUs = nowUs - lastWindowUs;
    lastWindowUs = nowUs;
    agavMetricsSend(windowUs);
  }
}

void agavMetricsStart() {
  if (!agavMetricsConfigured()) {
    Serial.println("metrics: DD_API_KEY not set, disabled");
    return;
  }

  snprintf(g_versionTag, sizeof(g_versionTag), "version:%s", M5DECK_VERSION);
  agavMetricsRegisterIdleHooks();

  const BaseType_t created = xTaskCreatePinnedToCore(
      agavMetricsTask, "dd-metrics", 16384, nullptr, 1, nullptr, 0);
  if (created != pdPASS) {
    Serial.println("metrics: task create failed");
    return;
  }

  Serial.printf("metrics: started prefix=%s version=%s\n", AGAV_METRIC_PREFIX,
                M5DECK_VERSION);
}
