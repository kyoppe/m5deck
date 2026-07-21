#pragma once

#include <Arduino.h>

static constexpr int AGAV_MAX_PLANTS = 48;
static constexpr uint32_t AGAV_STABLE_MS = 2000;
static constexpr float AGAV_STABLE_BAND_G = 0.5f;
static constexpr float AGAV_MIN_SEND_G = 10.0f;

enum AgavUiState {
  AGAV_UI_MEASURING = 0,
  AGAV_UI_SELECT_PLANT = 1,
  AGAV_UI_SENT = 2,
};

enum AgavApiState {
  AGAV_API_DISABLED = 0,
  AGAV_API_IDLE,
  AGAV_API_LOADING,
  AGAV_API_READY,
  AGAV_API_ERROR,
};

enum AgavSendState {
  AGAV_SEND_NONE = 0,
  AGAV_SEND_PENDING,
  AGAV_SEND_OK,
  AGAV_SEND_FAIL,
};

bool agavEnabled();
void agavOnWeightModeEnter();
void agavOnWeightModeExit();
void agavLoadPlantsIfNeeded();
void agavTick(uint32_t nowMs, float displayG, bool loaded);
void agavPrevPlant();
void agavNextPlant();
void agavRefreshPlants();
void agavConfirmSend();
void agavCancelSelect();
bool agavPlantThumbPath(int index, char *out, size_t outLen);

bool agavHasPlants();
bool agavIsSelectMode();
bool agavIsSentMode();
bool agavIsStabilizing();
int agavPlantCount();
int agavPlantIndex();
float agavPendingWeightG();
AgavApiState agavApiState();
AgavSendState agavSendState();
const char *agavSelectedLabel();
const char *agavSelectedStatus();
const char *agavStatusLine();
bool agavSentBannerActive(uint32_t nowMs);
int agavSentBannerWeight();
const char *agavSentBannerLabel();
int agavSentBannerPlantIndex();
