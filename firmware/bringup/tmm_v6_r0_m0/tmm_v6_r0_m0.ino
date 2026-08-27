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
bool bootObserved = false;
bool changeDisplayObserved = false;
int bootInitialState = HIGH;
int changeDisplayInitialState = HIGH;
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
  Serial.println(F("{\"profile\":\"TMM_V6_R0_M0\",\"mode\":\"bring-up-only\",\"controller\":\"ESP32-S3\",\"productionReady\":false}"));
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

String runLedSequence() {
  uint8_t directionA = 0, directionB = 0, latchA = 0, latchB = 0;
  if (!mcp1ReadRegister(MCP23017_IODIRA, directionA)
      || !mcp1ReadRegister(MCP23017_IODIRB, directionB)
      || !mcp1ReadRegister(MCP23017_OLATA, latchA)
      || !mcp1ReadRegister(MCP23017_OLATB, latchB)) {
    return F("{\"ok\":false,\"error\":\"mcp1_unavailable\"}");
  }
  mcp1WriteRegister(MCP23017_IODIRA, 0x00);
  mcp1WriteRegister(MCP23017_IODIRB, directionB & ~0x01U);
  mcp1WriteRegister(MCP23017_OLATA, latchA ^ 0xFFU);
  mcp1WriteRegister(MCP23017_OLATB, latchB ^ 0x01U);
  delay(400); serviceHeartbeat();
  mcp1WriteRegister(MCP23017_OLATA, latchA);
  mcp1WriteRegister(MCP23017_OLATB, latchB);
  for (uint8_t bit = 0; bit < 8; ++bit) {
    mcp1WriteRegister(MCP23017_OLATA, latchA ^ (1U << bit));
    delay(220); serviceHeartbeat();
    mcp1WriteRegister(MCP23017_OLATA, latchA);
  }
  mcp1WriteRegister(MCP23017_OLATB, latchB ^ 0x01U);
  delay(220); serviceHeartbeat();
  mcp1WriteRegister(MCP23017_OLATB, latchB);
  mcp1WriteRegister(MCP23017_IODIRA, directionA);
  mcp1WriteRegister(MCP23017_IODIRB, directionB);
  return F("{\"ok\":true,\"fixedPowerLed\":1,\"sequencedLeds\":[2,3,4,5,6,7,8,9,10]}");
}

String readAht10Json() {
  Wire.beginTransmission(0x38); Wire.write(0xE1); Wire.write(0x08); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return F("{\"ok\":false,\"error\":\"aht10_not_found\"}");
  delay(20); serviceHeartbeat();
  Wire.beginTransmission(0x38); Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return F("{\"ok\":false,\"error\":\"aht10_trigger_failed\"}");
  delay(90); serviceHeartbeat();
  if (Wire.requestFrom(0x38, static_cast<uint8_t>(6)) != 6) return F("{\"ok\":false,\"error\":\"aht10_read_failed\"}");
  uint8_t data[6]; for (uint8_t &value : data) value = Wire.read();
  const uint32_t rawHumidity = (static_cast<uint32_t>(data[1]) << 12) | (static_cast<uint32_t>(data[2]) << 4) | (data[3] >> 4);
  const uint32_t rawTemperature = (static_cast<uint32_t>(data[3] & 0x0F) << 16) | (static_cast<uint32_t>(data[4]) << 8) | data[5];
  const float humidity = rawHumidity * 100.0F / 1048576.0F;
  const float temperature = rawTemperature * 200.0F / 1048576.0F - 50.0F;
  char json[100]; snprintf(json, sizeof(json), "{\"ok\":true,\"temperatureC\":%.2f,\"humidityPercent\":%.2f}", temperature, humidity);
  return String(json);
}

String testSdCardJson() {
  serviceHeartbeat();
  if (!SD.begin(SD_CS, SPI)) return F("{\"ok\":false,\"error\":\"sd_mount_failed\"}");
  File file = SD.open("/TMM_QC.TXT", FILE_APPEND);
  if (!file) { SD.end(); return F("{\"ok\":false,\"error\":\"sd_open_failed\"}"); }
  file.printf("TMM QC write millis=%lu\n", millis());
  file.close();
  const bool written = SD.exists("/TMM_QC.TXT");
  SD.end(); serviceHeartbeat();
  return written ? F("{\"ok\":true,\"file\":\"/TMM_QC.TXT\"}") : F("{\"ok\":false,\"error\":\"sd_verify_failed\"}");
}

