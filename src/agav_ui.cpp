#include "agav_ui.h"

#include <M5Unified.h>
#include <math.h>

#include "agav_brand.h"
#include "agav_logo.h"
#include "agav_thumb.h"
#include "agavydration.h"

static constexpr int THUMB_SIZE = 96;
static constexpr int WATERMARK_SIZE = 96;
static constexpr uint8_t WATERMARK_ALPHA = 48;
static M5Canvas watermarkSprite(&M5.Display);
static bool watermarkReady = false;

static void drawScaledCenterText(M5Canvas &c, const char *text, int cx, int cy,
                                float targetH, uint16_t fg, uint16_t bg) {
  c.setFont(&fonts::Font4);
  c.setTextSize(1.0f);
  const int baseH = c.fontHeight();
  float scale = 1.0f;
  if (baseH > 0) scale = targetH / (float)baseH;
  c.setTextSize(scale);
  int tw = c.textWidth(text);
  const int W = c.width();
  if (tw > W - 12) {
    scale *= (float)(W - 12) / (float)tw;
    c.setTextSize(scale);
  }
  c.setTextDatum(middle_center);
  c.setTextColor(fg, bg);
  c.drawString(text, cx, cy);
}

static void drawScaledCenterWeight(M5Canvas &c, const char *value, int cx, int cy,
                                   float targetH, uint16_t numFg, uint16_t unitFg,
                                   uint16_t bg) {
  constexpr int kUnitGap = 4;
  constexpr float kUnitScale = 0.38f;

  c.setFont(&fonts::Font4);
  c.setTextSize(1.0f);
  const int baseH = c.fontHeight();
  float numScale = 1.0f;
  if (baseH > 0) numScale = targetH / (float)baseH;
  c.setTextSize(numScale);
  int numW = c.textWidth(value);

  c.setFont(&fonts::Font2);
  float unitScale = numScale * kUnitScale;
  c.setTextSize(unitScale);
  const int unitW = c.textWidth("g");
  int totalW = numW + kUnitGap + unitW;

  const int W = c.width();
  if (totalW > W - 12) {
    const float shrink = (float)(W - 12) / (float)totalW;
    numScale *= shrink;
    unitScale = numScale * kUnitScale;
    c.setFont(&fonts::Font4);
    c.setTextSize(numScale);
    numW = c.textWidth(value);
    c.setFont(&fonts::Font2);
    c.setTextSize(unitScale);
    totalW = numW + kUnitGap + c.textWidth("g");
  }

  const int left = cx - totalW / 2;
  c.setFont(&fonts::Font4);
  c.setTextSize(numScale);
  c.setTextDatum(middle_left);
  c.setTextColor(numFg, bg);
  c.drawString(value, left, cy);

  c.setFont(&fonts::Font2);
  c.setTextSize(unitScale);
  c.setTextDatum(middle_left);
  c.setTextColor(unitFg, bg);
  c.drawString("g", left + numW + kUnitGap, cy);
}

static uint16_t blendLogoPixel(uint16_t src, uint16_t bg) {
  const int a = WATERMARK_ALPHA;
  const int ia = 255 - a;
  const int sr = ((src >> 11) & 0x1F) * 255 / 31;
  const int sg = ((src >> 5) & 0x3F) * 255 / 63;
  const int sb = (src & 0x1F) * 255 / 31;
  const int br = ((bg >> 11) & 0x1F) * 255 / 31;
  const int bgc = ((bg >> 5) & 0x3F) * 255 / 63;
  const int bb = (bg & 0x1F) * 255 / 31;
  return lgfx::color565((sr * a + br * ia) / 255,
                        (sg * a + bgc * ia) / 255,
                        (sb * a + bb * ia) / 255);
}

static void ensureWatermark() {
  if (watermarkReady) return;
  watermarkSprite.setColorDepth(16);
  watermarkSprite.setPsram(true);
  if (!watermarkSprite.createSprite(WATERMARK_SIZE, WATERMARK_SIZE)) return;
  watermarkSprite.fillScreen(AGAV_BG);
  watermarkSprite.drawPng(AGAV_LOGO_PNG, AGAV_LOGO_PNG_LEN, 0, 0);
  for (int y = 0; y < WATERMARK_SIZE; y++) {
    for (int x = 0; x < WATERMARK_SIZE; x++) {
      const uint16_t src = watermarkSprite.readPixel(x, y);
      watermarkSprite.drawPixel(x, y, blendLogoPixel(src, AGAV_BG));
    }
  }
  watermarkReady = true;
}

