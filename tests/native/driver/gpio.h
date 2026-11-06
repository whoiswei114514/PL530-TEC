#pragma once

#include <cstdint>

using esp_err_t = int;
using gpio_num_t = int;
using gpio_isr_t = void (*)(void*);
enum gpio_int_type_t { GPIO_INTR_DISABLE = 0, GPIO_INTR_NEGEDGE = 2 };
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_NOT_FOUND = 0x105;
constexpr int ESP_INTR_FLAG_IRAM = 1 << 10;

esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void* argument);
