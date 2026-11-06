#pragma once
struct TwoWire {
  bool begin(unsigned char, unsigned char, unsigned int) { return true; }
  bool end() { return true; }
  void setTimeOut(unsigned short) {}
};
inline TwoWire Wire;
