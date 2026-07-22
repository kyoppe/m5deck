#pragma once

#include <Arduino.h>

void agavNetworkInit();

class AgavNetworkGuard {
 public:
  explicit AgavNetworkGuard(uint32_t timeoutMs);
  ~AgavNetworkGuard();

  explicit operator bool() const { return locked_; }

 private:
  bool locked_ = false;
};
