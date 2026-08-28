#include <Arduino.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <Ethernet.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <Wire.h>
#include <WiFi.h>

#include "tmm_v6_r0_m0_pins.h"
#include "tmm_v6_r0_m0_version.h"
#include "tmm_web_ota.h"

using namespace tmm_m0;

namespace {

String commandBuffer;
String wifiSsid;
String wifiPassword;
String otaPassword;
Preferences preferences;
DNSServer dnsServer;
WebServer webServer(80);
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr size_t WIFI_SSID_MAX_LENGTH = 32;
constexpr size_t WIFI_PASSWORD_MAX_LENGTH = 63;
constexpr size_t OTA_PASSWORD_MIN_LENGTH = 8;
constexpr size_t OTA_PASSWORD_MAX_LENGTH = 63;
constexpr uint8_t MCP23017_IODIRB = 0x01;
constexpr uint8_t MCP23017_IODIRA = 0x00;
constexpr uint8_t MCP23017_OLATA = 0x14;
constexpr uint8_t MCP23017_OLATB = 0x15;
constexpr uint8_t OLED_I2C_CANDIDATES[] = {0x3C, 0x3D};
constexpr size_t OLED_WIDTH = 128;
constexpr size_t OLED_HEIGHT = 64;
constexpr size_t OLED_BUFFER_SIZE = OLED_WIDTH * OLED_HEIGHT / 8;

bool heartbeatReady = false;
uint32_t lastHeartbeatMs = 0;
uint32_t lastWifiAttemptMs = 0;
wl_status_t lastWifiStatus = WL_NO_SHIELD;
bool oledReady = false;
uint8_t oledAddress = 0;
uint8_t oledBuffer[OLED_BUFFER_SIZE] = {};
bool otaReady = false;
uint8_t lastOtaProgress = 255;
bool qcPortalReady = false;
String qcApSsid;
bool ethernetReady = false;
IPAddress ethernetIp;
TmmWebOtaUploader webOta;

void startWifi(void);

bool mcp1WriteRegister(uint8_t registerAddress, uint8_t value) {
  Wire.beginTransmission(MCP1_I2C_ADDRESS);
  Wire.write(registerAddress);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool mcp1ReadRegister(uint8_t registerAddress, uint8_t &value) {
  Wire.beginTransmission(MCP1_I2C_ADDRESS);
  Wire.write(registerAddress);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MCP1_I2C_ADDRESS, static_cast<uint8_t>(1)) != 1) return false;
  value = Wire.read();
  return true;
}

bool pulseHeartbeat() {
  uint8_t outputLatch = 0;
  if (!mcp1ReadRegister(MCP23017_OLATB, outputLatch)) return false;
  if (!mcp1WriteRegister(MCP23017_OLATB, outputLatch | (1U << MCP1B4_BIT))) return false;
  delayMicroseconds(100);
  return mcp1WriteRegister(MCP23017_OLATB, outputLatch & ~(1U << MCP1B4_BIT));
}

bool initializeHeartbeat() {
  uint8_t direction = 0;
  uint8_t outputLatch = 0;
  if (!mcp1ReadRegister(MCP23017_IODIRB, direction)) return false;
  if (!mcp1ReadRegister(MCP23017_OLATB, outputLatch)) return false;

  // Establish the inactive level before enabling B4 as an output.
  if (!mcp1WriteRegister(MCP23017_OLATB, outputLatch & ~(1U << MCP1B4_BIT))) return false;
  if (!mcp1WriteRegister(MCP23017_IODIRB, direction & ~(1U << MCP1B4_BIT))) return false;

  lastHeartbeatMs = millis();
  return pulseHeartbeat();
}

void serviceHeartbeat() {
  const uint32_t now = millis();
  if (!heartbeatReady) {
    heartbeatReady = initializeHeartbeat();
    lastHeartbeatMs = now;
    return;
  }
  if (now - lastHeartbeatMs < HEARTBEAT_INTERVAL_MS) return;
  heartbeatReady = pulseHeartbeat();
  lastHeartbeatMs = now;
}

bool i2cResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void printProfile() {
  Serial.printf(
    "{\"profile\":\"TMM_V6_R0_M0\",\"version\":\"%s\",\"mode\":\"bring-up-only\",\"controller\":\"ESP32-S3\",\"productionReady\":false}\n",
    FIRMWARE_VERSION);
}

void scanI2c() {
  Serial.print(F("{\"i2c\":["));
  bool first = true;
  for (uint8_t address = 1; address < 127; ++address) {
    if ((address & 0x07) == 0) serviceHeartbeat();
    if (!i2cResponds(address)) continue;
    if (!first) Serial.print(',');
    Serial.printf("\"0x%02X\"", address);
    first = false;
  }
  Serial.println(F("]}"));
}

void checkExpectedI2c() {
  Serial.print(F("{\"expectedI2c\":["));
  for (size_t index = 0; index < sizeof(EXPECTED_I2C_ADDRESSES); ++index) {
    serviceHeartbeat();
    const uint8_t address = EXPECTED_I2C_ADDRESSES[index];
    if (index) Serial.print(',');
    Serial.printf("{\"address\":\"0x%02X\",\"present\":%s}", address, i2cResponds(address) ? "true" : "false");
  }
  Serial.println(F("]}"));
}

const uint8_t *oledGlyph(char character) {
  static constexpr uint8_t BLANK[] = {0x00, 0x00, 0x00, 0x00, 0x00};
  static constexpr uint8_t COLON[] = {0x00, 0x36, 0x36, 0x00, 0x00};
  static constexpr uint8_t DOT[] = {0x00, 0x60, 0x60, 0x00, 0x00};
  static constexpr uint8_t DIGITS[][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}
  };
  static constexpr uint8_t A[] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
  static constexpr uint8_t E[] = {0x7F, 0x49, 0x49, 0x49, 0x41};
  static constexpr uint8_t F[] = {0x7F, 0x09, 0x09, 0x09, 0x01};
  static constexpr uint8_t H[] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
  static constexpr uint8_t I[] = {0x00, 0x41, 0x7F, 0x41, 0x00};
  static constexpr uint8_t M[] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
  static constexpr uint8_t P[] = {0x7F, 0x09, 0x09, 0x09, 0x06};
  static constexpr uint8_t S[] = {0x46, 0x49, 0x49, 0x49, 0x31};
  static constexpr uint8_t T[] = {0x01, 0x01, 0x7F, 0x01, 0x01};
  static constexpr uint8_t W[] = {0x7F, 0x20, 0x18, 0x20, 0x7F};
  if (character >= '0' && character <= '9') return DIGITS[character - '0'];
  if (character == ':') return COLON;
  if (character == '.') return DOT;
  if (character == 'A') return A;
  if (character == 'E') return E;
  if (character == 'F') return F;
  if (character == 'H') return H;
  if (character == 'I') return I;
  if (character == 'M') return M;
  if (character == 'P') return P;
  if (character == 'S') return S;
  if (character == 'T') return T;
  if (character == 'W') return W;
  return BLANK;
}

void oledDrawPixel(size_t x, size_t y) {
  if (x >= OLED_WIDTH || y >= OLED_HEIGHT) return;
  oledBuffer[x + (y / 8) * OLED_WIDTH] |= 1U << (y & 0x07);
}

void oledDrawText(size_t x, size_t y, const String &text) {
  for (size_t index = 0; index < text.length() && x + 5 < OLED_WIDTH; ++index, x += 6) {
    const uint8_t *glyph = oledGlyph(text[index]);
    for (size_t column = 0; column < 5; ++column) {
      for (size_t row = 0; row < 7; ++row) {
        if (glyph[column] & (1U << row)) oledDrawPixel(x + column, y + row);
      }
    }
  }
}

bool oledWriteCommands(const uint8_t *commands, size_t count) {
  Wire.beginTransmission(oledAddress);
  Wire.write(0x00);
  Wire.write(commands, count);
  return Wire.endTransmission() == 0;
}

bool oledFlush() {
  const uint8_t addressWindow[] = {0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
  if (!oledWriteCommands(addressWindow, sizeof(addressWindow))) return false;
  for (size_t offset = 0; offset < OLED_BUFFER_SIZE; offset += 16) {
    Wire.beginTransmission(oledAddress);
    Wire.write(0x40);
    Wire.write(oledBuffer + offset, 16);
    if (Wire.endTransmission() != 0) return false;
    if ((offset & 0x7F) == 0) serviceHeartbeat();
  }
  return true;
}

void renderOledStatus() {
  if (!oledReady) return;
  memset(oledBuffer, 0, sizeof(oledBuffer));
  oledDrawText(0, 0, F("TMM M0"));
  if (ethernetReady) {
    oledDrawText(0, 16, F("ETH IP:"));
    oledDrawText(0, 32, ethernetIp.toString());
  } else if (!wifiSsid.length()) {
    oledDrawText(0, 16, F("AP:"));
    oledDrawText(0, 32, WiFi.softAPIP().toString());
  } else if (WiFi.status() != WL_CONNECTED) {
    oledDrawText(0, 16, F("WIFI WAIT"));
    oledDrawText(0, 32, WiFi.softAPIP().toString());
  } else {
    oledDrawText(0, 16, F("IP:"));
    oledDrawText(0, 32, WiFi.localIP().toString());
  }
  oledReady = oledFlush();
}

bool initializeOled() {
  for (uint8_t candidate : OLED_I2C_CANDIDATES) {
    serviceHeartbeat();
    if (!i2cResponds(candidate)) continue;
    oledAddress = candidate;
    break;
  }
  if (!oledAddress) {
    Serial.println(F("{\"oled\":{\"detected\":false}}"));
    return false;
  }
  const uint8_t initialization[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
    0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
    0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
  };
  oledReady = oledWriteCommands(initialization, sizeof(initialization));
  Serial.printf("{\"oled\":{\"detected\":true,\"ready\":%s,\"address\":\"0x%02X\"}}\n", oledReady ? "true" : "false", oledAddress);
  renderOledStatus();
  return oledReady;
}

void printGpioSnapshot() {
  Serial.printf(
    "{\"gpio\":{\"loraAck\":%d,\"loraAux\":%d,\"loraLink\":%d,\"loraM0\":%d,\"loraM1\":%d,"
    "\"selectorDmm\":%d,\"selectorMma\":%d,\"selectorWebserver\":%d,\"changeDisplay\":%d,\"boot\":%d,\"ethernetInt\":%d}}\n",
    digitalRead(LORA_ACK), digitalRead(LORA_AUX), digitalRead(LORA_LINK), digitalRead(LORA_M0), digitalRead(LORA_M1),
    digitalRead(SELECTOR_DMM), digitalRead(SELECTOR_MMA), digitalRead(SELECTOR_WEBSERVER),
    digitalRead(CHANGE_DISPLAY), digitalRead(BOOT_BUTTON), digitalRead(ETH_INT));
}

void printWifiStatus() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  Serial.printf(
    "{\"wifi\":{\"provisioned\":%s,\"connected\":%s",
    wifiSsid.length() ? "true" : "false", connected ? "true" : "false");
  if (connected) {
    Serial.printf(",\"ip\":\"%s\",\"rssi\":%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  Serial.println(F("}}"));
  renderOledStatus();
}

bool loadWifiConfiguration() {
  if (!preferences.begin("tmm-wifi", true)) return false;
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  preferences.end();
  return wifiSsid.length() > 0;
}

bool saveWifiConfiguration(const String &ssid, const String &password) {
  serviceHeartbeat();
  if (!preferences.begin("tmm-wifi", false)) return false;
  const bool saved = preferences.putString("ssid", ssid) == ssid.length()
    && preferences.putString("password", password) == password.length();
  preferences.end();
  serviceHeartbeat();
  if (!saved) return false;
  wifiSsid = ssid;
  wifiPassword = password;
  return true;
}

bool clearWifiConfiguration() {
  serviceHeartbeat();
  if (!preferences.begin("tmm-wifi", false)) return false;
  const bool cleared = preferences.clear();
  preferences.end();
  serviceHeartbeat();
  if (!cleared) return false;
  wifiSsid = "";
  wifiPassword = "";
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  return true;
}

bool loadOtaConfiguration() {
  if (!preferences.begin("tmm-ota", true)) return false;
  otaPassword = preferences.getString("password", "");
  preferences.end();
  return otaPassword.length() >= OTA_PASSWORD_MIN_LENGTH;
}

bool saveOtaConfiguration(const String &password) {
  serviceHeartbeat();
  if (!preferences.begin("tmm-ota", false)) return false;
  const bool saved = preferences.putString("password", password) == password.length();
  preferences.end();
  serviceHeartbeat();
  if (!saved) return false;
  otaPassword = password;
  return true;
}

bool clearOtaConfiguration() {
  serviceHeartbeat();
  if (!preferences.begin("tmm-ota", false)) return false;
  const bool cleared = preferences.clear();
  preferences.end();
  serviceHeartbeat();
  if (!cleared) return false;
  if (otaReady) ArduinoOTA.end();
  otaPassword = "";
  otaReady = false;
  return true;
}

