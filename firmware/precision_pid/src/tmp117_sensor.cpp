#include "tmp117_sensor.h"

namespace {
constexpr uint8_t REG_TEMPERATURE = 0x00;
constexpr uint8_t REG_CONFIGURATION = 0x01;
constexpr uint8_t REG_DEVICE_ID = 0x0F;
constexpr uint16_t DEVICE_ID_TMP117 = 0x0117;
constexpr uint16_t DEVICE_ID_MASK = 0x0FFF;

// AVG=01 averages eight conversions (~124 ms), below the 200 ms read interval.
constexpr uint16_t CONFIGURATION_CONTINUOUS_AVERAGE_8 = 0x0020;
// MOD[1:0], CONV[2:0], and AVG[1:0]. Alert polarity/mode is not part of the
// sampling contract and is intentionally left out of the runtime validation.
constexpr uint16_t CONFIGURATION_MODE_MASK = 0x0FE0u;
constexpr uint16_t DATA_READY = 0x2000u;
constexpr uint16_t EEPROM_BUSY = 0x1000u;
constexpr uint8_t DEVICE_ID_READ_ATTEMPTS = 3u;
constexpr uint8_t CONFIGURATION_READY_ATTEMPTS = 12u;
constexpr uint8_t DATA_READY_ATTEMPTS = 8u;
constexpr uint16_t REGISTER_POLL_DELAY_MS = 2u;
}  // namespace

bool Tmp117Sensor::begin(TwoWire& wire) {
  end();
  m_wire = &wire;
  m_busMode = BusMode::Hardware;
  m_initialized = false;
  m_lastError.clear();
  return initializeDevice();
}

bool Tmp117Sensor::begin(uint8_t sda, uint8_t scl) {
  end();
  m_wire = nullptr;
  m_busMode = BusMode::Software;
  m_sda = sda;
  m_scl = scl;
  m_initialized = false;
  m_lastError.clear();
  recoverBus();
  delay(1);
  return initializeDevice();
}

void Tmp117Sensor::end() {
  if (m_busMode == BusMode::Software) {
    releaseLine(m_sda);
    releaseLine(m_scl);
  }
  m_wire = nullptr;
  m_busMode = BusMode::None;
  m_initialized = false;
}

bool Tmp117Sensor::initializeDevice() {

  delay(2);
  uint16_t id = 0;
  bool idRead = false;
  bool validId = false;
  for (uint8_t attempt = 0; attempt < DEVICE_ID_READ_ATTEMPTS; ++attempt) {
    uint16_t firstId = 0;
    uint16_t secondId = 0;
    uint16_t thirdId = 0;
    if (readRegister(REG_DEVICE_ID, firstId)) {
      idRead = true;
      id = firstId;
      if ((firstId & DEVICE_ID_MASK) == DEVICE_ID_TMP117 &&
          readRegister(REG_DEVICE_ID, secondId) &&
          readRegister(REG_DEVICE_ID, thirdId) &&
          secondId == firstId && thirdId == firstId) {
        validId = true;
        break;
      }
    }
    delay(REGISTER_POLL_DELAY_MS);
  }
  if (!idRead) {
    m_lastError = F("TMP117 I2C 无响应");
    return false;
  }
  if (!validId) {
    m_lastError = String(F("TMP117 ID 错误: 0x")) + String(id, HEX);
    return false;
  }

  uint16_t configuration = 0;
  bool configurationReady = false;
  for (uint8_t attempt = 0; attempt < CONFIGURATION_READY_ATTEMPTS; ++attempt) {
    if (!readRegister(REG_CONFIGURATION, configuration)) {
      m_lastError = F("TMP117 配置读取失败");
      return false;
    }
    if ((configuration & EEPROM_BUSY) == 0) {
      configurationReady = true;
      break;
    }
    delay(REGISTER_POLL_DELAY_MS);
  }
  if (!configurationReady) {
    m_lastError = F("TMP117 EEPROM 仍在忙");
    return false;
  }

  if ((configuration & CONFIGURATION_MODE_MASK) != CONFIGURATION_CONTINUOUS_AVERAGE_8 &&
      !writeRegister(REG_CONFIGURATION, CONFIGURATION_CONTINUOUS_AVERAGE_8)) {
    m_lastError = F("TMP117 配置写入失败");
    return false;
  }

  bool configured = false;
  for (uint8_t attempt = 0; attempt < CONFIGURATION_READY_ATTEMPTS; ++attempt) {
    if (!readRegister(REG_CONFIGURATION, configuration)) {
      m_lastError = F("TMP117 配置回读失败");
      return false;
    }
    if ((configuration & EEPROM_BUSY) == 0 &&
        (configuration & CONFIGURATION_MODE_MASK) == CONFIGURATION_CONTINUOUS_AVERAGE_8) {
      configured = true;
      break;
    }
    delay(REGISTER_POLL_DELAY_MS);
  }
  if (!configured) {
    m_lastError = F("TMP117 配置回读失败");
    return false;
  }

  m_lastError = F("等待首次转换");
  // Wait through the first conversion so the initial controller read cannot
  // observe the reset value of -256 C and turn into a false sensor fault.
  delay(140);
  m_initialized = true;
  return true;
}

