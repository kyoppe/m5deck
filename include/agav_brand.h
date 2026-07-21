#pragma once

#include <cstring>
#include <M5GFX.h>

// Agavydration brand palette (from public/styles.css)
static constexpr uint16_t AGAV_BG = lgfx::color565(28, 26, 23);         // #1c1a17
static constexpr uint16_t AGAV_SURFACE = lgfx::color565(36, 32, 25);    // #242019
static constexpr uint16_t AGAV_SURFACE2 = lgfx::color565(44, 39, 33);   // #2c2721
static constexpr uint16_t AGAV_INK = lgfx::color565(237, 232, 222);     // #ede8de
static constexpr uint16_t AGAV_INK_MUTED = lgfx::color565(149, 137, 122);  // #95897a
static constexpr uint16_t AGAV_SAGE = lgfx::color565(124, 148, 115);    // #7c9473
static constexpr uint16_t AGAV_SAGE_GLOW = lgfx::color565(159, 181, 150);  // #9fb596
static constexpr uint16_t AGAV_SAND = lgfx::color565(201, 166, 107);    // #c9a66b
static constexpr uint16_t AGAV_RUST = lgfx::color565(139, 62, 44);      // #8b3e2c
static constexpr uint16_t AGAV_LEARNING = lgfx::color565(140, 133, 121);   // #8c8579
static constexpr uint16_t AGAV_HAIRLINE = lgfx::color565(58, 52, 43);   // #3a342b
static constexpr uint16_t AGAV_PLACEHOLDER = lgfx::color565(26, 23, 20);  // #1a1714

inline uint16_t agavStatusColor(const char *status) {
  if (!status || !status[0]) return AGAV_INK_MUTED;
  if (strcmp(status, "ok") == 0) return AGAV_SAGE;
  if (strcmp(status, "soon") == 0) return AGAV_SAND;
  if (strcmp(status, "drying") == 0) return AGAV_RUST;
  if (strcmp(status, "learning") == 0) return AGAV_LEARNING;
  return AGAV_INK_MUTED;
}