void startOtaIfReady() {
  if (otaReady || WiFi.status() != WL_CONNECTED || otaPassword.length() < OTA_PASSWORD_MIN_LENGTH) return;
  ArduinoOTA.setHostname("tmm-v6-r0-m0");
  ArduinoOTA.setPassword(otaPassword.c_str());
  ArduinoOTA.onStart([]() {
    lastOtaProgress = 255;
    serviceHeartbeat();
    Serial.println(F("{\"ota\":{\"event\":\"start\"}}"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    serviceHeartbeat();
    const uint8_t percent = total ? static_cast<uint8_t>((progress * 100U) / total) : 0;
    if (percent / 10 == lastOtaProgress / 10) return;
    lastOtaProgress = percent;
    Serial.printf("{\"ota\":{\"event\":\"progress\",\"percent\":%u}}\n", percent);
  });
  ArduinoOTA.onEnd([]() {
    serviceHeartbeat();
    Serial.println(F("{\"ota\":{\"event\":\"complete\"}}"));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    serviceHeartbeat();
    Serial.printf("{\"ota\":{\"event\":\"error\",\"code\":%u}}\n", error);
  });
  ArduinoOTA.begin();
  otaReady = true;
  Serial.printf("{\"ota\":{\"ready\":true,\"hostname\":\"tmm-v6-r0-m0\",\"ip\":\"%s\"}}\n", WiFi.localIP().toString().c_str());
}

void serviceOta() {
  startOtaIfReady();
  if (!otaReady) return;
  serviceHeartbeat();
  ArduinoOTA.handle();
  serviceHeartbeat();
}

// Drive polarity of the LED2..LED10 outputs. Live bench evidence (v0.1.0 QC
// session, 2026-08-28): selecting LED9 as the single active output drove LED9
// OFF while LED2..LED8 and LED10 all lit, so the mapped MCP1 latch bits light
// their LEDs when driven LOW. The polarity is therefore an explicit
// ACTIVE_LOW drive, not the earlier snapshot-derived guess: OFF is
// HIGH on every LED bit and the active level is exactly one bit LOW.
enum class LedPolarity : uint8_t {
  ACTIVE_LOW
};

constexpr int LED_ON_LEVEL = LOW;
constexpr int LED_OFF_LEVEL = HIGH;
constexpr int LED_TEST_FIRST_LED = 2;
constexpr int LED_TEST_LAST_LED = 10;
constexpr uint32_t LED_TEST_STEP_INTERVAL_MS = 300;
constexpr uint8_t LED_TEST_MASK_PORTA = 0xFFU;  // GPA0..GPA7 = LED2..LED9
constexpr uint8_t LED_TEST_MASK_PORTB = 0x01U;  // GPB0 = LED10; GPB1..GPB7 are untouched

struct LedTestMapping {
  uint8_t ledNumber;
  uint8_t directionRegister;
  uint8_t latchRegister;
  uint8_t mask;
  LedPolarity polarity;
};

// LED2..LED9 sit on MCP1 GPA0..GPA7 and LED10 on GPB0.
constexpr LedTestMapping LED_TEST_MAPPINGS[] = {
  {2, MCP23017_IODIRA, MCP23017_OLATA, 0x01U << 0, LedPolarity::ACTIVE_LOW},
  {3, MCP23017_IODIRA, MCP23017_OLATA, 0x01U << 1, LedPolarity::ACTIVE_LOW},
  {4, MCP23017_IODIRA, MCP23017_OLATA, 0x01U << 2, LedPolarity::ACTIVE_LOW},
  {5, MCP23017_IODIRA, MCP23017_OLATA, 0x01U << 3, LedPolarity::ACTIVE_LOW},
  {6, MCP23017_IODIRA, MCP23017_OLATA, 0x01U << 4, LedPolarity::ACTIVE_LOW},
  {7, MCP23017_IODIRA, MCP23017_OLATA, 0x01U << 5, LedPolarity::ACTIVE_LOW},
  {8, MCP23017_IODIRA, MCP23017_OLATA, 0x01U << 6, LedPolarity::ACTIVE_LOW},
  {9, MCP23017_IODIRA, MCP23017_OLATA, 0x01U << 7, LedPolarity::ACTIVE_LOW},
  {10, MCP23017_IODIRB, MCP23017_OLATB, 0x01U << 0, LedPolarity::ACTIVE_LOW},
};

enum class LedMode : uint8_t { IDLE, SEQUENCE, MANUAL };

const char *ledModeName(LedMode mode) {
  switch (mode) {
    case LedMode::IDLE: return "idle";
    case LedMode::SEQUENCE: return "sequence";
    case LedMode::MANUAL: return "manual";
  }
  return "idle";
}

struct LedTestState {
  LedMode mode = LedMode::IDLE;
  // True only while this module owns the LED direction/latch bits, so a failed
  // start can never restore bits it never snapshotted.
  bool outputsConfigured = false;
  int currentLed = -1;  // the single lit LED while active; -1 = none lit
  uint16_t cyclesCompleted = 0;
  uint32_t lastStepMs = 0;
  // Register values read at START, before any write. Every restoration path
  // derives from these snapshots.
  uint8_t savedDirectionA = 0;
  uint8_t savedDirectionB = 0;
  uint8_t savedLatchA = 0;
  uint8_t savedLatchB = 0;
  String lastError;
};
LedTestState ledTest;

// Drive all LED-owned latch bits at once: `litLed` is the single LED that must
// be ON (active-low: its bit goes LOW) and every other LED bit goes HIGH (OFF).
// Each port byte is read fresh and rewritten masked, so unrelated bits -
// notably the GPB4 heartbeat line and GPB1..GPB7 - survive every access even
// though the heartbeat module rewrites GPB4 between calls. Applying the whole
// level set per call also makes every sequence step and manual click atomic:
// two LEDs can never be lit at the same time by this module.
bool ledTestApplyLevels(int litLed) {
  uint8_t levelA = LED_TEST_MASK_PORTA;  // all PORTA LED bits OFF (HIGH)
  uint8_t levelB = LED_TEST_MASK_PORTB;  // GPB0 OFF (HIGH)
  if (litLed >= LED_TEST_FIRST_LED && litLed <= LED_TEST_LAST_LED) {
    if (litLed <= 9) levelA &= ~(0x01U << (litLed - 2));
    else levelB &= ~0x01U;
  }
  uint8_t latchA = 0, latchB = 0;
  if (!mcp1ReadRegister(MCP23017_OLATA, latchA)
      || !mcp1ReadRegister(MCP23017_OLATB, latchB)) {
    return false;
  }
  return mcp1WriteRegister(MCP23017_OLATA, (latchA & ~LED_TEST_MASK_PORTA) | levelA)
    && mcp1WriteRegister(MCP23017_OLATB, (latchB & ~LED_TEST_MASK_PORTB) | levelB);
}

// Park the LED latches at the OFF (HIGH) level first, then enable only the
// LED-owned direction bits as outputs, so no LED can light while directions
// change. GPB1..GPB7 (including the GPB4 heartbeat line) are never touched.
bool ledTestConfigureOutputs() {
  if (!ledTestApplyLevels(-1)) return false;
  uint8_t directionA = 0, directionB = 0;
  if (!mcp1ReadRegister(MCP23017_IODIRA, directionA)
      || !mcp1ReadRegister(MCP23017_IODIRB, directionB)) {
    return false;
  }
  return mcp1WriteRegister(MCP23017_IODIRA, directionA & ~LED_TEST_MASK_PORTA)
    && mcp1WriteRegister(MCP23017_IODIRB, directionB & ~LED_TEST_MASK_PORTB);
}

// Restore only the LED-owned direction and latch bits to their START snapshots.
bool ledTestRestoreLedBits() {
  uint8_t directionA = 0, directionB = 0, latchA = 0, latchB = 0;
  if (!mcp1ReadRegister(MCP23017_IODIRA, directionA)
      || !mcp1ReadRegister(MCP23017_IODIRB, directionB)
      || !mcp1ReadRegister(MCP23017_OLATA, latchA)
      || !mcp1ReadRegister(MCP23017_OLATB, latchB)) {
    return false;
  }
  const uint8_t restoredDirectionA = (directionA & ~LED_TEST_MASK_PORTA) | (ledTest.savedDirectionA & LED_TEST_MASK_PORTA);
  const uint8_t restoredDirectionB = (directionB & ~LED_TEST_MASK_PORTB) | (ledTest.savedDirectionB & LED_TEST_MASK_PORTB);
  const uint8_t restoredLatchA = (latchA & ~LED_TEST_MASK_PORTA) | (ledTest.savedLatchA & LED_TEST_MASK_PORTA);
  const uint8_t restoredLatchB = (latchB & ~LED_TEST_MASK_PORTB) | (ledTest.savedLatchB & LED_TEST_MASK_PORTB);
  return mcp1WriteRegister(MCP23017_IODIRA, restoredDirectionA)
    && mcp1WriteRegister(MCP23017_IODIRB, restoredDirectionB)
    && mcp1WriteRegister(MCP23017_OLATA, restoredLatchA)
    && mcp1WriteRegister(MCP23017_OLATB, restoredLatchB);
}

void failLedTest(const __FlashStringHelper *error) {
  ledTest.mode = LedMode::IDLE;
  ledTest.currentLed = -1;
  ledTest.outputsConfigured = false;
  ledTest.lastError = error;
  // Best-effort masked restoration from the START snapshot; a failed
  // restoration replaces the reported error so it is never lost.
  if (!ledTestRestoreLedBits()) ledTest.lastError = F("mcp1_restore_failed");
}

// Shared entry for sequence and manual modes: snapshot the LED-owned direction
// and latch registers before any write, then park the latches OFF and enable
// the LED bits as outputs. On any failure the snapshot restore runs.
bool ledTestBegin() {
  uint8_t directionA = 0, directionB = 0, latchA = 0, latchB = 0;
  if (!mcp1ReadRegister(MCP23017_IODIRA, directionA)
      || !mcp1ReadRegister(MCP23017_IODIRB, directionB)
      || !mcp1ReadRegister(MCP23017_OLATA, latchA)
      || !mcp1ReadRegister(MCP23017_OLATB, latchB)) {
    ledTest.lastError = F("mcp1_unavailable");
    return false;
  }
  // Snapshot fields are assigned immediately after the four successful reads
  // and before any write, so every failure path below can restore from them.
  ledTest.savedDirectionA = directionA;
  ledTest.savedDirectionB = directionB;
  ledTest.savedLatchA = latchA;
  ledTest.savedLatchB = latchB;
  if (!ledTestConfigureOutputs()) {
    ledTest.lastError = F("mcp1_unavailable");
    if (!ledTestRestoreLedBits()) ledTest.lastError = F("mcp1_restore_failed");
    return false;
  }
  ledTest.outputsConfigured = true;
  return true;
}

// Quiesce and restore. Returns false when any step failed; `lastError` then
// carries why, with "mcp1_restore_failed" reserved for failed restoration.
// On full success `lastError` is cleared.
bool stopLedTest() {
  bool stopped = true;
  if (ledTest.outputsConfigured) {
    // Every LED bit to its OFF (HIGH) level before the directions return to
    // their snapshot, so handing the pins back can never leave an LED lit.
    if (!ledTestApplyLevels(-1)) {
      ledTest.lastError = F("mcp1_unavailable");
      stopped = false;
    }
    if (!ledTestRestoreLedBits()) {
      ledTest.lastError = F("mcp1_restore_failed");
      stopped = false;
    }
  }
  ledTest.outputsConfigured = false;
  ledTest.mode = LedMode::IDLE;
  ledTest.currentLed = -1;
  if (stopped) ledTest.lastError = "";
  return stopped;
}

bool startLedTest() {
  ledTest.mode = LedMode::IDLE;
  ledTest.currentLed = -1;
  if (!ledTestBegin()) return false;
  ledTest.cyclesCompleted = 0;
  ledTest.lastError = "";
  ledTest.mode = LedMode::SEQUENCE;
  ledTest.currentLed = LED_TEST_FIRST_LED;
  ledTest.lastStepMs = millis();
  if (!ledTestApplyLevels(LED_TEST_FIRST_LED)) {
    failLedTest(F("mcp1_unavailable"));
    return false;
  }
  return true;
}

// Manual mode: light exactly the requested LED and nothing else. Switching
// from idle or from a running sequence is safe - the outputs are already
// configured, and if they are not (fresh boot) the snapshot/enable path runs
// first. A manual click therefore also doubles as the operator's way to stop a
// sequence and park a single LED for visual inspection.
String manualLedJson(int ledNumber) {
  if (ledNumber < LED_TEST_FIRST_LED || ledNumber > LED_TEST_LAST_LED) {
    return F("{\"ok\":false,\"error\":\"invalid_led_number\"}");
  }
  if (ledTest.mode == LedMode::IDLE && !ledTest.outputsConfigured) {
    if (!ledTestBegin()) {
      return String(F("{\"ok\":false,\"error\":\"")) + ledTest.lastError + F("\"}");
    }
  }
  if (!ledTestApplyLevels(ledNumber)) {
    failLedTest(F("mcp1_unavailable"));
    return F("{\"ok\":false,\"error\":\"mcp1_unavailable\"}");
  }
  ledTest.mode = LedMode::MANUAL;
  ledTest.currentLed = ledNumber;
  ledTest.lastError = "";
  String json = F("{\"ok\":true,\"mode\":\"manual\",\"litLed\":");
  json += ledNumber;
  json += F("}");
  return json;
}

String manualLedsOffJson() {
  if (ledTest.mode == LedMode::IDLE && !ledTest.outputsConfigured) {
    if (!ledTestBegin()) {
      return String(F("{\"ok\":false,\"error\":\"")) + ledTest.lastError + F("\"}");
    }
  }
  if (!ledTestApplyLevels(-1)) {
    failLedTest(F("mcp1_unavailable"));
    return F("{\"ok\":false,\"error\":\"mcp1_unavailable\"}");
  }
  ledTest.mode = LedMode::MANUAL;
  ledTest.currentLed = -1;
  ledTest.lastError = "";
  return F("{\"ok\":true,\"mode\":\"manual\",\"litLed\":null}");
}

String startLedTestJson() {
  // A start while any mode is active must restart, not report alreadyRunning:
  // stop/restore the current pass safely, retake a fresh snapshot, resume at LED2.
  const bool restarted = ledTest.mode != LedMode::IDLE;
  if (restarted && !stopLedTest()) {
    return String(F("{\"ok\":false,\"restartFailed\":true,\"error\":\"")) + ledTest.lastError + F("\"}");
  }
  if (!startLedTest()) {
    return String(F("{\"ok\":false,\"error\":\"")) + ledTest.lastError + F("\"}");
  }
  return String(F("{\"ok\":true,\"restarted\":")) + (restarted ? F("true") : F("false"))
    + F(",\"mode\":\"sequence\",\"polarity\":\"active-low\",\"sequencedLeds\":[2,3,4,5,6,7,8,9,10],\"intervalMs\":")
    + LED_TEST_STEP_INTERVAL_MS + F("}");
}

String stopLedTestJson() {
  const bool wasRunning = ledTest.mode != LedMode::IDLE;
  const uint16_t cyclesCompleted = ledTest.cyclesCompleted;
  if (!wasRunning) {
    ledTest.lastError = "";
    return String(F("{\"ok\":true,\"stopped\":false,\"cyclesCompleted\":")) + cyclesCompleted + F("}");
  }
  if (!stopLedTest()) {
    return String(F("{\"ok\":false,\"error\":\"")) + ledTest.lastError + F("\"}");
  }
  return String(F("{\"ok\":true,\"stopped\":true,\"cyclesCompleted\":")) + cyclesCompleted + F("}");
}

void serviceLedTest() {
  if (ledTest.mode != LedMode::SEQUENCE) return;
  const uint32_t now = millis();
  if (now - ledTest.lastStepMs < LED_TEST_STEP_INTERVAL_MS) return;
  ledTest.lastStepMs = now;

  int nextLed = ledTest.currentLed + 1;
  if (nextLed > LED_TEST_LAST_LED) {
    nextLed = LED_TEST_FIRST_LED;
    ++ledTest.cyclesCompleted;
  }
  // One atomic level application per step: every LED bit goes OFF and exactly
  // the next LED's bit goes LOW, so two LEDs never overlap.
  if (!ledTestApplyLevels(nextLed)) {
    failLedTest(F("mcp1_unavailable"));
    return;
  }
  ledTest.currentLed = nextLed;
}

// ---- Buttons ----------------------------------------------------------------
// BOOT (GPIO0) and CHANGE DISPLAY (GPIO42) have no verified electrical active
// polarity, so the boot-time stable level of each input is adopted as its idle
// level instead of inventing an active-high assumption. Each input is tracked
// independently: a raw transition must hold for BUTTON_DEBOUNCE_MS before it
// moves the stable level, so noise shorter than the window never registers and
// a press is confirmed as a full cycle only once its release has debounced.
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;

struct ButtonTracker {
  uint8_t pin;
  int idleLevel = HIGH;
  int rawLevel = HIGH;
  int stableLevel = HIGH;
  bool pressed = false;
  uint32_t pressCount = 0;
  bool fullCycleConfirmed = false;
  const char *lastEvent = "idle";
  uint32_t lastEventMs = 0;
  uint32_t armedSinceMs = 0;
  int pendingLevel = HIGH;
  uint32_t pendingSinceMs = 0;
  bool departedFromIdle = false;
};

ButtonTracker bootButton{static_cast<uint8_t>(BOOT_BUTTON)};
ButtonTracker changeDisplayButton{static_cast<uint8_t>(CHANGE_DISPLAY)};

void initializeButtonTracker(ButtonTracker &button) {
  const uint32_t now = millis();
  const int level = digitalRead(button.pin);
  button.idleLevel = level;
  button.rawLevel = level;
  button.stableLevel = level;
  button.pendingLevel = level;
  button.pendingSinceMs = now;
  button.armedSinceMs = now;
}

void serviceButtonTracker(ButtonTracker &button) {
  const uint32_t now = millis();
  const int level = digitalRead(button.pin);
  button.rawLevel = level;
  if (level != button.pendingLevel) {
    button.pendingLevel = level;
    button.pendingSinceMs = now;
  }
  if (button.stableLevel == button.pendingLevel) return;
  if (now - button.pendingSinceMs < BUTTON_DEBOUNCE_MS) return;

  button.stableLevel = button.pendingLevel;
  button.lastEventMs = now;
  if (button.stableLevel == button.idleLevel) {
    // Release confirmed only here. It completes a press cycle only when the
    // stable level had actually departed from idle before; a count without
    // that departure would credit noise that never left the idle level.
    button.pressed = false;
    button.lastEvent = "released";
    if (button.departedFromIdle) {
      ++button.pressCount;
      button.fullCycleConfirmed = true;
      button.departedFromIdle = false;
    }
    return;
  }
  button.pressed = true;
  button.lastEvent = "pressed";
  button.departedFromIdle = true;
}

void serviceButtons() {
  serviceButtonTracker(bootButton);
  serviceButtonTracker(changeDisplayButton);
}

void armButtonTracker(ButtonTracker &button) {
  // Reset this button's evidence only. The idle level stays exactly as the
  // boot-time baseline established it: re-baselining to the instantaneous raw
  // level here would reverse the button's semantics whenever the operator
  // arms while holding the button, because the held level would become idle.
  const uint32_t now = millis();
  const int level = digitalRead(button.pin);
  button.rawLevel = level;
  button.stableLevel = level;
  button.pendingLevel = level;
  button.pendingSinceMs = now;
  button.pressCount = 0;
  button.fullCycleConfirmed = false;
  button.pressed = level != button.idleLevel;
  if (button.pressed) {
    // Armed while held: the input has already departed from idle, so the next
    // debounced return to idle completes exactly one full press cycle.
    button.departedFromIdle = true;
    button.lastEvent = "pressed";
  } else {
    button.departedFromIdle = false;
    button.lastEvent = "idle";
  }
  button.lastEventMs = now;
  button.armedSinceMs = now;
}

void appendButtonStatus(String &json, const ButtonTracker &button) {
  json += F("{\"pin\":");
  json += button.pin;
  json += F(",\"idleLevel\":");
  json += button.idleLevel;
  json += F(",\"rawLevel\":");
  json += button.rawLevel;
  json += F(",\"stableLevel\":");
  json += button.stableLevel;
  json += F(",\"pressed\":");
  json += button.pressed ? F("true") : F("false");
  json += F(",\"pressCount\":");
  json += button.pressCount;
  json += F(",\"fullCycleConfirmed\":");
  json += button.fullCycleConfirmed ? F("true") : F("false");
  json += F(",\"lastEvent\":\"");
  json += button.lastEvent;
  json += F("\",\"lastEventMs\":");
  json += button.lastEventMs;
  json += F(",\"armedSinceMs\":");
  json += button.armedSinceMs;
  json += F("}");
}

// ---- AHT10 ------------------------------------------------------------------
// Nonblocking sampler for the AHT10 at 0x38 following the Aosong datasheet
// power-up sequence: settle >=100 ms after VDD, soft reset (0xBA), status read
// through register 0x71, then the calibration/init command (0xE1 0x08 0x00)
// only when the calibrated status bit (bit3) is clear, and triggered
// measurements (0xAC 0x33 0x00) polled through the busy bit inside a bounded
// window. Every wait is bounded by a millis() deadline and every transaction
// result is recorded, so /api/status can distinguish a probe NACK from a
// reset, calibration, trigger or read failure. No value is ever fabricated:
// temperature and humidity come only from a completed, physically plausible
// conversion, and the last valid sample is retained (with its staleness)
// across failures.
constexpr uint8_t AHT10_I2C_ADDRESS = 0x38;
constexpr uint8_t AHT10_STATUS_REGISTER = 0x71;
constexpr uint8_t AHT10_STATUS_BUSY_BIT = 0x80;
constexpr uint8_t AHT10_STATUS_CALIBRATED_BIT = 0x08;
constexpr uint8_t AHT10_RESET_COMMAND[] = {0xBA};
constexpr uint8_t AHT10_CALIBRATE_COMMAND[] = {0xE1, 0x08, 0x00};
constexpr uint8_t AHT10_MEASURE_COMMAND[] = {0xAC, 0x33, 0x00};
constexpr uint32_t AHT10_POWERUP_DELAY_MS = 100;
constexpr uint32_t AHT10_SAMPLE_INTERVAL_MS = 2000;
constexpr uint32_t AHT10_RESET_DELAY_MS = 20;
constexpr uint32_t AHT10_CALIBRATE_DELAY_MS = 20;
constexpr uint32_t AHT10_MEASURE_DELAY_MS = 80;
constexpr uint32_t AHT10_BUSY_WINDOW_MS = 100;
constexpr uint32_t AHT10_POLL_INTERVAL_MS = 5;
constexpr uint32_t AHT10_STALE_AFTER_MS = 5000;
// Physically representable AHT10 output ranges.
constexpr float AHT10_TEMPERATURE_MIN_C = -40.0F;
constexpr float AHT10_TEMPERATURE_MAX_C = 85.0F;

enum class Aht10Stage : uint8_t {
  CYCLE_START, PROBE, WAIT_RESET, CHECK_CALIBRATION, WAIT_CALIBRATION,
  TRIGGER, WAIT_MEASURE, POLL_READ
};

const char *aht10StageName(Aht10Stage stage) {
  switch (stage) {
    case Aht10Stage::CYCLE_START: return "idle";
    case Aht10Stage::PROBE: return "probe";
    case Aht10Stage::WAIT_RESET: return "wait_reset";
    case Aht10Stage::CHECK_CALIBRATION: return "check_calibration";
    case Aht10Stage::WAIT_CALIBRATION: return "wait_calibration";
    case Aht10Stage::TRIGGER: return "trigger";
    case Aht10Stage::WAIT_MEASURE: return "measure";
    case Aht10Stage::POLL_READ: return "poll_read";
  }
  return "idle";
}

struct Aht10State {
  Aht10Stage stage = Aht10Stage::CYCLE_START;
  bool present = false;
  // True once the calibrated status bit has been observed since boot or the
  // last operator retry; calibration is retained by the sensor, so later
  // cycles skip reset/calibration and go straight to triggering.
  bool calibrationChecked = false;
  bool calibrated = false;
  bool ready = false;
  bool sampleValid = false;
  bool sampling = false;
  float temperatureC = 0.0F;
  float humidityPercent = 0.0F;
  uint8_t statusByte = 0;
  bool statusByteValid = false;
  uint32_t errorCount = 0;
  uint32_t sampleMs = 0;
  uint32_t stageStartMs = 0;
  uint32_t pollStartMs = 0;
  uint32_t lastPollMs = 0;
  uint32_t nextSampleMs = 0;
  const char *lastError = "";
};
Aht10State aht10;

bool aht10WriteCommand(const uint8_t *command, size_t length) {
  Wire.beginTransmission(AHT10_I2C_ADDRESS);
  Wire.write(command, length);
  return Wire.endTransmission() == 0;
}

// Register-pointer write followed by a one-byte read (used for the 0x71
// status register). The repeated start keeps the transaction atomic.
bool aht10ReadRegister(uint8_t registerAddress, uint8_t &value) {
  Wire.beginTransmission(AHT10_I2C_ADDRESS);
  Wire.write(registerAddress);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(AHT10_I2C_ADDRESS, static_cast<uint8_t>(1)) != 1) return false;
  value = Wire.read();
  return true;
}

// Terminal failure of the current cycle: schedule a retry, keep the last valid
// sample visible to consumers, and count the error for diagnostics.
void aht10Fail(uint32_t now, const char *error) {
  ++aht10.errorCount;
  aht10.stage = Aht10Stage::CYCLE_START;
  aht10.sampling = false;
  aht10.ready = false;
  aht10.lastError = error;
  aht10.nextSampleMs = now + AHT10_SAMPLE_INTERVAL_MS;
}

// Read the status register and refresh the cached calibrated flag. Returns
// false when the transaction itself failed.
bool aht10ReadStatus() {
  uint8_t status = 0;
  if (!aht10ReadRegister(AHT10_STATUS_REGISTER, status)) return false;
  aht10.statusByte = status;
  aht10.statusByteValid = true;
  aht10.calibrated = (status & AHT10_STATUS_CALIBRATED_BIT) != 0;
  return true;
}

void restartAht10() {
  // Schedule immediately and force the full probe/reset/calibration path, but
  // never touch sampleValid/sampleMs or the retained values: consumers keep
  // seeing the last valid sample (with its staleness) until a new cycle
  // overwrites it.
  aht10.stage = Aht10Stage::CYCLE_START;
  aht10.present = false;
  aht10.calibrationChecked = false;
  aht10.calibrated = false;
  aht10.ready = false;
  aht10.sampling = false;
  aht10.statusByte = 0;
  aht10.statusByteValid = false;
  aht10.errorCount = 0;
  aht10.lastError = "";
  aht10.nextSampleMs = millis();
}

void serviceAht10() {
  const uint32_t now = millis();
  switch (aht10.stage) {
    case Aht10Stage::CYCLE_START: {
      if (static_cast<int32_t>(now - aht10.nextSampleMs) < 0) return;
      // Datasheet power-up settle: the first bus access waits >=100 ms after
      // reset so an early probe can never mask a sensor that is merely still
      // powering up.
      if (now < AHT10_POWERUP_DELAY_MS) return;
      aht10.stage = Aht10Stage::PROBE;
      return;
    }
    case Aht10Stage::PROBE: {
      // Explicit ACK probe first: it separates "nothing acks 0x38" from a
      // later command-level failure, which the serial/I2C scan also confirms.
      if (!i2cResponds(AHT10_I2C_ADDRESS)) {
        aht10.present = false;
        aht10Fail(now, "aht10_not_found");
        return;
      }
      aht10.present = true;
      if (aht10.calibrationChecked) {
        aht10.stage = Aht10Stage::TRIGGER;
        return;
      }
      // One-time bring-up per boot or operator retry: soft reset, then verify
      // the calibrated bit before trusting any conversion.
      if (!aht10WriteCommand(AHT10_RESET_COMMAND, sizeof(AHT10_RESET_COMMAND))) {
        aht10Fail(now, "aht10_reset_failed");
        return;
      }
      aht10.stage = Aht10Stage::WAIT_RESET;
      aht10.stageStartMs = now;
      return;
    }
    case Aht10Stage::WAIT_RESET: {
      if (now - aht10.stageStartMs < AHT10_RESET_DELAY_MS) return;
      aht10.stage = Aht10Stage::CHECK_CALIBRATION;
      return;
    }
    case Aht10Stage::CHECK_CALIBRATION: {
      if (!aht10ReadStatus()) {
        aht10Fail(now, "aht10_status_read_failed");
        return;
      }
      if (aht10.calibrated) {
        aht10.calibrationChecked = true;
        aht10.stage = Aht10Stage::TRIGGER;
        return;
      }
      // Not calibrated (e.g. after a cold power-up): issue the calibration
      // command once, then re-check the bit below.
      if (!aht10WriteCommand(AHT10_CALIBRATE_COMMAND, sizeof(AHT10_CALIBRATE_COMMAND))) {
        aht10Fail(now, "aht10_calibrate_failed");
        return;
      }
      aht10.stage = Aht10Stage::WAIT_CALIBRATION;
      aht10.stageStartMs = now;
      return;
    }
    case Aht10Stage::WAIT_CALIBRATION: {
      if (now - aht10.stageStartMs < AHT10_CALIBRATE_DELAY_MS) return;
      if (!aht10ReadStatus()) {
        aht10Fail(now, "aht10_status_read_failed");
        return;
      }
      if (!aht10.calibrated) {
        aht10Fail(now, "aht10_not_calibrated");
        return;
      }
      aht10.calibrationChecked = true;
      aht10.stage = Aht10Stage::TRIGGER;
      return;
    }
    case Aht10Stage::TRIGGER: {
      if (!aht10WriteCommand(AHT10_MEASURE_COMMAND, sizeof(AHT10_MEASURE_COMMAND))) {
        aht10Fail(now, "aht10_trigger_failed");
        return;
      }
      aht10.sampling = true;
      aht10.stage = Aht10Stage::WAIT_MEASURE;
      aht10.stageStartMs = now;
      return;
    }
    case Aht10Stage::WAIT_MEASURE: {
      if (now - aht10.stageStartMs < AHT10_MEASURE_DELAY_MS) return;
      aht10.stage = Aht10Stage::POLL_READ;
      aht10.pollStartMs = now;
      aht10.lastPollMs = now;
      return;
    }
    case Aht10Stage::POLL_READ: {
      // Rate-limit the reads inside the bounded busy window so the loop does
      // not hammer the sensor with back-to-back I2C transactions while it is
      // still converting.
      if (now - aht10.lastPollMs < AHT10_POLL_INTERVAL_MS) return;
      aht10.lastPollMs = now;
      if (Wire.requestFrom(AHT10_I2C_ADDRESS, static_cast<uint8_t>(6)) != 6) {
        if (now - aht10.pollStartMs < AHT10_BUSY_WINDOW_MS) return;
        aht10Fail(now, "aht10_read_failed");
        return;
      }
      uint8_t data[6];
      for (uint8_t &value : data) value = Wire.read();
      aht10.statusByte = data[0];
      aht10.statusByteValid = true;
      if (data[0] & AHT10_STATUS_BUSY_BIT) {
        // Busy: keep polling inside the bounded window, then fail cleanly.
        if (now - aht10.pollStartMs < AHT10_BUSY_WINDOW_MS) return;
        aht10Fail(now, "aht10_busy_timeout");
        return;
      }
      const uint32_t rawHumidity = (static_cast<uint32_t>(data[1]) << 12) | (static_cast<uint32_t>(data[2]) << 4) | (data[3] >> 4);
      const uint32_t rawTemperature = (static_cast<uint32_t>(data[3] & 0x0F) << 16) | (static_cast<uint32_t>(data[4]) << 8) | data[5];
      const float humidityPercent = rawHumidity * 100.0F / 1048576.0F;
      const float temperatureC = rawTemperature * 200.0F / 1048576.0F - 50.0F;
      if (temperatureC >= AHT10_TEMPERATURE_MIN_C && temperatureC <= AHT10_TEMPERATURE_MAX_C
          && humidityPercent >= 0.0F && humidityPercent <= 100.0F) {
        aht10.temperatureC = temperatureC;
        aht10.humidityPercent = humidityPercent;
        aht10.sampleMs = now;
        aht10.sampleValid = true;
        aht10.ready = true;
        aht10.lastError = "";
      } else {
        aht10.ready = false;
        aht10.lastError = "aht10_range_unplausible";
      }
      aht10.sampling = false;
      aht10.stage = Aht10Stage::CYCLE_START;
      aht10.nextSampleMs = now + AHT10_SAMPLE_INTERVAL_MS;
      return;
    }
  }
}

void appendAht10Status(String &json, uint32_t now) {
  json += F("{\"present\":");
  json += aht10.present ? F("true") : F("false");
  json += F(",\"ready\":");
  json += aht10.ready ? F("true") : F("false");
  json += F(",\"sampleValid\":");
  json += aht10.sampleValid ? F("true") : F("false");
  json += F(",\"temperatureC\":");
  json += aht10.sampleValid ? String(aht10.temperatureC, 2) : String(F("null"));
  json += F(",\"humidityPercent\":");
  json += aht10.sampleValid ? String(aht10.humidityPercent, 2) : String(F("null"));
  json += F(",\"sampleAgeMs\":");
  if (aht10.sampleValid) json += (now - aht10.sampleMs);
  else json += F("null");
  json += F(",\"stale\":");
  json += (!aht10.sampleValid || (now - aht10.sampleMs) > AHT10_STALE_AFTER_MS) ? F("true") : F("false");
  json += F(",\"sampling\":");
  json += aht10.sampling ? F("true") : F("false");
  json += F(",\"stage\":\"");
  json += aht10StageName(aht10.stage);
  json += F("\",\"calibrated\":");
  json += aht10.calibrated ? F("true") : F("false");
  json += F(",\"statusByte\":");
  if (aht10.statusByteValid) {
    char encoded[7];
    snprintf(encoded, sizeof(encoded), "\"0x%02X\"", aht10.statusByte);
    json += encoded;
  } else {
    json += F("null");
  }
  json += F(",\"errorCount\":");
  json += aht10.errorCount;
  json += F(",\"lastError\":\"");
  json += aht10.lastError;
  json += F("\"}");
}

// ---- MicroSD QC -------------------------------------------------------------
// Staged write/readback/cleanup QC replacing the old single blocking sd-write.
// Each loop() invocation performs at most ONE stage operation, so the heartbeat
// and the QC portal stay served throughout; every slow SD library call is
// bracketed by serviceHeartbeat() and nothing delays. The mount stage is
// preceded by a low-level CMD0 probe on the shared SPI bus, so a silent card
// (no R1 response) is reported as sd_no_card while a responding card that
// still fails the filesystem mount is reported as sd_mount_failed; the card
// type reported by the SD library is kept as run evidence. The test owns
// exactly one uniquely named 8.3 file per run: pre-existing files (including
// /TMM_QC.TXT and any filename collision) are never removed.
enum class SdQcStage : uint8_t {
  IDLE, PROBING, MOUNTING, WRITING, READING, CLEANING, PASSED, FAILED
};

// Payload stays well under 128 bytes and names the build and the run nonce.
constexpr size_t SD_QC_PAYLOAD_MAX = 128;
// Collision re-rolls before the run gives up rather than touching a foreign file.
constexpr uint8_t SD_QC_NAME_ATTEMPTS = 8;
// CMD0 retries inside the probe; the whole probe stays in the low-millisecond
// range at 400 kHz while the heartbeat is serviced between retry bursts.
constexpr uint8_t SD_PROBE_CMD0_ATTEMPTS = 200;
constexpr uint32_t SD_PROBE_SPI_HZ = 400000U;

struct SdQcState {
  SdQcStage stage = SdQcStage::IDLE;
  bool running = false;
  bool cardPresent = false;
  bool mounted = false;
  bool writePassed = false;
  bool readbackPassed = false;
  bool cleanupPassed = false;
  bool testPassed = false;
  uint32_t bytesWritten = 0;
  String testFile = "";
  uint32_t startedMs = 0;
  uint32_t durationMs = 0;
  String lastError = "";
  String cardType = "";
  uint32_t nonce = 0;
  String expectedPayload = "";
  // Internal: true only when THIS run created the owned path, so cleanup and
  // failure paths can never delete a pre-existing collision.
  bool fileCreatedByRun = false;
};
SdQcState sdQc;
uint32_t sdQcRunCounter = 0;

const char *sdQcStageName(SdQcStage stage) {
  switch (stage) {
    case SdQcStage::IDLE: return "idle";
    case SdQcStage::PROBING: return "probing";
    case SdQcStage::MOUNTING: return "mounting";
    case SdQcStage::WRITING: return "writing";
    case SdQcStage::READING: return "reading";
    case SdQcStage::CLEANING: return "cleaning";
    case SdQcStage::PASSED: return "passed";
    case SdQcStage::FAILED: return "failed";
  }
  return "idle";
}

uint32_t nextSdQcNonce() {
  ++sdQcRunCounter;
  uint32_t nonce = static_cast<uint32_t>(ESP.getEfuseMac())
    ^ (millis() * 2654435761U)
    ^ (sdQcRunCounter * 0x9E3779B9U);
  nonce ^= nonce >> 16;
  return nonce;
}

String buildSdQcPayload(uint32_t nonce) {
  char payload[SD_QC_PAYLOAD_MAX];
  snprintf(payload, sizeof(payload), "TMM_V6_R0_M0 v=%s nonce=%06lX",
    FIRMWARE_VERSION, static_cast<unsigned long>(nonce & 0xFFFFFFU));
  return String(payload);
}

String sdQcPathForNonce(uint32_t nonce) {
  char path[16];
  snprintf(path, sizeof(path), "/TQ%06lX.TST", static_cast<unsigned long>(nonce & 0xFFFFFFU));
  return String(path);
}

// Release the shared bus after a failed mount/probe: CS high plus eight spare
// clocks so the card lets go of MISO before anything else (e.g. the W5500)
// claims the bus.
void sdReleaseBus() {
  SPI.beginTransaction(SPISettings(SD_PROBE_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(SD_CS, HIGH);
  digitalWrite(ETH_CS, HIGH);
  SPI.transfer(0xFF);
  SPI.transfer(0xFF);
  SPI.endTransaction();
}

// Low-level CMD0 (GO_IDLE_STATE) presence probe. A card that never drives a
// non-0xFF R1 byte back means no card, no card detect, or dead signal lines -
// evidence the SD library cannot provide through SD.begin()'s bare bool.
// Runs at a deliberately slow 400 kHz (boot-safe for every SD card) and only
// while the filesystem is NOT mounted; both chip selects are released before
// returning. serviceHeartbeat() runs between retry bursts.
bool sdProbeCardPresent() {
  SPI.beginTransaction(SPISettings(SD_PROBE_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(SD_CS, HIGH);
  digitalWrite(ETH_CS, HIGH);
  // >=74 idle clocks with CS high let the card finish its own power-up.
  for (uint8_t clock = 0; clock < 10; ++clock) SPI.transfer(0xFF);
  digitalWrite(SD_CS, LOW);
  uint8_t response = 0xFF;
  for (uint8_t attempt = 0; attempt < SD_PROBE_CMD0_ATTEMPTS; ++attempt) {
    SPI.transfer(0x40 | 0);  // CMD0, argument 0
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    SPI.transfer(0x95);  // CMD0 is the only command with a valid CRC requirement
    for (uint8_t wait = 0; wait < 8; ++wait) {
      response = SPI.transfer(0xFF);
      if (response != 0xFF) break;
    }
    if (response == 0x01) break;  // idle state reached: the card acked
    if ((attempt & 0x0FU) == 0) serviceHeartbeat();
  }
  digitalWrite(SD_CS, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  return response == 0x01;
}

// Human-readable card type from the SD library's own detection, kept as run
// evidence once the filesystem mounts. The core's enum is exactly
// CARD_NONE/CARD_MMC/CARD_SD/CARD_SDHC/CARD_UNKNOWN (sd_defines.h).
String sdCardTypeName(uint8_t type) {
  switch (type) {
    case CARD_NONE: return String(F("none"));
    case CARD_MMC: return String(F("mmc"));
    case CARD_SD: return String(F("sd1"));
    case CARD_SDHC: return String(F("sdhc"));
    default: return String(F("unknown"));
  }
}

void failSdQc(const __FlashStringHelper *error, uint32_t now) {
  // Best-effort cleanup of ONLY this run's own file; a pre-existing collision
  // (never created by this run) is never removed. Removal is verified absent so
  // cleanupPassed records what actually happened; testPassed stays false and the
  // stage stays FAILED either way, so cleanup can never mask the original error.
  if (sdQc.mounted) {
    if (sdQc.fileCreatedByRun && sdQc.testFile.length()) {
      if (SD.exists(sdQc.testFile)) {
        sdQc.cleanupPassed = SD.remove(sdQc.testFile) && !SD.exists(sdQc.testFile);
      } else {
        sdQc.cleanupPassed = true;  // nothing of this run's left on the card
      }
    }
    SD.end();
    sdQc.mounted = false;
  }
  sdReleaseBus();
  sdQc.stage = SdQcStage::FAILED;
  sdQc.running = false;
  sdQc.durationMs = now - sdQc.startedMs;
  sdQc.testPassed = false;
  sdQc.lastError = String(error);
}

// Defined with the Ethernet QC state below; forward-declared here so the SD QC
// section can check the reciprocal guard without reordering the file.
bool ethernetQcBusy();

String startSdQcJson() {
  // The shared SPI bus cannot serve both tests at once.
  if (ethernetQcBusy()) {
    return F("{\"ok\":false,\"error\":\"resource_busy\",\"busy\":\"ethernet\"}");
  }
  if (sdQc.running) {
    return F("{\"ok\":false,\"error\":\"resource_busy\",\"busy\":\"sd\"}");
  }
  // Starting after a prior terminal state resets every piece of run evidence.
  const uint32_t now = millis();
  sdQc.stage = SdQcStage::PROBING;
  sdQc.running = true;
  sdQc.cardPresent = false;
  sdQc.cardType = "";
  sdQc.mounted = false;
  sdQc.writePassed = false;
  sdQc.readbackPassed = false;
  sdQc.cleanupPassed = false;
  sdQc.testPassed = false;
  sdQc.bytesWritten = 0;
  sdQc.testFile = "";
  sdQc.startedMs = now;
  sdQc.durationMs = 0;
  sdQc.lastError = "";
  sdQc.nonce = nextSdQcNonce();
  sdQc.expectedPayload = buildSdQcPayload(sdQc.nonce);
  sdQc.fileCreatedByRun = false;
  return F("{\"ok\":true,\"action\":\"sd-start\",\"state\":\"probing\"}");
}

void serviceSdQc() {
  if (!sdQc.running) return;
  const uint32_t now = millis();
  switch (sdQc.stage) {
    case SdQcStage::IDLE:
    case SdQcStage::PASSED:
    case SdQcStage::FAILED:
      return;
    case SdQcStage::PROBING: {
      serviceHeartbeat();
      // The W5500 must stay deselected while the SD card owns the shared bus.
      sdQc.cardPresent = sdProbeCardPresent();
      serviceHeartbeat();
      if (!sdQc.cardPresent) {
        failSdQc(F("sd_no_card"), now);
        return;
      }
      sdQc.stage = SdQcStage::MOUNTING;
      return;
    }
    case SdQcStage::MOUNTING: {
      serviceHeartbeat();
      digitalWrite(ETH_CS, HIGH);
      sdQc.mounted = SD.begin(SD_CS, SPI);
      serviceHeartbeat();
      if (!sdQc.mounted) {
        // The card acked CMD0 but the filesystem mount failed: a genuine
        // mount failure, not a missing card.
        failSdQc(F("sd_mount_failed"), now);
        return;
      }
      sdQc.cardType = sdCardTypeName(SD.cardType());
      sdQc.stage = SdQcStage::WRITING;
      return;
    }
    case SdQcStage::WRITING: {
      serviceHeartbeat();
      // Choose a run-owned 8.3 name; on a collision re-roll the nonce and never
      // touch the pre-existing file.
      bool named = false;
      for (uint8_t attempt = 0; attempt < SD_QC_NAME_ATTEMPTS; ++attempt) {
        if (attempt) {
          sdQc.nonce = nextSdQcNonce();
          sdQc.expectedPayload = buildSdQcPayload(sdQc.nonce);
        }
        sdQc.testFile = sdQcPathForNonce(sdQc.nonce);
        if (!SD.exists(sdQc.testFile)) {
          named = true;
          break;
        }
      }
      if (!named) {
        failSdQc(F("sd_name_exhausted"), now);
        return;
      }
      // Create fresh: never append to an existing file.
      File file = SD.open(sdQc.testFile, FILE_WRITE);
      if (!file) {
        failSdQc(F("sd_open_write_failed"), now);
        return;
      }
      sdQc.fileCreatedByRun = true;
      const size_t written = file.print(sdQc.expectedPayload);
      file.close();
      if (written != sdQc.expectedPayload.length()) {
        failSdQc(F("sd_write_failed"), now);
        return;
      }
      sdQc.bytesWritten = written;
      sdQc.writePassed = true;
      sdQc.stage = SdQcStage::READING;
      return;
    }
    case SdQcStage::READING: {
      serviceHeartbeat();
      File file = SD.open(sdQc.testFile, FILE_READ);
      if (!file) {
        failSdQc(F("sd_open_read_failed"), now);
        return;
      }
      const uint32_t size = file.size();
      String readBack;
      bool readOk = true;
      while (readOk && file.available()) {
        const int value = file.read();
        if (value < 0) readOk = false;
        else readBack += static_cast<char>(value);
      }
      file.close();
      if (!readOk) {
        failSdQc(F("sd_read_failed"), now);
        return;
      }
      // Exact size and content, with no extra byte beyond the payload.
      if (size != sdQc.expectedPayload.length() || readBack.length() != sdQc.expectedPayload.length()) {
        failSdQc(F("sd_size_mismatch"), now);
        return;
      }
      if (readBack != sdQc.expectedPayload) {
        failSdQc(F("sd_content_mismatch"), now);
        return;
      }
      sdQc.readbackPassed = true;
      sdQc.stage = SdQcStage::CLEANING;
      return;
    }
    case SdQcStage::CLEANING: {
      serviceHeartbeat();
      // Remove ONLY the owned path, then verify it is really gone.
      bool removed = true;
      if (SD.exists(sdQc.testFile)) removed = SD.remove(sdQc.testFile);
      if (!removed) {
        failSdQc(F("sd_remove_failed"), now);
        return;
      }
      if (SD.exists(sdQc.testFile)) {
        failSdQc(F("sd_remove_unverified"), now);
        return;
      }
      SD.end();
      sdQc.mounted = false;
      // Park both chip selects high so neither device is left selected.
      digitalWrite(SD_CS, HIGH);
      digitalWrite(ETH_CS, HIGH);
      sdQc.cleanupPassed = true;
      sdQc.stage = SdQcStage::PASSED;
      sdQc.running = false;
      sdQc.durationMs = now - sdQc.startedMs;
      sdQc.testPassed = true;
      sdQc.lastError = "";
      return;
    }
  }
}

// ---- Ethernet QC (W5500) ----------------------------------------------------
// Staged QC replaces the old single blocking ethernet-test. Only the DHCP
// request blocks (bounded to <=2.5 s); every other step is polled from loop()
// so the heartbeat and the QC portal stay served throughout. Evidence fields
// (hardwareDetected, dhcpPassed, ip, startedMs, durationMs, testPassed,
// lastError, stage) are kept after pass, fail, or operator stop so the QC
// report retains the last run.
enum class EthernetQcStage : uint8_t {
  IDLE, INITIALIZING, WAITING_LINK, ACQUIRING_DHCP, PASSED, FAILED
};

constexpr uint32_t ETH_LINK_POLL_INTERVAL_MS = 250;
constexpr uint32_t ETH_LINK_TIMEOUT_MS = 10000;
constexpr uint32_t ETH_DHCP_TIMEOUT_MS = 2500;
constexpr uint32_t ETH_DHCP_RESPONSE_TIMEOUT_MS = 600;

struct EthernetQcState {
  EthernetQcStage stage = EthernetQcStage::IDLE;
  bool running = false;
  bool hardwareDetected = false;
  String link = "unknown";
  bool dhcpPassed = false;
  IPAddress ip;
  uint32_t startMs = 0;
  uint32_t stageStartMs = 0;
  uint32_t lastLinkPollMs = 0;
  uint32_t durationMs = 0;
  String error;
  bool pass = false;
  uint8_t mac[6] = {};
  bool initAttempted = false;
  bool dhcpAttempted = false;
  int dhcpResult = 0;
};
EthernetQcState ethernetQc;

bool ethernetQcBusy() {
  return ethernetQc.running;
}

const char *ethernetQcStageName(EthernetQcStage stage) {
  switch (stage) {
    case EthernetQcStage::IDLE: return "idle";
    case EthernetQcStage::INITIALIZING: return "initializing";
    case EthernetQcStage::WAITING_LINK: return "waiting_link";
    case EthernetQcStage::ACQUIRING_DHCP: return "acquiring_dhcp";
    case EthernetQcStage::PASSED: return "passed";
    case EthernetQcStage::FAILED: return "failed";
  }
  return "idle";
}

void failEthernetQc(const __FlashStringHelper *error, uint32_t now) {
  ethernetQc.stage = EthernetQcStage::FAILED;
  ethernetQc.running = false;
  ethernetQc.durationMs = now - ethernetQc.startMs;
  ethernetQc.error = String(error);
  // A failed run never claims ready, even if an earlier run passed; the cached
  // IP only survives as evidence alongside a genuine pass.
  ethernetQc.pass = false;
  ethernetReady = false;
}

String startEthernetQcJson() {
  // The shared SPI bus cannot serve both tests at once.
  if (sdQc.running) {
    return F("{\"ok\":false,\"error\":\"resource_busy\",\"busy\":\"sd\"}");
  }
  // A start while already running restarts: all evidence from the previous run
  // is cleared before the W5500 is re-initialized.
  const uint32_t now = millis();
  const uint64_t deviceId = ESP.getEfuseMac();
  // Deterministic locally administered unicast MAC (02:...) derived from the
  // ESP32 eFuse, so every run of a given board presents the same address.
  ethernetQc.mac[0] = 0x02;
  ethernetQc.mac[1] = static_cast<uint8_t>(deviceId >> 32);
  ethernetQc.mac[2] = static_cast<uint8_t>(deviceId >> 24);
  ethernetQc.mac[3] = static_cast<uint8_t>(deviceId >> 16);
  ethernetQc.mac[4] = static_cast<uint8_t>(deviceId >> 8);
  ethernetQc.mac[5] = static_cast<uint8_t>(deviceId);
  ethernetQc.stage = EthernetQcStage::INITIALIZING;
  ethernetQc.running = true;
  ethernetQc.hardwareDetected = false;
  ethernetQc.link = "unknown";
  ethernetQc.dhcpPassed = false;
  ethernetQc.ip = IPAddress(0, 0, 0, 0);
  ethernetQc.startMs = now;
  ethernetQc.stageStartMs = now;
  ethernetQc.lastLinkPollMs = now;
  ethernetQc.durationMs = 0;
  ethernetQc.error = "";
  ethernetQc.pass = false;
  ethernetQc.initAttempted = false;
  ethernetQc.dhcpAttempted = false;
  ethernetQc.dhcpResult = 0;
  ethernetReady = false;
  // The W5500 owns the bus while this test runs; park the SD card's CS high.
  digitalWrite(SD_CS, HIGH);
  Ethernet.init(ETH_CS);
  return F("{\"ok\":true,\"action\":\"ethernet-start\",\"state\":\"initializing\"}");
}

String stopEthernetQcJson() {
  const bool wasRunning = ethernetQc.running;
  if (wasRunning) {
    ethernetQc.stage = EthernetQcStage::FAILED;
    ethernetQc.running = false;
    ethernetQc.durationMs = millis() - ethernetQc.startMs;
    ethernetQc.error = F("stopped_by_operator");
    // Stopping a run that had not passed never claims ready; the cached IP is
    // retained only as evidence of a prior pass.
    ethernetQc.pass = false;
    ethernetReady = false;
  }
  digitalWrite(SD_CS, HIGH);
  digitalWrite(ETH_CS, HIGH);
  String json = F("{\"ok\":true,\"stopped\":");
  json += wasRunning ? F("true") : F("false");
  json += F(",\"stage\":\"");
  json += ethernetQcStageName(ethernetQc.stage);
  json += F("\",\"hardwareDetected\":");
  json += ethernetQc.hardwareDetected ? F("true") : F("false");
  json += F(",\"dhcpPassed\":");
  json += ethernetQc.dhcpPassed ? F("true") : F("false");
  json += F(",\"ip\":\"");
  json += ethernetQc.ip.toString();
  json += F("\",\"startedMs\":");
  json += ethernetQc.startMs;
  json += F(",\"durationMs\":");
  json += ethernetQc.durationMs;
  json += F(",\"lastError\":\"");
  json += ethernetQc.error;
  json += F("\",\"testPassed\":");
  json += ethernetQc.pass ? F("true") : F("false");
  json += F("}");
  return json;
}

void serviceEthernetQc() {
  const uint32_t now = millis();
  switch (ethernetQc.stage) {
    case EthernetQcStage::IDLE:
    case EthernetQcStage::PASSED:
    case EthernetQcStage::FAILED:
      return;
    case EthernetQcStage::INITIALIZING: {
      serviceHeartbeat();
      if (!ethernetQc.initAttempted) {
        ethernetQc.initAttempted = true;
        // Static begin with 0.0.0.0 runs the W5500 detection path without
        // starting DHCP; W5500 init carries a one-time ~560 ms internal delay.
        Ethernet.begin(ethernetQc.mac, IPAddress(0, 0, 0, 0));
        serviceHeartbeat();
      }
      // Detection must name the W5500 specifically, not merely any nonzero ID.
      if (Ethernet.hardwareStatus() != EthernetW5500) {
        failEthernetQc(F("w5500_not_found"), now);
        return;
      }
      ethernetQc.hardwareDetected = true;
      ethernetQc.stage = EthernetQcStage::WAITING_LINK;
      ethernetQc.stageStartMs = now;
      return;
    }
    case EthernetQcStage::WAITING_LINK: {
      if (now - ethernetQc.stageStartMs > ETH_LINK_TIMEOUT_MS) {
        failEthernetQc(F("ethernet_link_timeout"), now);
        return;
      }
      if (now - ethernetQc.lastLinkPollMs < ETH_LINK_POLL_INTERVAL_MS) return;
      ethernetQc.lastLinkPollMs = now;
      serviceHeartbeat();
      const EthernetLinkStatus status = Ethernet.linkStatus();
      if (status == LinkON) {
        ethernetQc.link = "on";
        ethernetQc.stage = EthernetQcStage::ACQUIRING_DHCP;
        ethernetQc.stageStartMs = now;
        return;
      }
      ethernetQc.link = status == LinkOFF ? "off" : "unknown";
      return;
    }
    case EthernetQcStage::ACQUIRING_DHCP: {
      serviceHeartbeat();
      // Timestamp captured AFTER the blocking DHCP call: `now` was sampled
      // before it and would understate the run duration by up to
      // ETH_DHCP_TIMEOUT_MS.
      uint32_t completedNow = now;
      if (!ethernetQc.dhcpAttempted) {
        ethernetQc.dhcpAttempted = true;
        // Exactly one bounded DHCP request for the whole run; this call may
        // block for up to ETH_DHCP_TIMEOUT_MS.
        ethernetQc.dhcpResult = Ethernet.begin(ethernetQc.mac, ETH_DHCP_TIMEOUT_MS, ETH_DHCP_RESPONSE_TIMEOUT_MS);
        completedNow = millis();
        serviceHeartbeat();
      }
      const IPAddress acquired = Ethernet.localIP();
      if (ethernetQc.dhcpResult == 1 && static_cast<uint32_t>(acquired) != 0 && Ethernet.linkStatus() == LinkON) {
        ethernetQc.dhcpPassed = true;
        ethernetQc.ip = acquired;
        ethernetIp = acquired;
        ethernetQc.stage = EthernetQcStage::PASSED;
        ethernetQc.running = false;
        ethernetQc.durationMs = completedNow - ethernetQc.startMs;
        ethernetQc.error = "";
        ethernetQc.pass = true;
        ethernetReady = true;
        renderOledStatus();
        return;
      }
      failEthernetQc(F("ethernet_dhcp_failed"), completedNow);
      return;
    }
  }
}

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character == '\\' || character == '"') escaped += '\\';
    escaped += character >= 0x20 ? character : '?';
  }
  return escaped;
}

String scanI2cJson() {
  String json = F("{\"ok\":true,\"addresses\":[");
  bool first = true;
  for (uint8_t address = 1; address < 127; ++address) {
    if ((address & 0x07) == 0) serviceHeartbeat();
    if (!i2cResponds(address)) continue;
    if (!first) json += ',';
    char encoded[7];
    snprintf(encoded, sizeof(encoded), "\"0x%02X\"", address);
    json += encoded;
    first = false;
  }
  json += F("]}");
  return json;
}

String gpioSnapshotJson() {
  String json;
  json.reserve(300);
  json += F("{\"ok\":true,\"gpio\":{");
  json += F("\"loraAck\":"); json += digitalRead(LORA_ACK);
  json += F(",\"loraAux\":"); json += digitalRead(LORA_AUX);
  json += F(",\"loraLink\":"); json += digitalRead(LORA_LINK);
  json += F(",\"selectorDmm\":"); json += digitalRead(SELECTOR_DMM);
  json += F(",\"selectorMma\":"); json += digitalRead(SELECTOR_MMA);
  json += F(",\"selectorWebserver\":"); json += digitalRead(SELECTOR_WEBSERVER);
  json += F(",\"changeDisplay\":"); json += digitalRead(CHANGE_DISPLAY);
  json += F(",\"boot\":"); json += digitalRead(BOOT_BUTTON);
  json += F(",\"ethernetInt\":"); json += digitalRead(ETH_INT);
  json += F("}}");
  return json;
}

// On ESP32, PROGMEM maps into the flash address space, so the portal page can
// be scanned and streamed directly without copying it into RAM. The page is
// served in bounded chunks that replace {{FIRMWARE_VERSION}} with the version
// header constant, so no second copy of the version string exists.
constexpr char FIRMWARE_VERSION_TOKEN[] = "{{FIRMWARE_VERSION}}";
constexpr size_t QC_PORTAL_CHUNK_SIZE = 256;

const char QC_PORTAL_HTML[] PROGMEM = R"QC_HTML(
<!doctype html><html lang="id"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="firmware-version" content="{{FIRMWARE_VERSION}}"><title>TMM QC Dashboard</title><style>
:root{--bg:#060c12;--panel:#0e1c27;--panel2:#0a141d;--line:#1c3547;--line2:#2a5570;--tx:#e8f2f8;--mut:#7d99ad;--acc:#2fd4c3;--acc2:#4aa8ff;--ok:#3ddc97;--warn:#ffb454;--bad:#ff6b6b;--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
*{box-sizing:border-box}
body{margin:0;padding:16px 14px 44px;color:var(--tx);font:14px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:radial-gradient(900px 420px at 85% -8%,rgba(47,212,195,.10),transparent 60%),radial-gradient(700px 380px at -10% 110%,rgba(74,168,255,.08),transparent 60%),var(--bg)}
main,.top,.strip{max-width:1240px;margin-left:auto;margin-right:auto}
.top{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:12px}
.brand{display:flex;align-items:center;gap:11px}
.logo{width:42px;height:42px;border-radius:12px;display:grid;place-items:center;font-weight:800;font-size:.8rem;letter-spacing:.5px;color:#04211e;background:linear-gradient(135deg,var(--acc),#1b8fa8);box-shadow:0 6px 18px rgba(47,212,195,.22)}
h1{font-size:1.12rem;margin:0;letter-spacing:.3px}
.sub{color:var(--mut);font-size:.76rem;font-family:var(--mono)}
.live{padding:7px 13px;border-radius:99px;font-size:.76rem;font-weight:700;font-family:var(--mono);border:1px solid var(--line2);background:var(--panel2);transition:all .25s}
.live.on{color:#06281f;background:rgba(61,220,151,.14);border-color:rgba(61,220,151,.45)}
.live.off{color:#3a1414;background:rgba(255,107,107,.12);border-color:rgba(255,107,107,.4)}
.strip{display:grid;grid-template-columns:repeat(auto-fit,minmax(158px,1fr));gap:9px;margin-bottom:13px}
.chip{display:flex;align-items:center;gap:9px;padding:9px 12px;border:1px solid var(--line);border-radius:12px;background:linear-gradient(180deg,var(--panel),var(--panel2));transition:border-color .2s}
.chip:hover{border-color:var(--line2)}
.cdot{width:9px;height:9px;border-radius:50%;background:var(--mut);flex:none;box-shadow:0 0 0 3px rgba(125,153,173,.12);transition:all .25s}
.chip[data-state=ok] .cdot{background:var(--ok);box-shadow:0 0 0 3px rgba(61,220,151,.16)}
.chip[data-state=warn] .cdot{background:var(--warn);box-shadow:0 0 0 3px rgba(255,180,84,.16)}
.chip[data-state=bad] .cdot{background:var(--bad);box-shadow:0 0 0 3px rgba(255,107,107,.16)}
.chip small{display:block;color:var(--mut);font-size:.6rem;letter-spacing:.7px;text-transform:uppercase}
.chip b{display:block;font-size:.84rem;font-family:var(--mono);line-height:1.3}
.csub{font-size:.62rem!important;text-transform:none!important;letter-spacing:0!important;opacity:.85}
.card{border:1px solid var(--line);border-radius:15px;background:linear-gradient(180deg,#0e1c27,#0b1520);padding:15px 16px;box-shadow:0 14px 34px rgba(0,0,0,.32);transition:border-color .2s,box-shadow .2s;margin-bottom:12px}
.card.next{border-color:rgba(74,168,255,.55);box-shadow:0 0 0 1px rgba(74,168,255,.22),0 14px 34px rgba(0,0,0,.32)}
.chead{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:8px}
.card h2{font-size:.95rem;margin:0}
.hint{color:var(--mut);font-size:.78rem;margin:4px 0 10px}
.warn{color:var(--warn);font-size:.78rem}
.layout{display:grid;grid-template-columns:minmax(0,1fr) 340px;gap:12px;align-items:start}
.colmain{min-width:0;display:grid;gap:12px}
.side{display:grid;gap:12px;align-self:start;position:sticky;top:12px}
.grid{display:grid;gap:12px;align-items:start}
.ledbtns{display:flex;gap:6px;flex-wrap:wrap;margin:2px 0 4px}
.ledbtns button{padding:8px 12px;min-width:40px;font-family:var(--mono)}
.ledbtns button.active{background:linear-gradient(180deg,var(--acc),#1a8f83);color:#022420;border-color:#5ef0e2;box-shadow:0 0 10px rgba(47,212,195,.45)}
.guide{margin:0;padding-left:18px;display:grid;gap:7px;font-size:.78rem}
.guide li{padding-left:2px}
.guide b{color:var(--tx)}
.gnote{font-size:.72rem;color:var(--mut);border-top:1px dashed rgba(42,85,112,.4);margin-top:10px;padding-top:9px}
@media(max-width:920px){.layout{grid-template-columns:minmax(0,1fr)}.side{position:static;order:-1}}
.statechip{font-size:.64rem;font-weight:700;letter-spacing:.6px;padding:4px 9px;border-radius:99px;background:var(--panel2);border:1px solid var(--line);font-family:var(--mono);white-space:nowrap;color:var(--mut);transition:all .25s}
.statechip.ok{color:#06281f;background:rgba(61,220,151,.16);border-color:rgba(61,220,151,.4)}
.statechip.warn{color:#3a2404;background:rgba(255,180,84,.16);border-color:rgba(255,180,84,.4)}
.statechip.bad{color:#3a1010;background:rgba(255,107,107,.15);border-color:rgba(255,107,107,.4)}
.statechip.acc{color:#022420;background:rgba(47,212,195,.16);border-color:rgba(47,212,195,.45)}
.kv{display:grid;margin:8px 0}
.kvrow{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:5px 0;border-bottom:1px dashed rgba(42,85,112,.35);font-size:.79rem}
.kvrow:last-child{border-bottom:0}
.kvrow span{color:var(--mut)}
.kvrow b{font-family:var(--mono);font-weight:600;text-align:right;word-break:break-word}
.tag{display:inline-block;padding:3px 8px;border-radius:7px;font-size:.67rem;font-weight:700;font-family:var(--mono);background:var(--panel2);border:1px solid var(--line);color:var(--mut);transition:all .2s}
.tag.on{color:#3a2404;background:rgba(255,180,84,.15);border-color:rgba(255,180,84,.4)}
.tag.ok{color:#06281f;background:rgba(61,220,151,.16);border-color:rgba(61,220,151,.4)}
.tag.bad{color:#3a1010;background:rgba(255,107,107,.15);border-color:rgba(255,107,107,.4)}
.tag.flash{animation:pulse .7s}
@keyframes pulse{0%{transform:scale(.9);box-shadow:0 0 0 0 rgba(61,220,151,.55)}70%{transform:scale(1.06);box-shadow:0 0 0 9px rgba(61,220,151,0)}100%{transform:scale(1)}}
button{font:inherit;font-weight:700;font-size:.8rem;padding:9px 13px;border-radius:10px;border:1px solid transparent;background:linear-gradient(180deg,#2a8f86,#1d726b);color:#eafffb;cursor:pointer;transition:filter .15s,transform .1s,background .2s}
button:hover:not(:disabled){filter:brightness(1.12)}
button:active:not(:disabled){transform:translateY(1px)}
button:disabled{opacity:.4;cursor:not-allowed}
button.ghost{background:var(--panel2);border-color:var(--line2);color:var(--tx)}
button.pass{background:linear-gradient(180deg,#2f9e68,#1f7a4e);color:#eafff5}
button.byp{background:linear-gradient(180deg,#b3822c,#8f6420);color:#fff8ea}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin:10px 0 4px}
.verdict{margin-top:10px;padding-top:10px;border-top:1px solid var(--line);display:flex;flex-direction:column;gap:8px}
.gatereason{font-size:.74rem;color:var(--warn);min-height:1em}
.gatereason.ok{color:var(--ok)}
.decide{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.decide button{flex:1;min-width:104px}
.decbadge{margin-left:auto;padding:4px 9px;border-radius:99px;font-size:.65rem;font-weight:700;font-family:var(--mono);border:1px solid var(--line);background:var(--panel2);color:var(--mut)}
.decbadge.ok{color:#06281f;background:rgba(61,220,151,.16);border-color:rgba(61,220,151,.4)}
.decbadge.warn{color:#3a2404;background:rgba(255,180,84,.16);border-color:rgba(255,180,84,.4)}
.pbar{height:9px;border-radius:99px;background:#0a1620;border:1px solid var(--line);overflow:hidden;margin:10px 0}
.pbar i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--acc),var(--acc2));transition:width .4s}
.pnext{font-size:.82rem;background:rgba(74,168,255,.08);border:1px solid rgba(74,168,255,.25);border-radius:10px;padding:9px 12px;margin-bottom:10px}
.pitems{display:grid;grid-template-columns:repeat(auto-fit,minmax(132px,1fr));gap:7px}
.pitem{display:flex;justify-content:space-between;gap:8px;align-items:center;font-size:.73rem;padding:7px 9px;border:1px solid var(--line);border-radius:9px;background:var(--panel2)}
.pitem span{color:var(--mut)}
.pitem b.ok{color:var(--ok)}.pitem b.warn{color:var(--warn)}.pitem b.mut{color:var(--mut)}
.leds{display:flex;gap:6px;margin:8px 0 2px;flex-wrap:wrap}
.led{width:24px;height:24px;border-radius:7px;background:#122532;border:1px solid var(--line);display:grid;place-items:center;font-size:.58rem;font-family:var(--mono);color:var(--mut);transition:all .18s}
.led.on{background:linear-gradient(180deg,#2fd4c3,#1a8f83);color:#022420;border-color:#5ef0e2;box-shadow:0 0 12px rgba(47,212,195,.55)}
.led.pwr{background:rgba(61,220,151,.14);border-color:rgba(61,220,151,.4);color:var(--ok)}
.stages{display:flex;flex-wrap:wrap;gap:5px;margin:8px 0}
.stage{font-size:.61rem;font-weight:700;font-family:var(--mono);letter-spacing:.4px;padding:4px 8px;border-radius:7px;background:var(--panel2);border:1px solid var(--line);color:var(--mut);transition:all .2s}
.stage.past{color:var(--tx);border-color:var(--line2)}
.stage.on{color:#022420;background:rgba(47,212,195,.18);border-color:rgba(47,212,195,.5)}
.stage.ok{color:#06281f;background:rgba(61,220,151,.2);border-color:rgba(61,220,151,.55)}
.stage.bad{color:#3a1010;background:rgba(255,107,107,.16);border-color:rgba(255,107,107,.5)}
.btngrid{display:grid;gap:10px;grid-template-columns:1fr 1fr}
.btncard{border:1px solid var(--line);border-radius:12px;padding:11px;background:var(--panel2)}
.btncard h3{margin:0 0 4px;font-size:.8rem;display:flex;justify-content:space-between;align-items:center;gap:8px}
.armedtag{font-size:.58rem;font-weight:700;padding:3px 7px;border-radius:99px;border:1px solid var(--line);color:var(--mut);font-family:var(--mono);transition:all .2s}
.armedtag.on{color:#022420;background:rgba(47,212,195,.18);border-color:rgba(47,212,195,.5)}
.bigs{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:6px 0}
.big{border:1px solid var(--line);border-radius:12px;padding:10px 12px;background:var(--panel2)}
.big small{display:block;color:var(--mut);font-size:.6rem;letter-spacing:.6px;text-transform:uppercase}
.big b{font-family:var(--mono);font-size:1.4rem;font-weight:600;transition:color .3s}
.big b.ok{color:var(--ok)}
details.coll{border:1px solid var(--line);border-radius:15px;background:linear-gradient(180deg,#0d1922,#0a131b);overflow:hidden;margin-bottom:12px}
details.coll>summary{cursor:pointer;padding:13px 16px;font-weight:700;font-size:.86rem;list-style:none;display:flex;justify-content:space-between;align-items:center;gap:10px;transition:background .2s}
details.coll>summary:hover{background:rgba(42,85,112,.15)}
details.coll>summary::-webkit-details-marker{display:none}
details.coll>summary::after{content:"+";color:var(--mut);font-family:var(--mono)}
details.coll[open]>summary::after{content:"–"}
details.coll .inner{padding:2px 16px 16px}
input,textarea{width:100%;padding:10px 12px;background:#08121a;border:1px solid var(--line2);border-radius:10px;color:var(--tx);font:inherit;margin:7px 0}
input:focus,textarea:focus{outline:none;border-color:var(--acc)}
textarea{min-height:88px;resize:vertical}
.networks{display:grid;gap:7px;max-height:230px;overflow:auto;margin:10px 0}
.network{display:flex;justify-content:space-between;align-items:center;width:100%;padding:10px 12px;background:var(--panel2);border:1px solid var(--line);border-radius:10px;color:var(--tx);text-align:left;font-weight:600;font-size:.8rem}
.network.selected{border-color:var(--acc);background:rgba(47,212,195,.1)}
.network .mut{color:var(--mut);font-weight:500;font-family:var(--mono);font-size:.7rem}
.log{white-space:pre-wrap;word-break:break-word;background:#060e14;border:1px solid var(--line);border-radius:11px;padding:10px 12px;max-height:150px;overflow:auto;font-family:var(--mono);font-size:.71rem;color:#8fb4c9}
.err{color:var(--bad);font-size:.75rem;min-height:1em}
.modal{position:fixed;inset:0;background:rgba(2,8,12,.74);display:grid;place-items:center;padding:18px;z-index:20}
.modal[hidden]{display:none}
.sheet{width:min(470px,100%);background:linear-gradient(180deg,#10202c,#0c1720);border:1px solid var(--line2);border-radius:16px;padding:18px;box-shadow:0 24px 60px rgba(0,0,0,.5)}
.sheet h3{margin:0 0 6px;font-size:1rem}
@media(max-width:600px){body{padding:12px 10px 34px}.card{padding:13px 12px}.btngrid,.bigs{grid-template-columns:1fr}.decide button{min-width:88px}}
</style></head><body>
<header class="top">
 <div class="brand"><div class="logo">TMM</div><div><h1>TMM V6 R0 M0</h1><div class="sub">Portal QC lokal · <span data-fw="{{FIRMWARE_VERSION}}">{{FIRMWARE_VERSION}}</span></div></div></div>
 <div class="live" id="live">● MENGHUBUNGKAN</div>
</header>
<div class="strip">
 <div class="chip" id="chipHb" data-state="mut"><span class="cdot"></span><div><small>Heartbeat</small><b id="chipHbVal">—</b><small class="csub" id="chipHbSub">—</small></div></div>
 <div class="chip" id="chipWifi" data-state="mut"><span class="cdot"></span><div><small>Wi-Fi</small><b id="chipWifiVal">—</b><small class="csub" id="chipWifiSub">—</small></div></div>
 <div class="chip" id="chipOled" data-state="mut"><span class="cdot"></span><div><small>OLED</small><b id="chipOledVal">—</b><small class="csub" id="chipOledSub">—</small></div></div>
 <div class="chip" id="chipOta" data-state="mut"><span class="cdot"></span><div><small>OTA</small><b id="chipOtaVal">—</b><small class="csub" id="chipOtaSub">—</small></div></div>
</div>
<main>
<div class="layout">
 <div class="colmain">
<div class="grid">
<section class="card" id="card-led" data-qc="led">
 <div class="chead"><h2>1 · LED 1–10</h2><span class="statechip" id="ledState">IDLE</span></div>
 <p class="hint">Output terbukti <b>active-low</b>: LED menyala saat output LOW. Tekan <b>Jalankan siklus LED</b> — LED2–LED10 menyala <b>satu per satu</b> berurutan (0,3 s per LED) dan terus berulang sampai <b>Stop</b>. LED1 (indikator power) menyala terus dan bukan bagian siklus.</p>
 <div class="leds" id="ledDots"></div>
 <div class="kv">
  <div class="kvrow"><span>Mode</span><b id="ledMode">—</b></div>
  <div class="kvrow"><span>LED aktif saat ini</span><b id="ledCurrent">—</b></div>
  <div class="kvrow"><span>Interval per LED</span><b id="ledInterval">—</b></div>
  <div class="kvrow"><span>Siklus selesai</span><b id="ledCycles">0</b></div>
  <div class="kvrow"><span>Error terakhir</span><b id="ledError">—</b></div>
 </div>
 <div class="actions"><button id="ledStartBtn" onclick="ledStart()">Jalankan siklus LED</button><button class="ghost" id="ledStopBtn" onclick="ledStop()">Stop siklus</button></div>
 <p class="hint" style="margin:8px 0 4px"><b>Mode manual:</b> klik nomor untuk menyalakan <b>tepat LED itu</b> (yang lain mati). <b>Semua OFF</b> mematikan LED2–LED10 tanpa mengubah bit lain.</p>
 <div class="ledbtns" id="ledManualBtns"></div>
 <div class="verdict"><span class="gatereason" id="ledGate"></span><div class="decide"><button class="pass" id="ledPass" onclick="decide('led','pass')">PASS</button><button class="byp" onclick="openBypass('led')">BYPASS</button><span class="decbadge" id="ledDecision">MENUNGGU</span></div></div>
</section>
<section class="card" id="card-buttons" data-qc="buttons">
 <div class="chead"><h2>2 · Tombol Fisik</h2><span class="statechip warn" id="btnState">MENUNGGU UJI</span></div>
 <p class="hint">Arm setiap tombol secara terpisah, lalu tekan dan lepas tombol fisik hingga siklus penuh terbukti. Kedua tombol wajib lolos sebelum PASS.</p>
 <div class="btngrid">
  <div class="btncard" id="subBoot">
   <h3>BOOT <span class="armedtag" id="bootArmed">BELUM ARM</span></h3>
   <div class="actions" style="margin:6px 0 2px"><button class="ghost" onclick="armButton('boot')">Arm BOOT</button></div>
   <div class="kv">
    <div class="kvrow"><span>Level stabil</span><b id="bootStable">—</b></div>
    <div class="kvrow"><span>Bukti tekan</span><b><span class="tag" id="bootPress">LEPAS</span></b></div>
    <div class="kvrow"><span>Bukti siklus penuh</span><b><span class="tag" id="bootCycle">BELUM ADA</span></b></div>
    <div class="kvrow"><span>Total siklus</span><b id="bootCount">0 siklus</b></div>
    <div class="kvrow"><span>Event terakhir</span><b id="bootEvent">—</b></div>
   </div>
  </div>
  <div class="btncard" id="subCd">
   <h3>CHANGE DISPLAY <span class="armedtag" id="cdArmed">BELUM ARM</span></h3>
   <div class="actions" style="margin:6px 0 2px"><button class="ghost" onclick="armButton('cd')">Arm CHANGE DISPLAY</button></div>
   <div class="kv">
    <div class="kvrow"><span>Level stabil</span><b id="cdStable">—</b></div>
    <div class="kvrow"><span>Bukti tekan</span><b><span class="tag" id="cdPress">LEPAS</span></b></div>
    <div class="kvrow"><span>Bukti siklus penuh</span><b><span class="tag" id="cdCycle">BELUM ADA</span></b></div>
    <div class="kvrow"><span>Total siklus</span><b id="cdCount">0 siklus</b></div>
    <div class="kvrow"><span>Event terakhir</span><b id="cdEvent">—</b></div>
   </div>
  </div>
 </div>
 <div class="verdict"><span class="gatereason" id="buttonsGate"></span><div class="decide"><button class="pass" id="buttonsPass" onclick="decide('buttons','pass')">PASS</button><button class="byp" onclick="openBypass('buttons')">BYPASS</button><span class="decbadge" id="buttonsDecision">MENUNGGU</span></div></div>
</section>
<section class="card" id="card-aht" data-qc="aht">
 <div class="chead"><h2>3 · AHT10 Suhu &amp; Kelembapan</h2><span class="statechip" id="ahtState">MENUNGGU</span></div>
 <p class="hint">Nilai diperbarui otomatis dari status live perangkat (sampling ±2 detik) — tidak ada tombol baca manual.</p>
 <div class="bigs">
  <div class="big"><small>Suhu</small><b id="ahtTemp">—</b></div>
  <div class="big"><small>Kelembapan</small><b id="ahtHum">—</b></div>
 </div>
 <div class="kv">
  <div class="kvrow"><span>Usia sampel</span><b id="ahtAge">—</b></div>
  <div class="kvrow"><span>Deteksi sensor (ACK 0x38)</span><b id="ahtPresent">—</b></div>
  <div class="kvrow"><span>Tahap I2C</span><b id="ahtStage">—</b></div>
  <div class="kvrow"><span>Kalibrasi (status bit3)</span><b id="ahtCal">—</b></div>
  <div class="kvrow"><span>Status byte mentah</span><b id="ahtStatus">—</b></div>
  <div class="kvrow"><span>Jumlah error</span><b id="ahtErrors">—</b></div>
  <div class="kvrow"><span>Error terakhir</span><b id="ahtError">—</b></div>
 </div>
 <div class="actions"><button class="ghost" id="ahtRetry" onclick="act('aht10-retry')">Coba ulang sensor</button></div>
 <div class="verdict"><span class="gatereason" id="ahtGate"></span><div class="decide"><button class="pass" id="ahtPass" onclick="decide('aht','pass')">PASS</button><button class="byp" onclick="openBypass('aht')">BYPASS</button><span class="decbadge" id="ahtDecision">MENUNGGU</span></div></div>
</section>
<section class="card" id="card-ethernet" data-qc="ethernet">
 <div class="chead"><h2>4 · Ethernet W5500</h2><span class="statechip" id="ethState">IDLE</span></div>
 <p class="hint"><b>Prosedur QC:</b> colok kabel LAN ke jack RJ45, tekan <b>Mulai tes</b>. Syarat lolos: W5500 terdeteksi + link ON + DHCP sukses + IP bukan 0.0.0.0.</p>
 <div class="stages" id="ethStages"></div>
 <div class="kv">
  <div class="kvrow"><span>Deteksi W5500</span><b><span class="tag" id="ethDetect">—</span></b></div>
  <div class="kvrow"><span>Link</span><b><span class="tag" id="ethLink">—</span></b></div>
  <div class="kvrow"><span>DHCP</span><b><span class="tag" id="ethDhcp">—</span></b></div>
  <div class="kvrow"><span>IP</span><b id="ethIp">—</b></div>
  <div class="kvrow"><span>Durasi</span><b id="ethDur">—</b></div>
  <div class="kvrow"><span>Error terakhir</span><b id="ethError">—</b></div>
 </div>
 <div class="actions"><button id="ethStart" onclick="act('ethernet-start')">Mulai tes</button><button class="ghost" id="ethStop" onclick="act('ethernet-stop')" disabled>Stop</button></div>
 <div class="verdict"><span class="gatereason" id="ethernetGate"></span><div class="decide"><button class="pass" id="ethernetPass" onclick="decide('ethernet','pass')">PASS</button><button class="byp" onclick="openBypass('ethernet')">BYPASS</button><span class="decbadge" id="ethernetDecision">MENUNGGU</span></div></div>
</section>
<section class="card" id="card-sd" data-qc="sd">
 <div class="chead"><h2>5 · MicroSD</h2><span class="statechip" id="sdState">IDLE</span></div>
 <p class="hint"><b>Prosedur QC:</b> masukkan kartu MicroSD, tekan <b>Mulai tes</b>. Firmware membuat satu file unik milik tes, menulis, menutup, membuka ulang, membandingkan isi secara persis, menghapusnya, lalu memastikan file benar-benar hilang. File lama Anda tidak disentuh.</p>
 <div class="stages" id="sdStages"></div>
 <div class="kv">
  <div class="kvrow"><span>Kartu merespons (CMD0)</span><b><span class="tag" id="sdCard">—</span></b></div>
  <div class="kvrow"><span>Tipe kartu</span><b id="sdType">—</b></div>
  <div class="kvrow"><span>Mount</span><b><span class="tag" id="sdMount">—</span></b></div>
  <div class="kvrow"><span>Tulis</span><b><span class="tag" id="sdWrite">—</span></b></div>
  <div class="kvrow"><span>Readback</span><b><span class="tag" id="sdRead">—</span></b></div>
  <div class="kvrow"><span>Cleanup</span><b><span class="tag" id="sdClean">—</span></b></div>
  <div class="kvrow"><span>Ukuran data</span><b id="sdBytes">—</b></div>
  <div class="kvrow"><span>File tes</span><b id="sdFile">—</b></div>
  <div class="kvrow"><span>Durasi</span><b id="sdDur">—</b></div>
  <div class="kvrow"><span>Error terakhir</span><b id="sdError">—</b></div>
 </div>
 <div class="actions"><button id="sdStart" onclick="act('sd-start')">Mulai tes</button></div>
 <div class="verdict"><span class="gatereason" id="sdGate"></span><div class="decide"><button class="pass" id="sdPass" onclick="decide('sd','pass')">PASS</button><button class="byp" onclick="openBypass('sd')">BYPASS</button><span class="decbadge" id="sdDecision">MENUNGGU</span></div></div>
</section>
</div>
 <details class="coll">
  <summary>Jaringan Wi-Fi (provisioning)<span class="statechip" id="wifiChip">SEKUNDER</span></summary>
  <div class="inner">
   <p class="hint">QC hardware tidak memerlukan Wi-Fi. Bagian ini hanya untuk koneksi LAN dan OTA.</p>
   <button class="ghost" id="scanBtn" onclick="startScan()">Cari jaringan Wi-Fi</button>
   <div id="networks" class="networks"><span class="hint">Tekan tombol untuk memindai.</span></div>
   <input id="wifiPassword" type="password" maxlength="63" placeholder="Password Wi-Fi (kosongkan untuk jaringan terbuka)" autocomplete="off">
   <div class="actions"><button id="connectBtn" onclick="connectWifi()" disabled>Sambungkan</button><button class="ghost" onclick="act('wifi-reconnect')">Hubungkan ulang</button></div>
   <p class="warn">Password dikirim lewat HTTP hotspot terbuka dan disimpan di NVS perangkat. Gunakan hanya pada meja QC terkontrol.</p>
  </div>
 </details>
 <details class="coll">
  <summary>Update firmware OTA<span class="statechip warn">SEKUNDER</span></summary>
  <div class="inner">
   <p class="warn">Hanya file aplikasi <b>.ino.bin</b> yang sah untuk partisi ini. File <b>merged</b> atau full-flash 16 MB dilarang dan dapat merusak bootloader/partisi.</p>
   <input id="otaPassword" type="password" minlength="8" maxlength="63" placeholder="Password OTA perangkat" autocomplete="off">
   <input id="otaFile" type="file" accept=".bin,application/octet-stream">
   <div class="pbar"><i id="otaBar"></i></div>
   <div class="actions"><button class="ghost" onclick="saveOtaPassword()">Simpan password OTA</button><button id="otaUploadBtn" onclick="uploadFirmware()">Upload &amp; restart</button></div>
   <p class="hint" id="otaMessage">Heartbeat tetap dilayani selama upload. Perangkat restart otomatis setelah image valid.</p>
  </div>
 </details>
 </div>
 <aside class="side">
  <section class="card" id="card-progress">
   <div class="chead"><h2>Progres QC Perangkat</h2><button class="ghost" id="exportBtn" onclick="exportQc()" disabled>Export JSON</button></div>
   <div class="pbar"><i id="pbarFill"></i></div>
   <div class="pnext" id="pnext">Memuat status perangkat…</div>
   <div class="pitems" id="pitems"></div>
   <p class="gnote">Setiap item wajib diakhiri PASS berbasis bukti atau BYPASS dengan alasan tertulis. Export terbuka setelah kelima item diputuskan.</p>
  </section>
  <section class="card" id="card-guide">
   <div class="chead"><h2>Tutorial &amp; Aturan</h2><span class="statechip acc">WAJIB</span></div>
   <ol class="guide">
    <li><b>LED 1–10:</b> tekan <b>Jalankan siklus LED</b> — LED2–LED10 menyala satu per satu (0,3 s per LED) dan terus berulang sampai <b>Stop</b>. LED1 adalah indikator power dan harus tetap menyala. Mode <b>manual</b> menyalakan tepat satu LED untuk inspeksi.</li>
    <li><b>Tombol Fisik:</b> tekan <b>Arm</b> pada kartu tombol, lalu tekan-lepas tombol fisik hingga <b>siklus penuh</b> tercatat. Kedua tombol wajib lolos.</li>
    <li><b>AHT10:</b> nilai suhu/kelembapan muncul otomatis (±2 s). Jika sensor tidak terbaca, periksa diagnostik ACK/kalibrasi pada kartu.</li>
    <li><b>Ethernet:</b> colok kabel LAN, tekan <b>Mulai tes</b>; lolos bila W5500 terdeteksi + link ON + DHCP memberi IP.</li>
    <li><b>MicroSD:</b> masukkan kartu, tekan <b>Mulai tes</b>; firmware menulis, membaca ulang, lalu menghapus satu file tes miliknya sendiri. File Anda tidak disentuh.</li>
   </ol>
   <p class="gnote">Urutan kartu = urutan wajib. PASS hanya dengan bukti live; BYPASS adalah pengecualian tercatat, bukan bukti hardware. Setelah kelima item diputuskan, tombol <b>Export JSON</b> terbuka.</p>
  </section>
 </aside>
</div>
</main>
<div class="modal" id="bypassModal" hidden>
 <div class="sheet">
  <h3 id="bypassTitle">Bypass</h3>
  <p class="hint">Bypass adalah pengecualian tercatat, bukan bukti hardware PASS. Alasan wajib diisi.</p>
  <textarea id="bypassReason" maxlength="240" placeholder="Contoh: perangkat uji tidak tersedia di meja QC"></textarea>
  <div class="err" id="bypassError"></div>
  <div class="actions"><button class="byp" onclick="confirmBypass()">Simpan bypass</button><button class="ghost" onclick="closeBypass()">Batal</button></div>
 </div>
</div>
<script>
'use strict';
const q=id=>document.getElementById(id);
const ITEMS=['led','buttons','aht','ethernet','sd'];
const LABELS={led:'LED 1–10',buttons:'Tombol Fisik',aht:'AHT10',ethernet:'Ethernet W5500',sd:'MicroSD'};
const ETH_STAGES=['idle','initializing','waiting_link','acquiring_dhcp','passed','failed'];
const ETH_LABELS=['IDLE','INISIALISASI','MENUNGGU LINK','DHCP','LOLOS','GAGAL'];
const SD_STAGES=['idle','probing','mounting','writing','reading','cleaning','passed','failed'];
const SD_LABELS=['IDLE','PROBE','MOUNT','TULIS','BACA','BERSIH','LOLOS','GAGAL'];
let lastStatus=null,bypassId=null;
const decisions={},armed={boot:false,cd:false},prev={},logLines=[];
let pollRunning=false,pollWake=false;
const set=(id,text)=>{const n=q(id);if(n)n.textContent=text;};
const setCls=(id,text,cls)=>{const n=q(id);if(!n)return;if(text!==undefined)n.textContent=text;if(cls)n.className=cls;};
const dash=v=>v===null||v===undefined||v===''?'—':v;
const el=(tag,cls,text)=>{const n=document.createElement(tag);if(cls)n.className=cls;if(text!==undefined)n.textContent=text;return n;};
function chipState(id,state,val,sub){const c=q(id);if(c)c.dataset.state=state;set(id+'Val',val);if(sub!==undefined)set(id+'Sub',sub);}
function flash(key,id,val){const sig=String(val);if(prev[key]!==undefined&&prev[key]!==sig){const n=q(id);if(n){n.classList.remove('flash');void n.offsetWidth;n.classList.add('flash');}}prev[key]=sig;}
function addLog(text){logLines.push(new Date().toLocaleTimeString()+'  '+text);while(logLines.length>40)logLines.shift();set('log',logLines.join('\n'));}
function setLive(on){const n=q('live');if(!n)return;n.textContent=on?'● ONLINE':'● OFFLINE';n.className='live '+(on?'on':'off');}
function buildStepper(prefix,stages,labels){const w=q(prefix+'Stages');if(!w)return;stages.forEach((name,i)=>{const s=el('span','stage',(labels&&labels[i])||name.toUpperCase());s.id=prefix+'Stage'+i;w.appendChild(s);});}
function setStepper(prefix,stages,stage){const idx=stages.indexOf(stage||'idle');stages.forEach((name,i)=>{const n=q(prefix+'Stage'+i);if(!n)return;let cls='stage';if(idx>=0&&i<idx)cls+=' past';if(idx>=0&&i===idx)cls+=(name==='passed'?' ok':name==='failed'?' bad':' on');n.className=cls;});}
function sleepSliced(ms){return new Promise(resolve=>{let left=ms;const t=setInterval(()=>{left-=250;if(left<=0||pollWake){clearInterval(t);resolve();}},250);});}
function testActive(s){return !!(s&&((s.ledTest&&s.ledTest.running)||(s.ethernet&&s.ethernet.running)||(s.sdTest&&s.sdTest.running)));}
function ledActive(s){return !!(s&&s.ledTest&&s.ledTest.running);}
async function pollLoop(){
 if(pollRunning)return;pollRunning=true;
 for(;;){
  pollWake=false;
  try{
   const r=await fetch('/api/status',{cache:'no-store'});
   lastStatus=await r.json();setLive(true);
   try{renderAll(lastStatus);}catch(e){addLog('Render status gagal: '+((e&&e.message)||e));}
  }catch(e){setLive(false);}
  // Poll cadence matches the fastest moving evidence: while the LED sequence
  // or manual mode is live (300 ms step) poll at 300 ms so the on-screen dots
  // never jump ahead of the hardware; other running tests poll at 1 s; idle
  // falls back to 2 s.
  const wait=pollWake?0:(ledActive(lastStatus)?300:(testActive(lastStatus)?1000:2000));
  if(wait)await sleepSliced(wait);
 }
}
function renderAll(s){
 renderStrip(s);renderLed(s.ledTest||{});renderButtons(s.buttons||{});
 renderAht(s.aht10||{});renderEthernet(s.ethernet||{});renderSd(s.sdTest||{});renderDecisions();
}
function renderStrip(s){
 const hb=s.heartbeat||{},n=s.network||{},o=s.oled||{},ot=s.ota||{};
 chipState('chipHb',hb.ready?'ok':'bad',hb.ready?'AKTIF':'MATI','interval '+(hb.intervalMs||0)+' ms');
 chipState('chipWifi',n.connected?'ok':'mut',n.connected?'LAN TERSEDIA':'OFFLINE',n.connected?((n.lanIp||'—')+' · '+(n.rssi||0)+' dBm'):'AP '+(n.apSsid||'—'));
 chipState('chipOled',o.ready?'ok':'bad',o.ready?'SIAP':'TIDAK ADA','SSD1306 128×64');
 chipState('chipOta',ot.ready?'ok':(ot.configured?'warn':'mut'),ot.ready?'AKTIF':(ot.configured?'SIAP':'BELUM DIISI'),'web upload .ino.bin');
}
function renderLed(t){
 t=t||{};
 const mode=t.mode||'idle',run=!!t.running,active=(t.activeLed===null||t.activeLed===undefined)?null:t.activeLed;
 setCls('ledState',run?(mode==='manual'?'MANUAL':'SIKLUS BERJALAN'):(t.lastError?'ERROR':((t.cyclesCompleted||0)>0?'SELESAI':'IDLE')),'statechip '+(run?'acc':(t.lastError?'bad':((t.cyclesCompleted||0)>0?'ok':''))));
 set('ledMode',mode==='sequence'?'Siklus berurutan (loop sampai Stop)':(mode==='manual'?'Manual (tepat satu LED)':'Idle'));
 set('ledCurrent',active===null?'—':'LED'+active+' (output LOW)');
 set('ledInterval',dash(t.intervalMs)+' ms');
 set('ledCycles',String(t.cyclesCompleted||0));
 set('ledError',dash(t.lastError));
 for(let i=2;i<=10;i++){const d=q('ledDot'+i);if(d)d.classList.toggle('on',active===i);
  const m=q('ledMb'+i);if(m)m.classList.toggle('active',active===i);}
 flash('ledCycles','ledCycles',t.cyclesCompleted||0);
}
function oneButton(key,b){
 b=b||{};
 const tag=q(key+'Armed');if(tag){tag.textContent=armed[key]?'ARMED':'BELUM ARM';tag.classList.toggle('on',!!armed[key]);}
 set(key+'Stable',b.stableLevel===undefined?'—':(b.stableLevel===b.idleLevel?'IDLE (level '+b.stableLevel+')':'AKTIF (level '+b.stableLevel+')'));
 setCls(key+'Press',b.pressed?'DITEKAN':'LEPAS','tag'+(b.pressed?' on':''));
 setCls(key+'Cycle',b.fullCycleConfirmed?'SIKLUS PENUH ✓':'BELUM ADA','tag'+(b.fullCycleConfirmed?' ok':''));
 set(key+'Count',(b.pressCount||0)+' siklus');
 set(key+'Event',dash(b.lastEvent));
 flash(key+'Press',key+'Press',!!b.pressed);
 flash(key+'Cycle',key+'Cycle',!!b.fullCycleConfirmed);
}
function renderButtons(b){
 b=b||{};oneButton('boot',b.boot);oneButton('cd',b.changeDisplay);
 const ok=!!(b.boot&&b.boot.fullCycleConfirmed)&&!!(b.changeDisplay&&b.changeDisplay.fullCycleConfirmed);
 setCls('btnState',ok?'TERBUKTI':'MENUNGGU UJI','statechip '+(ok?'ok':'warn'));
}
function renderAht(a){
 a=a||{};const valid=!!a.sampleValid&&!a.stale;
 setCls('ahtTemp',a.temperatureC===null||a.temperatureC===undefined?'—':Number(a.temperatureC).toFixed(1)+' °C',valid?'ok':'');
 setCls('ahtHum',a.humidityPercent===null||a.humidityPercent===undefined?'—':Number(a.humidityPercent).toFixed(1)+' %',valid?'ok':'');
 setCls('ahtState',a.stale?'STALE':(a.sampling?'SAMPLING':(a.sampleValid?'VALID':'MENUNGGU')),'statechip '+(a.stale?'warn':(a.sampling?'acc':(a.sampleValid?'ok':''))));
 set('ahtAge',a.sampleAgeMs===null||a.sampleAgeMs===undefined?'—':Math.round(a.sampleAgeMs/1000)+' s sejak sampel');
 set('ahtPresent',a.present?'Sensor merespons di 0x38':'Sensor tidak merespons di 0x38');
 set('ahtStage',dash(a.stage));
 setCls('ahtCal',a.calibrated?'terkalibrasi':'belum terkonfirmasi','tag'+(a.calibrated?' ok':' bad'));
 set('ahtStatus',dash(a.statusByte));
 set('ahtErrors',String(a.errorCount||0));
 set('ahtError',dash(a.lastError));
 const r=q('ahtRetry');if(r)r.disabled=!!a.sampling;
}
function renderEthernet(e){
 e=e||{};setStepper('eth',ETH_STAGES,e.stage);
 setCls('ethState',e.running?'TES BERJALAN':(e.testPassed?'LOLOS':(e.stage==='failed'?'GAGAL':'IDLE')),'statechip '+(e.running?'acc':(e.testPassed?'ok':(e.stage==='failed'?'bad':''))));
 setCls('ethDetect',e.hardwareDetected?'W5500 terdeteksi':'W5500 belum terdeteksi','tag'+(e.hardwareDetected?' ok':''));
 setCls('ethLink',e.link==='on'?'Link ON':(e.link==='off'?'Link OFF':'Link tidak diketahui'),'tag'+(e.link==='on'?' ok':''));
 setCls('ethDhcp',e.dhcpPassed?'DHCP berhasil':'DHCP belum berhasil','tag'+(e.dhcpPassed?' ok':''));
 set('ethIp',e.ip&&e.ip!=='0.0.0.0'?e.ip:'—');
 set('ethDur',e.durationMs===null||e.durationMs===undefined?'berjalan…':Math.round(e.durationMs/1000)+' s');
 set('ethError',dash(e.lastError));
 const s=q('ethStart'),x=q('ethStop');if(s)s.disabled=!!e.running;if(x)x.disabled=!e.running;
}
function renderSd(d){
 d=d||{};setStepper('sd',SD_STAGES,d.stage);
 setCls('sdState',d.running?'TES BERJALAN':(d.testPassed?'LOLOS':(d.stage==='failed'?'GAGAL':'IDLE')),'statechip '+(d.running?'acc':(d.testPassed?'ok':(d.stage==='failed'?'bad':''))));
 setCls('sdCard',d.cardPresent?'Kartu merespons (CMD0 ack)':'Kartu tidak merespons','tag'+(d.cardPresent?' ok':' bad'));
 set('sdType',dash(d.cardType));
 setCls('sdMount',d.mounted?'Mount OK':'Belum mount','tag'+(d.mounted?' ok':''));
 setCls('sdWrite',d.writePassed?'Tulis OK':'Belum lolos','tag'+(d.writePassed?' ok':''));
 setCls('sdRead',d.readbackPassed?'Isi identik':'Belum lolos','tag'+(d.readbackPassed?' ok':''));
 setCls('sdClean',d.cleanupPassed?'Dihapus & terverifikasi':'Belum lolos','tag'+(d.cleanupPassed?' ok':''));
 set('sdBytes',d.bytesWritten===null||d.bytesWritten===undefined?'—':d.bytesWritten+' byte');
 set('sdFile',dash(d.testFile));
 set('sdDur',d.durationMs===null||d.durationMs===undefined?'berjalan…':Math.round(d.durationMs/1000)+' s');
 set('sdError',dash(d.lastError));
 const s=q('sdStart');if(s)s.disabled=!!d.running;
}
function gateReason(id){
 const s=lastStatus||{};
 if(id==='led'){const t=s.ledTest||{};
  if(!t.running&&(t.cyclesCompleted||0)<1)return 'Belum ada siklus LED2–LED10 yang selesai.';
  if(t.lastError)return 'Error: '+t.lastError;
  if(t.running)return 'Tekan Stop untuk menghentikan siklus sebelum PASS.';
  return '';}
 if(id==='buttons'){const b=s.buttons||{};
  if(!(b.boot&&b.boot.fullCycleConfirmed))return 'BOOT: siklus tekan–lepas penuh belum terbukti.';
  if(!(b.changeDisplay&&b.changeDisplay.fullCycleConfirmed))return 'CHANGE DISPLAY: siklus tekan–lepas penuh belum terbukti.';
  return '';}
 if(id==='aht'){const a=s.aht10||{};
  if(!a.sampleValid)return 'Belum ada sampel AHT10 yang valid.';
  if(a.stale)return 'Sampel basi (stale). Tunggu sampel baru atau tekan Coba ulang.';
  return '';}
 if(id==='ethernet'){const e=s.ethernet||{};
  if(e.testPassed)return '';
  if(e.lastError)return 'Error: '+e.lastError;
  if(!e.hardwareDetected)return 'W5500 belum terdeteksi.';
  if(e.link!=='on')return 'Link Ethernet belum ON — colok kabel LAN.';
  if(!e.dhcpPassed)return 'DHCP belum berhasil mendapatkan IP.';
  return 'Jalankan tes Ethernet sampai tahap LOLOS.';}
 if(id==='sd'){const d=s.sdTest||{};
  if(d.testPassed)return '';
  if(d.lastError)return 'Error: '+d.lastError;
  if(d.stage==='idle')return 'Jalankan tes MicroSD.';
  if(!d.mounted)return 'Kartu MicroSD gagal dimount.';
  return 'Tes MicroSD belum mencapai tahap LOLOS.';}
 return 'Item tidak dikenal.';
}
function snapshot(id){const s=lastStatus||{};return {led:s.ledTest,buttons:s.buttons,aht:s.aht10,ethernet:s.ethernet,sd:s.sdTest}[id]||null;}
function decide(id,status){
 if(status!=='pass')return;
 if(decisions[id]&&decisions[id].status==='pass')return;
 const reason=gateReason(id);
 if(reason){addLog('PASS '+LABELS[id]+' diblokir: '+reason);return;}
 decisions[id]={status:'pass',at:new Date().toISOString(),evidence:snapshot(id)};
 addLog(LABELS[id]+' → PASS');
 renderDecisions();
}
function openBypass(id){bypassId=id;set('bypassTitle','Bypass — '+LABELS[id]);set('bypassError','');q('bypassReason').value='';q('bypassModal').hidden=false;q('bypassReason').focus();}
function closeBypass(){q('bypassModal').hidden=true;bypassId=null;}
function confirmBypass(){
 const reason=q('bypassReason').value.trim();
 if(!reason){set('bypassError','Bypass ditolak: alasan wajib diisi.');return;}
 decisions[bypassId]={status:'bypass',reason:reason,at:new Date().toISOString(),evidence:snapshot(bypassId)};
 addLog(LABELS[bypassId]+' → BYPASS ('+reason+')');
 closeBypass();renderDecisions();
}
function renderDecisions(){
 const nextId=ITEMS.find(id=>!decisions[id])||null;
 let done=0;
 ITEMS.forEach(id=>{
  const d=decisions[id];if(d)done++;
  const badge=q(id+'Decision');
  if(badge){badge.textContent=d?d.status.toUpperCase():'MENUNGGU';badge.className='decbadge '+(d?(d.status==='pass'?'ok':'warn'):'');}
  const card=q('card-'+id);if(card)card.classList.toggle('next',!d&&nextId===id);
  const g=q(id+'Gate');
  if(g){const blocked=gateReason(id);
   g.textContent=d?(d.status==='pass'?'Disetujui oleh operator.':'Bypass: '+d.reason):(blocked?'PASS diblokir: '+blocked:'Bukti lengkap — PASS tersedia.');
   g.className='gatereason'+(!d&&!blocked?' ok':'');}
  const p=q(id+'Pass');if(p)p.disabled=(!d&&!!gateReason(id))||!!(d&&d.status==='pass');
 });
 set('pnext',nextId?'Langkah berikutnya: '+LABELS[nextId]:'Semua item selesai — export laporan QC.');
 const fill=q('pbarFill');if(fill)fill.style.width=(done/ITEMS.length*100)+'%';
 const list=q('pitems');if(list){list.textContent='';ITEMS.forEach(id=>{const d=decisions[id];const row=el('div','pitem');row.appendChild(el('span',null,LABELS[id]));row.appendChild(el('b',d?(d.status==='pass'?'ok':'warn'):'mut',d?d.status.toUpperCase():'WAJIB'));list.appendChild(row);});}
 const exp=q('exportBtn');if(exp)exp.disabled=done!==ITEMS.length;
}
function exportQc(){
 if(ITEMS.some(id=>!decisions[id]))return;
 const meta=document.querySelector('meta[name="firmware-version"]');
 const report={schemaVersion:2,product:'TMM',hardware:'TMM_V6_R0_M0',
  firmwareVersion:(lastStatus&&lastStatus.firmwareVersion)||(meta?meta.content:'unknown'),
  exportedAt:new Date().toISOString(),deviceStatus:lastStatus,decisions:decisions,
  overall:ITEMS.every(id=>decisions[id].status==='pass')?'pass':'pass-with-bypass'};
 const a=el('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(report,null,2)],{type:'application/json'}));
 a.download='TMM_QC_'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';
 document.body.appendChild(a);a.click();a.remove();
 setTimeout(()=>URL.revokeObjectURL(a.href),2000);
 addLog('Laporan QC diexport.');
}
async function act(name,extra){
 addLog('Kirim perintah: '+name+(extra?(' '+extra):''));
 try{
  const r=await fetch('/api/action?name='+encodeURIComponent(name)+(extra?'&'+extra:''),{method:'POST'});
  const text=await r.text();let msg=text;
  try{msg=JSON.stringify(JSON.parse(text));}catch(e){}
  addLog(name+' → '+msg);pollWake=true;return r.ok;
 }catch(e){addLog(name+' gagal: '+((e&&e.message)||e));return false;}
}
async function ledStart(){await act('led-start');}
async function ledStop(){await act('led-stop');}
async function ledManual(n){await act('led-manual','led='+n);}
async function ledAllOff(){await act('led-all-off');}
async function armButton(which){const ok=await act(which==='boot'?'boot-arm':'change-display-arm');if(ok)armed[which]=true;}
let selectedSsid='';
async function startScan(){q('scanBtn').disabled=true;set('networks','Memindai jaringan...');try{await fetch('/api/wifi/scan',{method:'POST'});pollScan();}catch(e){set('networks','Pemindaian gagal.');q('scanBtn').disabled=false;}}
async function pollScan(){
 try{const r=await fetch('/api/wifi/scan',{cache:'no-store'});const d=await r.json();
  if(d.state==='scanning'){setTimeout(pollScan,800);return;}
  drawNetworks(d.networks||[]);
 }catch(e){set('networks','Pemindaian gagal.');}
 q('scanBtn').disabled=false;
}
function drawNetworks(items){
 const box=q('networks');box.textContent='';
 if(!items||!items.length){set('networks','Tidak ada jaringan ditemukan.');return;}
 items.forEach(n=>{
  const b=el('button','network');
  b.appendChild(el('span',null,n.ssid));
  b.appendChild(el('span','mut',n.rssi+' dBm · '+(n.secure?'terkunci':'terbuka')));
  b.onclick=()=>{selectedSsid=n.ssid;box.querySelectorAll('.network').forEach(x=>x.classList.remove('selected'));b.classList.add('selected');q('connectBtn').disabled=false;};
  box.appendChild(b);
 });
}
async function connectWifi(){
 if(!selectedSsid)return;
 q('connectBtn').disabled=true;
 addLog('Menyambungkan ke '+selectedSsid+'...');
 try{
  const r=await fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({ssid:selectedSsid,password:q('wifiPassword').value})});
  addLog('Wi-Fi → '+(await r.text()));q('wifiPassword').value='';pollWake=true;
 }catch(e){addLog('Wi-Fi gagal: '+((e&&e.message)||e));}
 q('connectBtn').disabled=false;
}
async function saveOtaPassword(){
 const p=q('otaPassword').value;
 if(p.length<8){set('otaMessage','Password OTA minimal 8 karakter.');return;}
 try{const r=await fetch('/api/ota/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({password:p})});set('otaMessage',await r.text());}
 catch(e){set('otaMessage','Gagal menyimpan password OTA.');}
}
function uploadFirmware(){
 const file=q('otaFile').files[0],password=q('otaPassword').value;
 if(!file){set('otaMessage','Pilih file .ino.bin terlebih dahulu.');return;}
 if(!file.name.endsWith('.bin')){set('otaMessage','File harus berekstensi .bin.');return;}
 if(password.length<8){set('otaMessage','Masukkan password OTA perangkat (min. 8 karakter).');return;}
 const xhr=new XMLHttpRequest(),body=new FormData();
 body.append('firmware',file,file.name);
 q('otaUploadBtn').disabled=true;
 xhr.open('POST','/api/ota/upload?password='+encodeURIComponent(password));
 xhr.upload.onprogress=e=>{if(e.lengthComputable)q('otaBar').style.width=Math.round(e.loaded/e.total*100)+'%';};
 xhr.onload=()=>{set('otaMessage',xhr.responseText||('Selesai ('+xhr.status+')'));q('otaPassword').value='';q('otaUploadBtn').disabled=false;};
 xhr.onerror=()=>{set('otaMessage','Upload gagal: koneksi terputus.');q('otaUploadBtn').disabled=false;};
 xhr.send(body);
}
function buildStatic(){
 const dw=q('ledDots');
 if(dw){const p=el('span','led pwr','1');p.title='LED1 power (selalu menyala)';dw.appendChild(p);
  for(let i=2;i<=10;i++){const d=el('span','led',String(i));d.id='ledDot'+i;dw.appendChild(d);}}
 const lm=q('ledManualBtns');
 if(lm){for(let i=2;i<=10;i++){const b=el('button','ghost',String(i));b.id='ledMb'+i;b.onclick=()=>ledManual(i);lm.appendChild(b);}
  const off=el('button','ghost','Semua OFF');off.id='ledMbOff';off.onclick=ledAllOff;lm.appendChild(off);}
 buildStepper('eth',ETH_STAGES,ETH_LABELS);
 buildStepper('sd',SD_STAGES,SD_LABELS);
}
q('bypassModal').addEventListener('click',e=>{if(e.target===q('bypassModal'))closeBypass();});
document.addEventListener('keydown',e=>{if(e.key==='Escape'&&!q('bypassModal').hidden)closeBypass();});
buildStatic();renderDecisions();addLog('Portal QC siap. Mulai dari item 1: LED.');pollLoop();
</script></body></html>)QC_HTML";

String qcStatusJson() {
  const uint32_t nowMs = millis();
  const bool connected = WiFi.status() == WL_CONNECTED;
  String json;
  json.reserve(1900);
  json += F("{\"heartbeat\":{\"ready\":");
  json += heartbeatReady ? F("true") : F("false");
  json += F(",\"intervalMs\":");
  json += HEARTBEAT_INTERVAL_MS;
  json += F("},\"network\":{\"apSsid\":\"");
  json += qcApSsid;
  json += F("\",\"apIp\":\"");
  json += WiFi.softAPIP().toString();
  json += F("\",\"clients\":");
  json += WiFi.softAPgetStationNum();
  json += F(",\"connected\":");
  json += connected ? F("true") : F("false");
  json += F(",\"lanIp\":\"");
  json += connected ? WiFi.localIP().toString() : String();
  json += F("\",\"rssi\":");
  json += connected ? WiFi.RSSI() : 0;
  json += F("},\"oled\":{\"ready\":");
  json += oledReady ? F("true") : F("false");
  json += F("},\"ota\":{\"configured\":");
  json += otaPassword.length() >= OTA_PASSWORD_MIN_LENGTH ? F("true") : F("false");
  json += F(",\"ready\":");
  json += otaReady ? F("true") : F("false");
  json += F("},\"buttons\":{\"boot\":");
  appendButtonStatus(json, bootButton);
  json += F(",\"changeDisplay\":");
  appendButtonStatus(json, changeDisplayButton);
  json += F("},\"ledTest\":{\"running\":");
  json += ledTest.mode != LedMode::IDLE ? F("true") : F("false");
  json += F(",\"mode\":\"");
  json += ledModeName(ledTest.mode);
  json += F("\",\"polarity\":\"active-low\",\"activeLed\":");
  json += ledTest.currentLed >= 0 ? String(ledTest.currentLed) : String(F("null"));
  json += F(",\"intervalMs\":");
  json += LED_TEST_STEP_INTERVAL_MS;
  json += F(",\"cyclesCompleted\":");
  json += ledTest.cyclesCompleted;
  json += F(",\"lastError\":\"");
  json += ledTest.lastError;
  json += F("\"},\"ethernet\":{\"running\":");
  json += ethernetQc.running ? F("true") : F("false");
  json += F(",\"stage\":\"");
  json += ethernetQcStageName(ethernetQc.stage);
  json += F("\",\"hardwareDetected\":");
  json += ethernetQc.hardwareDetected ? F("true") : F("false");
  json += F(",\"link\":\"");
  json += ethernetQc.link;
  json += F("\",\"dhcpPassed\":");
  json += ethernetQc.dhcpPassed ? F("true") : F("false");
  json += F(",\"ip\":\"");
  json += ethernetQc.ip.toString();
  json += F("\",\"startedMs\":");
  json += ethernetQc.startMs;
  json += F(",\"durationMs\":");
  // durationMs is null while the run is in flight and numeric only once the
  // run reached a terminal state (passed, failed, or stopped).
  if (ethernetQc.running) json += F("null");
  else json += ethernetQc.durationMs;
  json += F(",\"lastError\":\"");
  json += ethernetQc.error;
  json += F("\",\"testPassed\":");
  json += ethernetQc.pass ? F("true") : F("false");
  json += F("},\"sdTest\":{\"running\":");
  json += sdQc.running ? F("true") : F("false");
  json += F(",\"stage\":\"");
  json += sdQcStageName(sdQc.stage);
  json += F("\",\"cardPresent\":");
  json += sdQc.cardPresent ? F("true") : F("false");
  json += F(",\"cardType\":\"");
  json += sdQc.cardType;
  json += F("\",\"mounted\":");
  json += sdQc.mounted ? F("true") : F("false");
  json += F(",\"writePassed\":");
  json += sdQc.writePassed ? F("true") : F("false");
  json += F(",\"readbackPassed\":");
  json += sdQc.readbackPassed ? F("true") : F("false");
  json += F(",\"cleanupPassed\":");
  json += sdQc.cleanupPassed ? F("true") : F("false");
  json += F(",\"testPassed\":");
  json += sdQc.testPassed ? F("true") : F("false");
  json += F(",\"bytesWritten\":");
  json += sdQc.bytesWritten;
  json += F(",\"testFile\":\"");
  json += sdQc.testFile;
  json += F("\",\"startedMs\":");
  json += sdQc.startedMs;
  json += F(",\"durationMs\":");
  // durationMs is null while the run is in flight and numeric only once the
  // run reached a terminal state (passed or failed).
  if (sdQc.running) json += F("null");
  else json += sdQc.durationMs;
  json += F(",\"lastError\":\"");
  json += sdQc.lastError;
  json += F("\",\"nonce\":");
  json += sdQc.nonce;
  json += F(",\"expectedPayload\":\"");
  json += sdQc.expectedPayload;
  json += F("\"},\"aht10\":");
  appendAht10Status(json, nowMs);
  json += F(",\"firmwareVersion\":\"");
  json += FIRMWARE_VERSION;
  json += F("\"}");
  return json;
}

void sendQcPortal() {
  serviceHeartbeat();
  const size_t pageLength = sizeof(QC_PORTAL_HTML) - 1;
  const size_t tokenLength = sizeof(FIRMWARE_VERSION_TOKEN) - 1;
  const size_t versionLength = sizeof(FIRMWARE_VERSION) - 1;

  // Count token occurrences to announce an exact Content-Length while streaming.
  size_t tokenCount = 0;
  for (size_t offset = 0; offset < pageLength;) {
    const char *found = strstr(QC_PORTAL_HTML + offset, FIRMWARE_VERSION_TOKEN);
    if (!found) break;
    ++tokenCount;
    offset = static_cast<size_t>(found - QC_PORTAL_HTML) + tokenLength;
  }

  webServer.sendHeader(F("Cache-Control"), F("no-store"));
  webServer.setContentLength(pageLength - tokenCount * tokenLength + tokenCount * versionLength);
  webServer.send(200, "text/html; charset=utf-8", "");

  // Stream the page, substituting every {{FIRMWARE_VERSION}} occurrence with
  // the single version constant from the version header.
  size_t emitted = 0;
  while (emitted < pageLength) {
    serviceHeartbeat();
    const char *found = strstr(QC_PORTAL_HTML + emitted, FIRMWARE_VERSION_TOKEN);
    const size_t boundary = found ? static_cast<size_t>(found - QC_PORTAL_HTML) : pageLength;
    if (emitted < boundary) {
      size_t take = boundary - emitted;
      if (take > QC_PORTAL_CHUNK_SIZE) take = QC_PORTAL_CHUNK_SIZE;
      webServer.sendContent(QC_PORTAL_HTML + emitted, take);
      emitted += take;
      continue;
    }
    webServer.sendContent(FIRMWARE_VERSION, versionLength);
    emitted += tokenLength;
  }
}

void redirectToQcPortal() {
  webServer.sendHeader(F("Location"), String(F("http://")) + WiFi.softAPIP().toString() + '/');
  webServer.send(302, "text/plain", "");
}

void startWifiScan() {
  serviceHeartbeat();
  WiFi.scanDelete();
  const int16_t result = WiFi.scanNetworks(true, false);
  serviceHeartbeat();
  if (result == WIFI_SCAN_FAILED) {
    webServer.send(500, "application/json", "{\"state\":\"failed\"}");
    return;
  }
  webServer.send(202, "application/json", "{\"state\":\"scanning\"}");
}

void sendWifiScanResult() {
  serviceHeartbeat();
  const int16_t count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) {
    webServer.send(202, "application/json", "{\"state\":\"scanning\"}");
    return;
  }
  if (count == WIFI_SCAN_FAILED) {
    webServer.send(409, "application/json", "{\"state\":\"idle\",\"networks\":[]}");
    return;
  }

  String json = F("{\"state\":\"complete\",\"networks\":[");
  bool first = true;
  const int16_t limitedCount = count > 20 ? 20 : count;
  for (int16_t index = 0; index < limitedCount; ++index) {
    serviceHeartbeat();
    const String ssid = WiFi.SSID(index);
    if (!ssid.length()) continue;
    if (!first) json += ',';
    json += F("{\"ssid\":\"");
    json += jsonEscape(ssid);
    json += F("\",\"rssi\":");
    json += WiFi.RSSI(index);
    json += F(",\"secure\":");
    json += WiFi.encryptionType(index) == WIFI_AUTH_OPEN ? F("false") : F("true");
    json += '}';
    first = false;
  }
  json += F("]}");
  webServer.sendHeader(F("Cache-Control"), F("no-store"));
  webServer.send(200, "application/json", json);
  WiFi.scanDelete();
  serviceHeartbeat();
}

void connectWifiFromPortal() {
  const String ssid = webServer.arg("ssid");
  const String password = webServer.arg("password");
  if (!ssid.length() || ssid.length() > WIFI_SSID_MAX_LENGTH || password.length() > WIFI_PASSWORD_MAX_LENGTH) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_credentials_length\"}");
    return;
  }
  if (!saveWifiConfiguration(ssid, password)) {
    webServer.send(500, "application/json", "{\"ok\":false,\"error\":\"save_failed\"}");
    return;
  }
  webServer.send(202, "application/json", "{\"ok\":true,\"state\":\"connecting\"}");
  WiFi.disconnect(false, false);
  startWifi();
}

void saveOtaPasswordFromPortal() {
  const String password = webServer.arg("password");
  if (password.length() < OTA_PASSWORD_MIN_LENGTH || password.length() > OTA_PASSWORD_MAX_LENGTH) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"password_length_must_be_8_to_63\"}");
    return;
  }
  if (!saveOtaConfiguration(password)) {
    webServer.send(500, "application/json", "{\"ok\":false,\"error\":\"save_failed\"}");
    return;
  }
  if (otaReady) {
    ArduinoOTA.end();
    otaReady = false;
  }
  startOtaIfReady();
  webServer.send(200, "application/json", "{\"ok\":true,\"otaPassword\":\"configured\"}");
}

void runQcAction() {
  const String action = webServer.arg("name");
  serviceHeartbeat();
  if (action == "i2c-scan") {
    const String result = scanI2cJson();
    Serial.println(result);
    webServer.send(200, "application/json", result);
  } else if (action == "gpio-snapshot") {
    const String result = gpioSnapshotJson();
    Serial.println(result);
    webServer.send(200, "application/json", result);
  } else if (action == "heartbeat-test") {
    heartbeatReady = pulseHeartbeat();
    lastHeartbeatMs = millis();
    webServer.send(200, "application/json", heartbeatReady
      ? "{\"ok\":true,\"heartbeat\":\"pulse-sent\"}"
      : "{\"ok\":false,\"heartbeat\":\"failed\"}");
  } else if (action == "led-start") {
    webServer.send(200, "application/json", startLedTestJson());
  } else if (action == "led-stop") {
    webServer.send(200, "application/json", stopLedTestJson());
  } else if (action == "led-manual") {
    // Invalid or missing led numbers are rejected by manualLedJson itself, so
    // the portal always receives an explicit ok/error verdict.
    const int ledNumber = webServer.arg("led").toInt();
    webServer.send(200, "application/json", manualLedJson(ledNumber));
  } else if (action == "led-all-off") {
    webServer.send(200, "application/json", manualLedsOffJson());
  } else if (action == "aht10-retry") {
    restartAht10();
    webServer.send(200, "application/json", "{\"ok\":true,\"aht10\":\"restarting\"}");
  } else if (action == "boot-arm") {
    armButtonTracker(bootButton);
    webServer.send(200, "application/json", "{\"ok\":true,\"button\":\"boot\",\"armed\":true}");
  } else if (action == "change-display-arm") {
    armButtonTracker(changeDisplayButton);
    webServer.send(200, "application/json", "{\"ok\":true,\"button\":\"changeDisplay\",\"armed\":true}");
  } else if (action == "ethernet-start") {
    webServer.send(200, "application/json", startEthernetQcJson());
  } else if (action == "ethernet-stop") {
    webServer.send(200, "application/json", stopEthernetQcJson());
  } else if (action == "sd-start") {
    webServer.send(200, "application/json", startSdQcJson());
  } else if (action == "oled-refresh") {
    renderOledStatus();
    webServer.send(200, "application/json", oledReady
      ? "{\"ok\":true,\"oled\":\"refreshed\"}"
      : "{\"ok\":false,\"oled\":\"not-ready\"}");
  } else if (action == "wifi-reconnect") {
    if (wifiSsid.length()) {
      WiFi.reconnect();
      lastWifiAttemptMs = millis();
      webServer.send(202, "application/json", "{\"ok\":true,\"wifi\":\"reconnecting\"}");
    } else {
      webServer.send(409, "application/json", "{\"ok\":false,\"wifi\":\"not-provisioned\"}");
    }
  } else {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown_control\"}");
  }
  serviceHeartbeat();
}

void startQcPortal() {
  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFU);
  char ssid[20];
  snprintf(ssid, sizeof(ssid), "TMM-M0-%06lX", static_cast<unsigned long>(suffix));
  qcApSsid = ssid;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(qcApSsid.c_str())) {
    Serial.println(F("{\"qc\":{\"ready\":false,\"error\":\"soft_ap_failed\"}}"));
    return;
  }

  dnsServer.start(53, "*", WiFi.softAPIP());
  webServer.on("/", HTTP_GET, sendQcPortal);
  webServer.on("/api/status", HTTP_GET, []() {
    webServer.sendHeader(F("Cache-Control"), F("no-store"));
    webServer.send(200, "application/json", qcStatusJson());
  });
  webServer.on("/api/action", HTTP_POST, runQcAction);
  webServer.on("/api/wifi/scan", HTTP_POST, startWifiScan);
  webServer.on("/api/wifi/scan", HTTP_GET, sendWifiScanResult);
  webServer.on("/api/wifi/connect", HTTP_POST, connectWifiFromPortal);
  webServer.on("/api/ota/password", HTTP_POST, saveOtaPasswordFromPortal);
  webOta.begin(webServer, otaPassword, serviceHeartbeat);
  webServer.on("/generate_204", HTTP_ANY, redirectToQcPortal);
  webServer.on("/hotspot-detect.html", HTTP_ANY, redirectToQcPortal);
  webServer.on("/ncsi.txt", HTTP_ANY, redirectToQcPortal);
  webServer.on("/connecttest.txt", HTTP_ANY, redirectToQcPortal);
  webServer.onNotFound(sendQcPortal);
  webServer.begin();
  qcPortalReady = true;
  Serial.printf("{\"qc\":{\"ready\":true,\"ssid\":\"%s\",\"url\":\"http://%s/\",\"security\":\"open-bench-only\"}}\n",
    qcApSsid.c_str(), WiFi.softAPIP().toString().c_str());
  renderOledStatus();
}

void serviceQcPortal() {
  if (!qcPortalReady) return;
  serviceHeartbeat();
  dnsServer.processNextRequest();
  serviceHeartbeat();
  webServer.handleClient();
  serviceHeartbeat();
}

void startWifi() {
  if (!wifiSsid.length()) {
    Serial.println(F("{\"wifi\":{\"provisioned\":false,\"connected\":false}}"));
    renderOledStatus();
    return;
  }
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  lastWifiAttemptMs = millis();
  renderOledStatus();
}

void serviceWifi() {
  const wl_status_t status = WiFi.status();
  if (status != lastWifiStatus) {
    lastWifiStatus = status;
    printWifiStatus();
  }
  if (!wifiSsid.length()) return;
  if (status == WL_CONNECTED) return;
  const uint32_t now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;
  WiFi.reconnect();
  lastWifiAttemptMs = now;
}

void printBlockedDrivers() {
  Serial.println(F("{\"blocked\":[\"MCP1 assumed MCP23017 for B4 heartbeat only\",\"MCP2 exact part unknown\",\"Ethernet controller unknown\",\"RS485 direction and protocol unknown\",\"LoRa baud and protocol unknown\",\"ATtiny voltage conflict unresolved\"]}"));
}

void printHelp() {
  Serial.println(F("Commands: profile, wifi, wifi-set <ssid>|<password>, wifi-clear, ota, ota-set <password>, ota-clear, oled, qc, heartbeat, scan-i2c, check-i2c, gpio, blocked, help"));
}

void runCommand(String command) {
  command.trim();
  if (command == "profile") return printProfile();
  if (command == "wifi") return printWifiStatus();
  if (command == "ota") {
    Serial.printf("{\"ota\":{\"configured\":%s,\"ready\":%s,\"hostname\":\"tmm-v6-r0-m0\"}}\n", otaPassword.length() >= OTA_PASSWORD_MIN_LENGTH ? "true" : "false", otaReady ? "true" : "false");
    return;
  }
  if (command.startsWith("ota-set ")) {
    const String password = command.substring(8);
    if (password.length() < OTA_PASSWORD_MIN_LENGTH || password.length() > OTA_PASSWORD_MAX_LENGTH) {
      Serial.println(F("{\"error\":\"ota_password_length\"}"));
      return;
    }
    if (!saveOtaConfiguration(password)) {
      Serial.println(F("{\"error\":\"ota_save_failed\"}"));
      return;
    }
    if (otaReady) ArduinoOTA.end();
    otaReady = false;
    startOtaIfReady();
    Serial.println(F("{\"ota\":{\"configured\":true}}"));
    return;
  }
  if (command == "ota-clear") {
    Serial.println(clearOtaConfiguration()
      ? F("{\"ota\":{\"configured\":false,\"ready\":false}}")
      : F("{\"error\":\"ota_clear_failed\"}"));
    return;
  }
  if (command == "oled") {
    Serial.printf("{\"oled\":{\"ready\":%s,\"address\":\"0x%02X\"}}\n", oledReady ? "true" : "false", oledAddress);
    renderOledStatus();
    return;
  }
  if (command == "qc") {
    Serial.printf("{\"qc\":{\"ready\":%s,\"ssid\":\"%s\",\"apIp\":\"%s\",\"clients\":%u}}\n",
      qcPortalReady ? "true" : "false", qcApSsid.c_str(), WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum());
    return;
  }
  if (command.startsWith("wifi-set ")) {
    const String credentials = command.substring(9);
    const int separator = credentials.indexOf('|');
    if (separator <= 0) {
      Serial.println(F("{\"error\":\"wifi_set_format\"}"));
      return;
    }
    const String ssid = credentials.substring(0, separator);
    const String password = credentials.substring(separator + 1);
    if (ssid.length() > WIFI_SSID_MAX_LENGTH || password.length() > WIFI_PASSWORD_MAX_LENGTH) {
      Serial.println(F("{\"error\":\"wifi_credentials_too_long\"}"));
      return;
    }
    if (!saveWifiConfiguration(ssid, password)) {
      Serial.println(F("{\"error\":\"wifi_save_failed\"}"));
      return;
    }
    WiFi.disconnect(false, false);
    startWifi();
    Serial.println(F("{\"wifi\":{\"provisioned\":true,\"connecting\":true}}"));
    return;
  }
  if (command == "wifi-clear") {
    Serial.println(clearWifiConfiguration()
      ? F("{\"wifi\":{\"provisioned\":false,\"connected\":false}}")
      : F("{\"error\":\"wifi_clear_failed\"}"));
    return;
  }
  if (command == "heartbeat") {
    Serial.printf("{\"heartbeat\":{\"ready\":%s,\"intervalMs\":%lu}}\n", heartbeatReady ? "true" : "false", HEARTBEAT_INTERVAL_MS);
    return;
  }
  if (command == "scan-i2c") return scanI2c();
  if (command == "check-i2c") return checkExpectedI2c();
  if (command == "gpio") return printGpioSnapshot();
  if (command == "blocked") return printBlockedDrivers();
  if (command == "help" || command.length() == 0) return printHelp();
  Serial.println(F("{\"error\":\"unknown_command\"}"));
}

void configurePassiveInputs() {
  const int inputPins[] = {
    LORA_ACK, LORA_AUX, LORA_LINK, LORA_M0, LORA_M1,
    SELECTOR_DMM, SELECTOR_MMA, SELECTOR_WEBSERVER,
    CHANGE_DISPLAY, BOOT_BUTTON, ETH_INT
  };
  for (int pin : inputPins) pinMode(pin, INPUT);
  // The level each button holds at boot becomes its idle baseline; no
  // electrical active polarity is assumed.
  initializeButtonTracker(bootButton);
  initializeButtonTracker(changeDisplayButton);
}

}  // namespace

void setup() {
  Wire.begin(I2C_SDA, I2C_SCL);
  heartbeatReady = initializeHeartbeat();

  Serial.begin(115200);
  configurePassiveInputs();
  pinMode(SD_CS, OUTPUT);
  pinMode(ETH_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  digitalWrite(ETH_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  printProfile();
  Serial.printf("{\"heartbeat\":{\"ready\":%s,\"intervalMs\":%lu}}\n", heartbeatReady ? "true" : "false", HEARTBEAT_INTERVAL_MS);
  loadWifiConfiguration();
  loadOtaConfiguration();
  initializeOled();
  startQcPortal();
  startWifi();
  printHelp();
}

void loop() {
  serviceHeartbeat();
  serviceButtons();
  serviceAht10();
  serviceWifi();
  serviceQcPortal();
  serviceOta();
  serviceLedTest();
  serviceEthernetQc();
  serviceSdQc();
  webOta.serviceRestart();
  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' || character == '\r') {
      if (commandBuffer.length()) runCommand(commandBuffer);
      commandBuffer = "";
      continue;
    }
    if (commandBuffer.length() < 128) commandBuffer += character;
  }
}
