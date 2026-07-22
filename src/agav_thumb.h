#pragma once

#include <M5GFX.h>

void agavThumbInit();
void agavThumbShutdown();
void agavThumbRequest(int plantIndex);
void agavThumbStopPrefetch();
void agavThumbRequestCacheReset();
void agavThumbService();
bool agavThumbDraw(M5Canvas &canvas, int x, int y, int size, const char *label,
                   int plantIndex = -1);
bool agavThumbLoading();