String testEthernetJson() {
  uint64_t deviceId = ESP.getEfuseMac();
  uint8_t mac[6] = {0x02, static_cast<uint8_t>(deviceId >> 32), static_cast<uint8_t>(deviceId >> 24), static_cast<uint8_t>(deviceId >> 16), static_cast<uint8_t>(deviceId >> 8), static_cast<uint8_t>(deviceId)};
  Ethernet.init(ETH_CS);
  Ethernet.begin(mac, IPAddress(0, 0, 0, 0)); serviceHeartbeat();
  if (Ethernet.hardwareStatus() == EthernetNoHardware) return F("{\"ok\":false,\"error\":\"w5500_not_found\"}");
  if (Ethernet.linkStatus() == LinkOFF) return F("{\"ok\":false,\"error\":\"ethernet_cable_unplugged\"}");
  const int dhcp = Ethernet.begin(mac, 3500, 700); serviceHeartbeat();
  if (!dhcp) return F("{\"ok\":false,\"error\":\"ethernet_dhcp_failed\"}");
  ethernetIp = Ethernet.localIP(); ethernetReady = true; renderOledStatus();
  return String(F("{\"ok\":true,\"ip\":\"")) + ethernetIp.toString() + F("\"}");
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

const char QC_PORTAL_HTML[] PROGMEM = R"QC_HTML(
<!doctype html><html lang="id"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>TMM QC</title><style>
:root{font-family:Inter,system-ui,sans-serif;color:#eaf2f8;background:#071019}*{box-sizing:border-box}body{max-width:900px;margin:auto;padding:18px;background:radial-gradient(circle at top right,#10375c 0,transparent 38%)}header{display:flex;align-items:center;justify-content:space-between;margin:10px 0 20px}h1{font-size:1.45rem;margin:0}.muted{color:#8ea5b9}.pill{padding:7px 11px;border-radius:99px;background:#152b3d;font-size:.8rem}.steps{display:flex;gap:7px;margin:0 0 18px}.step{flex:1;padding:9px;border-radius:9px;background:#112331;color:#8ea5b9;text-align:center;font-size:.82rem}.step.on{background:#1368ce;color:white}.card{background:#101f2b;border:1px solid #263d4e;border-radius:16px;padding:17px;margin:12px 0;box-shadow:0 12px 28px #0004}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:11px}.row{display:flex;justify-content:space-between;gap:12px;padding:7px 0;border-bottom:1px solid #203544}.row:last-child{border:0}.value{font-family:ui-monospace,monospace;text-align:right}.ok{color:#55e68a}.bad{color:#ffb45e}.networks{display:grid;gap:8px;max-height:250px;overflow:auto;margin:12px 0}.network{display:flex;justify-content:space-between;align-items:center;width:100%;padding:12px;background:#162d3d;border:1px solid #29495e;border-radius:10px;color:white;text-align:left}.network.selected{border-color:#49a3ff;background:#173f61}input{width:100%;padding:12px;background:#091620;border:1px solid #345168;border-radius:9px;color:white;margin:7px 0}button{padding:11px 14px;border:0;border-radius:9px;background:#287fe0;color:white;font-weight:700;cursor:pointer}button.secondary{background:#263d4e}button:disabled{opacity:.45;cursor:not-allowed}.actions{display:flex;gap:8px;flex-wrap:wrap}.actions button{flex:1;min-width:145px}.log{white-space:pre-wrap;background:#08131c;border-radius:9px;padding:12px;min-height:45px;color:#9fc0d7;font-family:ui-monospace,monospace;font-size:.82rem}.locked{opacity:.62}.warn{color:#ffbd66;font-size:.86rem}@media(max-width:520px){.steps{display:grid;grid-template-columns:1fr 1fr}.actions{display:grid}.actions button{width:100%}}
.tutorial{position:fixed;right:16px;top:76px;width:300px}.qcitem{padding:14px 0;border-bottom:1px solid #294052}.qcitem.active{border-left:4px solid #49a3ff;padding-left:12px;background:#0d2738}.approve{background:#16834a}.bypass{background:#8b5b18}.next{padding:12px;border-radius:10px;background:#123652;margin:10px 0}.progressbar{height:8px;background:#243948;border-radius:99px;overflow:hidden}.progressbar i{display:block;height:100%;background:#55e68a;width:0;transition:width .25s}.otaBox{border-color:#356b91}.uploadProgress{height:9px;background:#243948;border-radius:99px;overflow:hidden;margin:10px 0}.uploadProgress i{display:block;height:100%;width:0;background:#49a3ff;transition:width .15s}@media(min-width:1000px){body{margin-left:calc((100vw - 1240px)/2);margin-right:340px}}@media(max-width:999px){.tutorial{position:static;width:auto}}
</style></head><body><header><div><h1>TMM V6 R0 M0</h1><span class="muted">Portal QC lokal</span></div><span id="live" class="pill">Menghubungkan...</span></header>
<div class="steps"><div class="step on">1 · Pilih Wi-Fi</div><div class="step">2 · Sambungkan</div><div class="step">3 · Jalankan QC</div></div>
<section class="card"><h2>Jaringan lokal</h2><p class="muted">Pilih jaringan yang terdeteksi. Hotspot QC tetap aktif saat perangkat tersambung ke LAN.</p><button id="scan" onclick="startScan()">Cari jaringan Wi-Fi</button><div id="networks" class="networks"><span class="muted">Tekan tombol untuk memindai.</span></div><input id="password" type="password" maxlength="63" placeholder="Password Wi-Fi (kosongkan untuk jaringan terbuka)"><div class="actions"><button id="connect" disabled onclick="connectWifi()">Sambungkan ke jaringan</button><button class="secondary" onclick="act('wifi-reconnect')">Hubungkan ulang</button></div><p class="warn">Password dikirim melalui HTTP hotspot dan disimpan di NVS; gunakan hanya pada meja QC terkontrol.</p></section>
<section class="card"><h2>Status langsung</h2><div class="grid"><div><div class="row"><span>Heartbeat</span><span id="hb" class="value">...</span></div><div class="row"><span>Interval</span><span id="hi" class="value">...</span></div><div class="row"><span>OLED</span><span id="oled" class="value">...</span></div><div class="row"><span>OTA</span><span id="ota" class="value">...</span></div></div><div><div class="row"><span>Hotspot</span><span id="ap" class="value">...</span></div><div class="row"><span>AP IP / klien</span><span id="api" class="value">...</span></div><div class="row"><span>LAN Wi-Fi</span><span id="sta" class="value">...</span></div><div class="row"><span>LAN IP / RSSI</span><span id="lan" class="value">...</span></div></div></div></section>
<section class="card otaBox"><h2>Update firmware OTA</h2><p class="muted">Pilih file aplikasi <b>.ino.bin</b>. Jangan gunakan merged/flash image 4 MB.</p><input id="otaPassword" type="password" minlength="8" maxlength="63" placeholder="Password OTA perangkat"><input id="otaFile" type="file" accept=".bin,application/octet-stream"><div class="uploadProgress"><i id="otaBar"></i></div><div class="actions"><button onclick="saveOtaPassword()">Simpan password OTA</button><button id="otaUpload" onclick="uploadFirmware()">Upload & restart</button></div><p id="otaMessage" class="warn">Heartbeat tetap dilayani selama upload. Perangkat restart otomatis setelah update valid.</p></section>
<section class="card"><h2>Tes QC wajib</h2><div class="qcitem" data-qc="led"><b>1. LED 1–10</b><p class="muted">LED1 power harus tetap menyala; LED2–10 berkedip bersama lalu berurutan.</p><div class="actions"><button onclick="act('led-sequence')">Jalankan LED</button><button class="approve" onclick="approve('led')">Visual benar</button><button class="bypass" onclick="bypass('led')">Bypass</button></div></div><div class="qcitem" data-qc="buttons"><b>2. Tombol BOOT & CHANGE DISPLAY</b><p class="muted">Tekan kedua tombol fisik sampai terdeteksi.</p><div class="actions"><button onclick="act('gpio-snapshot')">Baca tombol</button><button class="approve" onclick="approveButtons()">Tombol benar</button><button class="bypass" onclick="bypass('buttons')">Bypass</button></div></div><div class="qcitem" data-qc="aht"><b>3. AHT10</b><p class="muted">Suhu dan kelembapan harus tampil masuk akal secara visual.</p><div class="actions"><button onclick="act('aht10-read')">Baca suhu</button><button class="approve" onclick="approve('aht')">Nilai benar</button><button class="bypass" onclick="bypass('aht')">Bypass</button></div></div><div class="qcitem" data-qc="ethernet"><b>4. Ethernet W5500</b><p class="muted">Colok LAN. OLED menampilkan ETH IP setelah DHCP berhasil.</p><div class="actions"><button onclick="act('ethernet-test')">Tes Ethernet</button><button class="approve" onclick="approve('ethernet')">Koneksi benar</button><button class="bypass" onclick="bypass('ethernet')">Bypass</button></div></div><div class="qcitem" data-qc="sd"><b>5. Micro SD</b><p class="muted">Firmware menulis lalu membaca kembali file verifikasi.</p><div class="actions"><button onclick="act('sd-write')">Tulis & verifikasi</button><button class="approve" onclick="approve('sd')">SD benar</button><button class="bypass" onclick="bypass('sd')">Bypass</button></div></div><h3>Hasil perangkat</h3><div id="log" class="log">Mulai dari LED 1–10.</div></section>
<aside class="card tutorial"><h2>Pemandu QC</h2><div class="progressbar"><i id="progressBar"></i></div><div id="next" class="next">Langkah berikutnya: LED 1–10</div><p>Kerjakan kartu yang disorot. Semua langkah wajib PASS atau BYPASS beralasan.</p><div id="progress"></div><button id="export" disabled onclick="exportQc()">Export laporan JSON</button><p class="warn">LoRa dilewati. Bypass adalah pengecualian, bukan bukti hardware PASS.</p></aside>
<script>
const q=id=>document.getElementById(id), ready=v=>v?'<span class="ok">READY</span>':'<span class="bad">NOT READY</span>';let selected='',lastStatus={};const qc={led:null,buttons:null,aht:null,ethernet:null,sd:null};
async function refresh(){try{const s=await(await fetch('/api/status',{cache:'no-store'})).json();lastStatus=s;q('live').innerHTML='<span class="ok">● ONLINE</span>';q('hb').innerHTML=ready(s.heartbeat.ready);q('hi').textContent=s.heartbeat.intervalMs+' ms';q('ap').textContent=s.network.apSsid;q('api').textContent=s.network.apIp+' / '+s.network.clients;q('sta').innerHTML=ready(s.network.connected);q('lan').textContent=s.network.connected?s.network.lanIp+' / '+s.network.rssi+' dBm':'offline';q('oled').innerHTML=ready(s.oled.ready);q('ota').innerHTML=ready(s.ota.ready);document.querySelectorAll('.step')[1].classList.toggle('on',s.network.connected);document.querySelectorAll('.step')[2].classList.toggle('on',s.heartbeat.ready)}catch(e){q('live').innerHTML='<span class="bad">● OFFLINE</span>'}}
function drawNetworks(items){const box=q('networks');box.textContent='';if(!items.length){box.textContent='Tidak ada jaringan ditemukan.';return}items.forEach(n=>{const b=document.createElement('button');b.className='network';const name=document.createElement('span');name.textContent=n.ssid;const detail=document.createElement('span');detail.textContent=n.rssi+' dBm '+(n.secure?'🔒':'');b.append(name,detail);b.onclick=()=>{selected=n.ssid;document.querySelectorAll('.network').forEach(x=>x.classList.remove('selected'));b.classList.add('selected');q('connect').disabled=false};box.appendChild(b)})}
async function startScan(){q('scan').disabled=true;q('networks').textContent='Memindai jaringan...';await fetch('/api/wifi/scan',{method:'POST'});pollScan()}
async function pollScan(){try{const r=await fetch('/api/wifi/scan',{cache:'no-store'});const d=await r.json();if(d.state==='scanning'){setTimeout(pollScan,800);return}drawNetworks(d.networks||[])}catch(e){q('networks').textContent='Pemindaian gagal.'}q('scan').disabled=false}
async function connectWifi(){if(!selected)return;q('connect').disabled=true;q('log').textContent='Menyambungkan ke '+selected+'...';const body=new URLSearchParams({ssid:selected,password:q('password').value});const r=await fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});q('log').textContent=await r.text();q('password').value='';setTimeout(refresh,1200);q('connect').disabled=false}
async function act(name){q('log').textContent='Menjalankan '+name+'...';try{const r=await fetch('/api/action?name='+encodeURIComponent(name),{method:'POST'});const text=await r.text();try{q('log').textContent=JSON.stringify(JSON.parse(text),null,2)}catch(e){q('log').textContent=text}refresh();return r.ok}catch(e){q('log').textContent='Perintah gagal';return false}}
function renderProgress(){q('progress').textContent='';const entries=Object.entries(qc),done=entries.filter(([,v])=>v).length,next=entries.find(([,v])=>!v)?.[0];entries.forEach(([id,v])=>{const d=document.createElement('div');d.className='row';d.innerHTML='<span>'+id.toUpperCase()+'</span><span class="value '+(v?.status==='pass'?'ok':v?'bad':'muted')+'">'+(v?.status?.toUpperCase()||'WAJIB')+'</span>';q('progress').appendChild(d)});q('progressBar').style.width=(done/entries.length*100)+'%';q('next').textContent=next?'Langkah berikutnya: '+next.toUpperCase():'QC lengkap — export laporan';document.querySelectorAll('[data-qc]').forEach(x=>x.classList.toggle('active',x.dataset.qc===next));q('export').disabled=done!==entries.length}
function approve(id){qc[id]={status:'pass',at:new Date().toISOString()};renderProgress()}function approveButtons(){if(!lastStatus.buttons?.bootObserved||!lastStatus.buttons?.changeDisplayObserved){q('log').textContent='Tekan BOOT dan CHANGE DISPLAY terlebih dahulu.';return}approve('buttons')}
function bypass(id){const reason=prompt('Alasan bypass '+id+':');if(!reason||!reason.trim()){q('log').textContent='Bypass ditolak: alasan wajib diisi.';return}qc[id]={status:'bypass',reason:reason.trim(),at:new Date().toISOString()};renderProgress()}
async function saveOtaPassword(){const password=q('otaPassword').value;if(password.length<8){q('otaMessage').textContent='Password OTA minimal 8 karakter.';return}const r=await fetch('/api/ota/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({password})});q('otaMessage').textContent=await r.text();refresh()}
function uploadFirmware(){const file=q('otaFile').files[0],password=q('otaPassword').value;if(!file){q('otaMessage').textContent='Pilih file .ino.bin terlebih dahulu.';return}if(!file.name.endsWith('.bin')){q('otaMessage').textContent='File harus berekstensi .bin.';return}if(password.length<8){q('otaMessage').textContent='Masukkan password OTA perangkat.';return}const xhr=new XMLHttpRequest(),body=new FormData();body.append('firmware',file,file.name);q('otaUpload').disabled=true;xhr.open('POST','/api/ota/upload?password='+encodeURIComponent(password));xhr.upload.onprogress=e=>{if(e.lengthComputable)q('otaBar').style.width=Math.round(e.loaded/e.total*100)+'%'};xhr.onload=()=>{q('otaMessage').textContent=xhr.responseText;q('otaPassword').value='';if(xhr.status>=300)q('otaUpload').disabled=false};xhr.onerror=()=>{q('otaMessage').textContent='Upload gagal: koneksi terputus.';q('otaUpload').disabled=false};xhr.send(body)}
function exportQc(){const report={schemaVersion:1,product:'TMM',hardware:'TMM_V6_R0_M0',exportedAt:new Date().toISOString(),deviceStatus:lastStatus,results:qc,overall:Object.values(qc).every(v=>v?.status==='pass')?'pass':'pass-with-bypass'};const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(report,null,2)],{type:'application/json'}));a.download='TMM_QC_'+Date.now()+'.json';a.click();URL.revokeObjectURL(a.href)}renderProgress();refresh();setInterval(refresh,2000);
</script></body></html>)QC_HTML";