bool Tmp117Sensor::readTemperature(float& temperatureC) {
  temperatureC = NAN;
  if (m_busMode == BusMode::None || !m_initialized) {
    m_lastError = F("TMP117 未初始化");
    return false;
  }

  uint16_t configuration = 0;
  bool dataReady = false;
  for (uint8_t attempt = 0; attempt < DATA_READY_ATTEMPTS; ++attempt) {
    if (!readRegister(REG_CONFIGURATION, configuration)) {
      m_lastError = F("TMP117 状态读取失败");
      return false;
    }
    if ((configuration & EEPROM_BUSY) != 0 ||
        (configuration & CONFIGURATION_MODE_MASK) != CONFIGURATION_CONTINUOUS_AVERAGE_8) {
      m_lastError = F("TMP117 配置改变或正在复位");
      m_initialized = false;
      return false;
    }
    if ((configuration & DATA_READY) != 0) {
      dataReady = true;
      break;
    }
    delay(REGISTER_POLL_DELAY_MS);
  }
  if (!dataReady) {
    m_lastError = F("TMP117 没有新的温度转换");
    return false;
  }

  uint16_t raw = 0;
  if (!readRegister(REG_TEMPERATURE, raw)) {
    m_lastError = F("TMP117 温度读取失败");
    return false;
  }

  // TMP117 uses signed 16-bit values with an LSB of 1/128 degree Celsius.
  const int16_t signedRaw = static_cast<int16_t>(raw);
  temperatureC = static_cast<float>(signedRaw) / 128.0f;
  if (temperatureC < -55.0f || temperatureC > 150.0f || !isfinite(temperatureC)) {
    m_lastError = F("TMP117 温度超出传感器量程");
    temperatureC = NAN;
    return false;
  }

  m_lastError = F("正常");
  return true;
}

bool Tmp117Sensor::readRegister(uint8_t registerAddress, uint16_t& value) {
  if (m_busMode == BusMode::Hardware) {
    return readHardwareRegister(registerAddress, value);
  }
  if (m_busMode == BusMode::Software) {
    return readSoftwareRegister(registerAddress, value);
  }
  return false;
}

bool Tmp117Sensor::writeRegister(uint8_t registerAddress, uint16_t value) {
  if (m_busMode == BusMode::Hardware) {
    return writeHardwareRegister(registerAddress, value);
  }
  if (m_busMode == BusMode::Software) {
    return writeSoftwareRegister(registerAddress, value);
  }
  return false;
}

bool Tmp117Sensor::readHardwareRegister(uint8_t registerAddress, uint16_t& value) {
  if (m_wire == nullptr) return false;
  m_wire->beginTransmission(TMP117_ADDRESS);
  m_wire->write(registerAddress);
  if (m_wire->endTransmission(false) != 0) {
    return false;
  }
  if (m_wire->requestFrom(static_cast<uint8_t>(TMP117_ADDRESS), static_cast<uint8_t>(2)) != 2) {
    return false;
  }

  const uint8_t msb = m_wire->read();
  const uint8_t lsb = m_wire->read();
  value = static_cast<uint16_t>((static_cast<uint16_t>(msb) << 8) | lsb);
  return true;
}

bool Tmp117Sensor::writeHardwareRegister(uint8_t registerAddress, uint16_t value) {
  if (m_wire == nullptr) return false;
  m_wire->beginTransmission(TMP117_ADDRESS);
  m_wire->write(registerAddress);
  m_wire->write(static_cast<uint8_t>(value >> 8));
  m_wire->write(static_cast<uint8_t>(value & 0xFFu));
  return m_wire->endTransmission() == 0;
}

void Tmp117Sensor::releaseLine(uint8_t pin) {
  pinMode(pin, INPUT_PULLUP);
}

void Tmp117Sensor::driveLow(uint8_t pin) {
  pinMode(pin, OUTPUT_OPEN_DRAIN);
  digitalWrite(pin, LOW);
}

void Tmp117Sensor::busDelay() {
  delayMicroseconds(SENSOR_I2C_HALF_PERIOD_US);
}

