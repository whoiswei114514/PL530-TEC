#pragma once
#include <Arduino.h>
struct DNSServer {
  void start(int, const char*, IPAddress) {}
  void processNextRequest() {}
};
