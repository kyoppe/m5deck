#pragma once

#include <M5Unified.h>

// Core2 (AXP192): USB cable = ACIN, not VBUS (M-Bus 5V stays up on battery).
inline bool agavUsbCableConnected() {
  if (M5.Power.getType() == m5::Power_Class::pmic_t::pmic_axp192) {
    return M5.Power.Axp192.isACIN();
  }
  const int vbus = M5.Power.getVBUSVoltage();
  return vbus > 4000;
}
