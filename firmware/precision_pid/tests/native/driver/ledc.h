#pragma once

#include <cstdint>

#include "gpio.h"

enum ledc_mode_t { LEDC_LOW_SPEED_MODE = 0 };
enum ledc_channel_t { LEDC_CHANNEL_0 = 0, LEDC_CHANNEL_1 = 1 };
enum ledc_timer_t { LEDC_TIMER_0 = 0, LEDC_TIMER_1 = 1,
                    LEDC_TIMER_2 = 2, LEDC_TIMER_3 = 3 };
enum ledc_intr_type_t { LEDC_INTR_DISABLE = 0 };

struct ledc_channel_config_t {
  int gpio_num;
  ledc_mode_t speed_mode;
  ledc_channel_t channel;
  ledc_intr_type_t intr_type;
  ledc_timer_t timer_sel;
  uint32_t duty;
  int hpoint;
  struct {
    unsigned int output_invert : 1;
  } flags;
};

esp_err_t ledc_channel_config(const ledc_channel_config_t* config);

#include <Arduino.h>
