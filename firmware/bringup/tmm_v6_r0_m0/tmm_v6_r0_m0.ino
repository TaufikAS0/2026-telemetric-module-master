#include <Arduino.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <WebServer.h>
#include <Wire.h>
#include <WiFi.h>

#include "tmm_v6_r0_m0_pins.h"

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
  if (!wifiSsid.length()) {
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

const char QC_PORTAL_HTML[] PROGMEM = R"QC_HTML(
<!doctype html><html lang="id"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TMM M0 QC</title><style>
:root{font-family:system-ui;color:#e8eef5;background:#0b1118}body{max-width:760px;margin:auto;padding:20px}h1{margin-bottom:4px}.sub{color:#91a4b7;margin-top:0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}.card{background:#16212d;border:1px solid #293949;border-radius:12px;padding:15px}.row{display:flex;justify-content:space-between;gap:12px;margin:7px 0}.v{font-family:monospace;text-align:right}.ok{color:#5ee18a}.bad{color:#ffbd66}button{width:100%;margin-top:9px;padding:11px;border:0;border-radius:8px;background:#2b78df;color:white;font-weight:700}#msg{min-height:24px;color:#91a4b7}.warn{color:#ffbd66;font-size:.9rem}</style></head>
<body><h1>TMM V6 R0 M0</h1><p class="sub">QC bring-up portal</p><div class="grid">
<section class="card"><b>Keselamatan</b><div class="row"><span>Heartbeat</span><span id="hb" class="v">...</span></div><div class="row"><span>Interval</span><span id="hi" class="v">...</span></div></section>
<section class="card"><b>Jaringan</b><div class="row"><span>Hotspot</span><span id="ap" class="v">...</span></div><div class="row"><span>AP IP / klien</span><span id="api" class="v">...</span></div><div class="row"><span>LAN Wi-Fi</span><span id="sta" class="v">...</span></div><div class="row"><span>LAN IP / RSSI</span><span id="lan" class="v">...</span></div></section>
<section class="card"><b>Perangkat</b><div class="row"><span>OLED</span><span id="oled" class="v">...</span></div><div class="row"><span>OTA</span><span id="ota" class="v">...</span></div></section>
<section class="card"><b>Kontrol aman</b><button onclick="act('i2c-scan')">Scan I2C</button><button onclick="act('oled-refresh')">Refresh OLED</button><button onclick="act('wifi-reconnect')">Reconnect Wi-Fi</button></section>
</div><p id="msg"></p><p class="warn">Hotspot dan HTTP ini terbuka untuk QC meja kerja. Jangan gunakan pada jaringan produksi.</p>
<script>
const q=id=>document.getElementById(id), yn=v=>v?'<span class="ok">READY</span>':'<span class="bad">NOT READY</span>';
async function refresh(){try{let s=await(await fetch('/api/status',{cache:'no-store'})).json();q('hb').innerHTML=yn(s.heartbeat.ready);q('hi').textContent=s.heartbeat.intervalMs+' ms';q('ap').textContent=s.network.apSsid;q('api').textContent=s.network.apIp+' / '+s.network.clients;q('sta').innerHTML=yn(s.network.connected);q('lan').textContent=s.network.connected?s.network.lanIp+' / '+s.network.rssi+' dBm':'offline';q('oled').innerHTML=yn(s.oled.ready);q('ota').innerHTML=yn(s.ota.ready)}catch(e){q('msg').textContent='Status gagal dibaca'}}
async function act(name){q('msg').textContent='Menjalankan '+name+'...';try{let r=await fetch('/api/action?name='+encodeURIComponent(name),{method:'POST'});q('msg').textContent=await r.text();refresh()}catch(e){q('msg').textContent='Perintah gagal'}}refresh();setInterval(refresh,2000);
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
  json += F("}}}");
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

void runQcAction() {
  const String action = webServer.arg("name");
  serviceHeartbeat();
  if (action == "i2c-scan") {
    scanI2c();
    webServer.send(200, "text/plain; charset=utf-8", "Scan I2C selesai; hasil ada di log serial.");
  } else if (action == "oled-refresh") {
    renderOledStatus();
    webServer.send(200, "text/plain; charset=utf-8", oledReady ? "OLED diperbarui." : "OLED belum siap.");
  } else if (action == "wifi-reconnect") {
    if (wifiSsid.length()) {
      WiFi.reconnect();
      lastWifiAttemptMs = millis();
      webServer.send(200, "text/plain; charset=utf-8", "Wi-Fi sedang disambungkan ulang.");
    } else {
      webServer.send(409, "text/plain; charset=utf-8", "Wi-Fi belum diprovisikan melalui serial.");
    }
  } else {
    webServer.send(400, "text/plain; charset=utf-8", "Kontrol tidak dikenal.");
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
  serviceWifi();
  serviceQcPortal();
  serviceOta();
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