static void drawWatermark(M5Canvas &c) {
  ensureWatermark();
  if (!watermarkReady) return;
  watermarkSprite.pushSprite(&c, (c.width() - WATERMARK_SIZE) / 2,
                             (c.height() - WATERMARK_SIZE) / 2);
}

static void drawFooter(M5Canvas &c, const char *left, const char *center,
                       uint16_t bg) {
  const int W = c.width();
  const int H = c.height();
  c.drawFastHLine(0, H - 24, W, AGAV_HAIRLINE);
  c.setFont(&fonts::Font2);
  c.setTextSize(1.0f);
  c.setTextColor(AGAV_INK_MUTED, bg);
  c.setTextDatum(bottom_left);
  c.drawString(left, 8, H - 4);
  c.setTextDatum(bottom_center);
  c.drawString(center, W / 2, H - 4);
}

static void drawNicknameBlock(M5Canvas &c, const char *name, int x, int y,
                              int maxW, int maxH, uint16_t fg, uint16_t bg) {
  const lgfx::IFont *fontsBySize[] = {
      &fonts::lgfxJapanGothicP_24,
      &fonts::lgfxJapanGothicP_20,
      &fonts::lgfxJapanGothicP_16,
  };
  c.setTextSize(1.0f);
  c.setTextDatum(top_left);
  c.setTextColor(fg, bg);

  for (const lgfx::IFont *font : fontsBySize) {
    c.setFont(font);
    if (c.textWidth(name) <= maxW && c.fontHeight() <= maxH) {
      const int textY = y + (maxH - c.fontHeight()) / 2;
      c.drawString(name, x, textY);
      c.drawString(name, x + 1, textY);
      return;
    }
  }

  c.setFont(&fonts::lgfxJapanGothicP_16);
  const int textY = y + (maxH - c.fontHeight()) / 2;
  c.drawString(name, x, textY);
  c.drawString(name, x + 1, textY);
}

static void drawPlantRow(M5Canvas &c, int cardX, int cardY, int cardW,
                         int cardH, const char *label, int plantIndex,
                         int index, int total) {
  c.fillRoundRect(cardX, cardY, cardW, cardH, 10, AGAV_SURFACE);
  c.drawRoundRect(cardX, cardY, cardW, cardH, 10, AGAV_HAIRLINE);

  const int thumbX = cardX + 8;
  const int thumbY = cardY + (cardH - THUMB_SIZE) / 2;
  agavThumbDraw(c, thumbX, thumbY, THUMB_SIZE, label, plantIndex);

  const int textX = thumbX + THUMB_SIZE + 8;
  const int textW = cardW - (textX - cardX) - 8;
  const int textH = cardH - 24;
  drawNicknameBlock(c, label, textX, cardY + 4, textW, textH, AGAV_INK,
                    AGAV_SURFACE);

  char counter[16];
  snprintf(counter, sizeof(counter), "%d / %d", index, total);
  c.setFont(&fonts::Font2);
  c.setTextSize(1.0f);
  c.setTextDatum(bottom_left);
  c.setTextColor(AGAV_INK_MUTED, AGAV_SURFACE);
  c.drawString(counter, textX, cardY + cardH - 8);
}

static void renderSent(M5Canvas &c, uint32_t nowMs) {
  const int W = c.width();
  const int H = c.height();
  c.fillScreen(AGAV_BG);

  char sentBuf[16];
  snprintf(sentBuf, sizeof(sentBuf), "%d", agavSentBannerWeight());
  drawScaledCenterText(c, "SENT", W / 2, 50, 32.0f, AGAV_SAGE_GLOW, AGAV_BG);
  drawScaledCenterWeight(c, sentBuf, W / 2, 96, 60.0f, AGAV_INK, AGAV_INK_MUTED,
                         AGAV_BG);

  const int cardW = W - 8;
  const int cardH = 96;
  const int cardX = 4;
  const int cardY = 120;
  drawPlantRow(c, cardX, cardY, cardW, cardH, agavSentBannerLabel(),
               agavSentBannerPlantIndex(), agavSentBannerPlantIndex() + 1,
               agavPlantCount());

  drawFooter(c, "", "lift pot", AGAV_BG);
  (void)nowMs;
}