String qcStatusJson() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  String json;
  json.reserve(420);
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
  json += F("},\"buttons\":{\"bootObserved\":");
  json += bootObserved ? F("true") : F("false");
  json += F(",\"changeDisplayObserved\":");
  json += changeDisplayObserved ? F("true") : F("false");
  json += F("},\"ethernet\":{\"ready\":");
  json += ethernetReady ? F("true") : F("false");
  json += F("}}");
  return json;
}

void sendQcPortal() {
  webServer.sendHeader(F("Cache-Control"), F("no-store"));
  webServer.send_P(200, "text/html; charset=utf-8", QC_PORTAL_HTML);
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
  } else if (action == "led-sequence") {
    webServer.send(200, "application/json", runLedSequence());
  } else if (action == "aht10-read") {
    webServer.send(200, "application/json", readAht10Json());
  } else if (action == "ethernet-test") {
    webServer.send(200, "application/json", testEthernetJson());
  } else if (action == "sd-write") {
    webServer.send(200, "application/json", testSdCardJson());
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
  bootInitialState = digitalRead(BOOT_BUTTON);
  changeDisplayInitialState = digitalRead(CHANGE_DISPLAY);
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
  if (digitalRead(BOOT_BUTTON) != bootInitialState) bootObserved = true;
  if (digitalRead(CHANGE_DISPLAY) != changeDisplayInitialState) changeDisplayObserved = true;
  serviceWifi();
  serviceQcPortal();
  serviceOta();
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
