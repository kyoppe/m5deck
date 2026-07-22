#include "agav_network.h"

static SemaphoreHandle_t gAgavNetworkMutex = nullptr;

void agavNetworkInit() {
  if (!gAgavNetworkMutex) {
    gAgavNetworkMutex = xSemaphoreCreateMutex();
  }
}

AgavNetworkGuard::AgavNetworkGuard(uint32_t timeoutMs) {
  agavNetworkInit();
  if (gAgavNetworkMutex) {
    locked_ =
        xSemaphoreTake(gAgavNetworkMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  }
}

AgavNetworkGuard::~AgavNetworkGuard() {
  if (locked_ && gAgavNetworkMutex) {
    xSemaphoreGive(gAgavNetworkMutex);
  }
}