static void renderSelect(M5Canvas &c, const AgavWeightView &view) {
  const int W = c.width();
  c.fillScreen(AGAV_BG);

  char wbuf[16];
  snprintf(wbuf, sizeof(wbuf), "%.0f g", agavPendingWeightG());
  c.setFont(&fonts::FreeSansBold12pt7b);
  c.setTextSize(1.0f);
  c.setTextDatum(middle_right);
  c.setTextColor(AGAV_SAND, AGAV_BG);
  c.drawString(wbuf, W - 8, 23);

  const int cardW = W - 8;
  const int cardH = 112;
  const int cardX = 4;
  const int cardY = 47;
  drawPlantRow(c, cardX, cardY, cardW, cardH, agavSelectedLabel(),
               agavPlantIndex(), agavPlantIndex() + 1, agavPlantCount());

  drawFooter(c, "A: cancel", "B: record", AGAV_BG);
  (void)view;
}

static void renderMeasuring(M5Canvas &c, const AgavWeightView &view) {
  const int W = c.width();
  const int H = c.height();
  c.fillScreen(AGAV_BG);
  drawWatermark(c);

  if (view.starting) {
    drawScaledCenterText(c, "Starting...", W / 2, H / 2, 28.0f, AGAV_SAND,
                         AGAV_BG);
    drawFooter(c, "A: exit", "", AGAV_BG);
    return;
  }
  if (!view.scaleFound) {
    drawScaledCenterText(c, "No scale", W / 2, H / 2, 28.0f, AGAV_RUST, AGAV_BG);
    drawFooter(c, "A: exit", "", AGAV_BG);
    return;
  }
  if (!view.scaleGapOk) {
    drawScaledCenterText(c, "Calibrate", W / 2, H / 2 - 10, 28.0f, AGAV_SAND,
                         AGAV_BG);
    c.setFont(&fonts::Font2);
    c.setTextSize(1.1f);
    c.setTextDatum(top_center);
    c.setTextColor(AGAV_INK_MUTED, AGAV_BG);
    c.drawString("Hold B", W / 2, H / 2 + 20);
    drawFooter(c, "A: exit", "B long: calib", AGAV_BG);
    return;
  }

  const bool showingZero = fabsf(view.liveWeightG) < 0.05f;
  const uint16_t weightColor =
      agavIsStabilizing()
          ? AGAV_SAND
          : (showingZero
                 ? (view.manualTareActive ? AGAV_SAGE_GLOW : AGAV_INK_MUTED)
                 : AGAV_SAGE_GLOW);
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", view.liveWeightG);
  drawScaledCenterWeight(c, buf, W / 2, H / 2 - 4, 92.0f, weightColor,
                         AGAV_INK_MUTED, AGAV_BG);

  if (view.manualTareActive) {
    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextSize(1.0f);
    c.setTextDatum(top_center);
    c.setTextColor(AGAV_SAGE_GLOW, AGAV_BG);
    c.drawString("TARE", W / 2, H / 2 + 50);
  }

  if (agavEnabled()) {
    uint16_t statusColor = AGAV_INK_MUTED;
    if (agavSendState() == AGAV_SEND_FAIL)
      statusColor = AGAV_RUST;
    else if (agavApiState() == AGAV_API_LOADING || agavIsStabilizing())
      statusColor = AGAV_SAND;
    c.setFont(&fonts::Font2);
    c.setTextSize(1.1f);
    c.setTextDatum(top_center);
    c.setTextColor(statusColor, AGAV_BG);
    c.drawString(agavStatusLine(), W / 2, H - 40);
    drawFooter(c, "A: exit", "B: tare  C: reload", AGAV_BG);
  } else {
    drawFooter(c, "A: exit", "B: tare  B long: calib", AGAV_BG);
  }

  if (view.weightHint && view.weightHint[0]) {
    c.setFont(&fonts::Font2);
    c.setTextSize(1.0f);
    c.setTextDatum(top_center);
    c.setTextColor(AGAV_SAND, AGAV_BG);
    c.drawString(view.weightHint, W / 2, H - 58);
  }
}

void agavRenderWeightScreen(M5Canvas &canvas, const AgavWeightView &view,
                            uint32_t nowMs) {
  if (agavEnabled() && agavSentBannerActive(nowMs)) {
    renderSent(canvas, nowMs);
    return;
  }
  if (agavEnabled() && agavIsSelectMode()) {
    renderSelect(canvas, view);
    return;
  }
  renderMeasuring(canvas, view);
}