bool Tmp117Sensor::waitHigh(uint8_t pin) {
  const uint32_t started = micros();
  while (digitalRead(pin) == LOW) {
    if (static_cast<uint32_t>(micros() - started) >= SENSOR_I2C_HIGH_TIMEOUT_US) {
      return false;
    }
    delayMicroseconds(1);
  }
  return true;
}

void Tmp117Sensor::recoverBus() {
  releaseLine(m_sda);
  releaseLine(m_scl);
  busDelay();
  for (int i = 0; i < 9; ++i) {
    if (digitalRead(m_sda) == HIGH) break;
    driveLow(m_scl);
    busDelay();
    releaseLine(m_scl);
    waitHigh(m_scl);
    busDelay();
  }
  driveLow(m_sda);
  busDelay();
  releaseLine(m_scl);
  waitHigh(m_scl);
  busDelay();
  releaseLine(m_sda);
  busDelay();
}

bool Tmp117Sensor::startCondition() {
  releaseLine(m_sda);
  busDelay();
  releaseLine(m_scl);
  if (!waitHigh(m_scl)) return false;
  busDelay();
  if (digitalRead(m_sda) == LOW) {
    recoverBus();
    if (digitalRead(m_sda) == LOW) return false;
  }
  driveLow(m_sda);
  busDelay();
  driveLow(m_scl);
  busDelay();
  return true;
}

bool Tmp117Sensor::stopCondition() {
  driveLow(m_sda);
  busDelay();
  releaseLine(m_scl);
  const bool clockReleased = waitHigh(m_scl);
  busDelay();
  releaseLine(m_sda);
  busDelay();
  return clockReleased && digitalRead(m_sda) == HIGH;
}

bool Tmp117Sensor::writeByte(uint8_t value) {
  for (uint8_t mask = 0x80u; mask != 0; mask >>= 1) {
    if ((value & mask) != 0) {
      releaseLine(m_sda);
    } else {
      driveLow(m_sda);
    }
    busDelay();
    releaseLine(m_scl);
    if (!waitHigh(m_scl)) return false;
    busDelay();
    driveLow(m_scl);
    busDelay();
  }

  releaseLine(m_sda);
  busDelay();
  releaseLine(m_scl);
  if (!waitHigh(m_scl)) return false;
  busDelay();
  const bool acknowledged = digitalRead(m_sda) == LOW;
  driveLow(m_scl);
  busDelay();
  releaseLine(m_sda);
  return acknowledged;
}

bool Tmp117Sensor::readByte(uint8_t& value, bool acknowledge) {
  value = 0;
  releaseLine(m_sda);
  for (uint8_t bit = 0; bit < 8; ++bit) {
    busDelay();
    releaseLine(m_scl);
    if (!waitHigh(m_scl)) return false;
    busDelay();
    value = static_cast<uint8_t>((value << 1) | digitalRead(m_sda));
    driveLow(m_scl);
    busDelay();
  }

  if (acknowledge) {
    driveLow(m_sda);
  } else {
    releaseLine(m_sda);
  }
  busDelay();
  releaseLine(m_scl);
  if (!waitHigh(m_scl)) return false;
  busDelay();
  driveLow(m_scl);
  busDelay();
  releaseLine(m_sda);
  return true;
}

bool Tmp117Sensor::readSoftwareRegister(uint8_t registerAddress, uint16_t& value) {
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    uint8_t msb = 0;
    uint8_t lsb = 0;
    const bool success =
        startCondition() &&
        writeByte(static_cast<uint8_t>(TMP117_ADDRESS << 1)) &&
        writeByte(registerAddress) &&
        startCondition() &&
        writeByte(static_cast<uint8_t>((TMP117_ADDRESS << 1) | 1u)) &&
        readByte(msb, true) &&
        readByte(lsb, false);
    const bool stopped = stopCondition();
    if (success && stopped) {
      value = static_cast<uint16_t>((static_cast<uint16_t>(msb) << 8) | lsb);
      return true;
    }
    recoverBus();
  }
  return false;
}

bool Tmp117Sensor::writeSoftwareRegister(uint8_t registerAddress, uint16_t value) {
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    const bool success =
        startCondition() &&
        writeByte(static_cast<uint8_t>(TMP117_ADDRESS << 1)) &&
        writeByte(registerAddress) &&
        writeByte(static_cast<uint8_t>(value >> 8)) &&
        writeByte(static_cast<uint8_t>(value & 0xFFu));
    const bool stopped = stopCondition();
    if (success && stopped) return true;
    recoverBus();
  }
  return false;
}
