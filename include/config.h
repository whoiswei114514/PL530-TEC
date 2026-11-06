#pragma once

#include <Arduino.h>
#include <cstdint>

#ifndef PL530_ENABLE_UNSAFE_DEBUG
#define PL530_ENABLE_UNSAFE_DEBUG 0
#endif

// ESP32-S3-WROOM-1-N8R8 pin assignment.
constexpr uint8_t PIN_TEC_IN1 = 4;
constexpr uint8_t PIN_TEC_IN2 = 5;
constexpr uint8_t PIN_DRV_SLEEP = 6;
constexpr uint8_t PIN_DRV_FAULT = 7;
// Module pads 12 and 17 are GPIO8 and GPIO9, respectively.
constexpr uint8_t PIN_SDA = 8;
constexpr uint8_t PIN_SCL = 9;
// UART0 test points for the external CH340 programmer.
constexpr uint8_t PIN_UART_RX = 44;
constexpr uint8_t PIN_UART_TX = 43;
constexpr const char* HARDWARE_REVISION = "Kit_2/PCB3_2 DRV8701P + TMP117";
constexpr bool HAS_TEC_CURRENT_ADC = false;  // DRV_SO terminates at the test pad only.
// The TMP117 bus uses software I2C because the field wiring may be
// reversed. 2 kHz (250 us half-period) allows enough high time for the
// ESP32's weak internal pull-ups (~45k-60k) and cable capacitance to charge to 3.3V.
constexpr uint32_t SENSOR_I2C_FREQUENCY = 2000u;
constexpr uint32_t SENSOR_I2C_HALF_PERIOD_US =
    1000000u / (2u * SENSOR_I2C_FREQUENCY);
constexpr uint32_t SENSOR_I2C_HIGH_TIMEOUT_US = 10000u;
constexpr uint16_t SENSOR_I2C_TIMEOUT_MS = 25u;
static_assert(SENSOR_I2C_FREQUENCY > 0 && SENSOR_I2C_HALF_PERIOD_US > 0,
              "Software I2C timing must be nonzero");

// Native USB pins are left unconnected because no USB connector is fitted.

constexpr uint32_t TEC_PWM_FREQUENCY = 1000u;
// ESP32-S3's 40 MHz LEDC clock operates cleanly at 1 kHz with 10-bit resolution.
constexpr uint8_t TEC_PWM_RESOLUTION = 10;
constexpr uint16_t TEC_PWM_MAX_COUNT = static_cast<uint16_t>((1u << TEC_PWM_RESOLUTION) - 1u);
constexpr uint8_t TEC_CAL_OUTPUT_LIMIT_PERCENT = 30;
constexpr uint8_t LEDC_CHANNEL_IN1 = 0;
constexpr uint8_t LEDC_CHANNEL_IN2 = 1;
// Allow the DRV8701 charge pump and AVDD/DVDD rails to settle before nFAULT
// monitoring is armed. With a 47 uF VCP charge pump capacitor, precharge takes
// up to ~30-35 ms, during which DRV8701 actively asserts nFAULT low (VCPUV).
constexpr uint32_t DRV_WAKE_DELAY_US = 35000u;
constexpr uint32_t DRV_WAKE_TIMEOUT_US = 300000u;
constexpr uint32_t DRV_SLEEP_DELAY_US = 200u;
static_assert(PIN_DRV_SLEEP < 32, "The fault ISR uses the low GPIO output register");

// These safety limits are intentionally separate from the WebUI soft PWM band.
constexpr float HARD_MIN_TEMPERATURE_C = 10.0f;
constexpr float HARD_MAX_TEMPERATURE_C = 55.0f;
constexpr float HARD_RECOVER_MIN_C = 11.5f;
constexpr float HARD_RECOVER_MAX_C = 53.5f;
constexpr uint32_t RUNAWAY_NO_PROGRESS_MS = 10000u;
constexpr uint32_t RUNAWAY_LIMIT_MS = 30000u;
constexpr float RUNAWAY_MIN_COOLING_RATE_C_PER_S = -0.02f;
constexpr float RUNAWAY_MIN_HEATING_RATE_C_PER_S = 0.02f;

constexpr float DEFAULT_TARGET_C = 25.0f;
constexpr float DEFAULT_PWM_HIGH_C = 30.0f;
constexpr float DEFAULT_PWM_LOW_C = 20.0f;

constexpr float BALANCE_BASE_DUTY_PERCENT = 10.0f;
constexpr float NORMAL_CONTROL_DEADBAND_C = 0.03f;
constexpr float BALANCE_DEADBAND_C = 0.06f;
constexpr float TEMPERATURE_FILTER_ALPHA = 0.095f;
constexpr float CONTROL_LOOP_INTERVAL_MS = 100.0f;
constexpr float MAX_NORMAL_DUTY_STEP_PER_TICK = 15.0f;
constexpr float MAX_BALANCE_DRIVE_STEP_PER_TICK = 5.0f;
constexpr float BALANCE_SLOPE_GAIN = 12.0f;
constexpr float BALANCE_ERROR_GAIN = 4.0f;

constexpr uint32_t SENSOR_READ_INTERVAL_MS = 100u;
constexpr uint32_t SENSOR_RETRY_INTERVAL_MS = 200u;
constexpr uint32_t SENSOR_STALE_TIMEOUT_MS = 1000u;
constexpr uint32_t OUTPUT_GUARD_INTERVAL_US = 50000u;
static_assert(SENSOR_STALE_TIMEOUT_MS > 2 * SENSOR_READ_INTERVAL_MS,
              "Allow ordinary sensor polling jitter");

constexpr const char* DEVICE_NAME = "PL530_CTRL";
constexpr const char* AP_SSID = "PL530_CTRL";
constexpr const char* AP_PASSWORD = "pl530tecpass";
#ifndef TMP117_ADDRESS
// TMP117 ADD0 connected to GND selects 1001000b = 0x48.
#define TMP117_ADDRESS 0x48
#endif
static_assert(TMP117_ADDRESS >= 0x48 && TMP117_ADDRESS <= 0x4B,
              "TMP117 supports I2C addresses 0x48 through 0x4B");
inline IPAddress apIp() { return IPAddress(192, 168, 1, 1); }
inline IPAddress apGateway() { return IPAddress(192, 168, 1, 1); }
inline IPAddress apSubnet() { return IPAddress(255, 255, 255, 0); }
