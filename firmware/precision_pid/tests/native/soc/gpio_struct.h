#pragma once

#include <cstdint>

struct GpioClearRegister {
  void operator=(uint32_t mask);
};

struct GpioStatusClearRegister {
  void operator=(uint32_t mask);
};

struct GpioRegisters {
  GpioClearRegister out_w1tc;
  uint32_t status = 0;
  GpioStatusClearRegister status_w1tc;
};

extern GpioRegisters GPIO;
