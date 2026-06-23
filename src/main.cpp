#include <M5Unified.h>

static uint32_t lastTick = 0;
static uint32_t counter = 0;

void drawStatus() {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 10);
  M5.Display.printf("M5Stack Core2");

  M5.Display.setTextSize(1);
  M5.Display.setCursor(10, 50);
  M5.Display.printf("Uptime : %lu s", millis() / 1000);

  int32_t bat = M5.Power.getBatteryLevel();
  M5.Display.setCursor(10, 70);
  M5.Display.printf("Battery: %ld %%", bat);

  M5.Display.setCursor(10, 90);
  M5.Display.printf("Touched: %lu", counter);

  M5.Display.setCursor(10, 120);
  M5.Display.printf("Tap screen to count up");
  M5.Display.endWrite();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(128);

  Serial.begin(115200);
  Serial.println("M5Stack Core2 booted.");

  drawStatus();
  lastTick = millis();
}

void loop() {
  M5.update();

  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) {
      counter++;
      Serial.printf("Touch at (%d, %d), count=%lu\n", t.x, t.y, counter);
      drawStatus();
    }
  }

  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    drawStatus();
  }

  delay(10);
}
