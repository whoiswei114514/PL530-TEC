#pragma once

#include <Arduino.h>
#include <array>
#include <deque>
#include <vector>

class TwoWire {
 public:
  std::array<uint16_t, 16> registers{};
  std::vector<uint8_t> tx;
  std::deque<uint8_t> rx;
  std::vector<uint8_t> registerReads;
  uint8_t selected = 0;
  uint8_t lastAddress = 0;
  bool nack = false;
  bool shortRead = false;
  bool ignoreConfigurationWrite = false;

  void beginTransmission(uint8_t address) { lastAddress = address; tx.clear(); }
  size_t write(uint8_t value) { tx.push_back(value); return 1; }
  uint8_t endTransmission(bool = true) {
    if (nack) return 2;
    if (tx.empty()) return 4;
    selected = tx[0];
    if (tx.size() == 3 && !(selected == 1 && ignoreConfigurationWrite)) {
      registers[selected] = (uint16_t(tx[1]) << 8) | tx[2];
    }
    return 0;
  }
  uint8_t requestFrom(uint8_t address, uint8_t count) {
    lastAddress = address;
    rx.clear();
    if (nack) return 0;
    registerReads.push_back(selected);
    uint16_t value = registers[selected];
    rx.push_back(value >> 8);
    if (!shortRead) rx.push_back(value & 0xFF);
    if (selected == 0 || selected == 1) registers[1] &= ~0x2000;
    return shortRead ? 1 : count;
  }
  int read() { if (rx.empty()) return -1; auto value = rx.front(); rx.pop_front(); return value; }
};
