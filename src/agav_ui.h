#pragma once

#include <M5GFX.h>

struct AgavWeightView {
  bool scaleFound;
  bool scaleGapOk;
  bool manualTareActive;
  float liveWeightG;
  const char *weightHint;
};

void agavRenderWeightScreen(M5Canvas &canvas, const AgavWeightView &view, uint32_t nowMs);
