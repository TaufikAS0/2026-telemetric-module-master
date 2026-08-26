#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>

#include "tmm_v6_r0_m0_pins.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#error "Copy wifi_secrets.example.h to wifi_secrets.h and fill in the local Wi-Fi credentials."
#endif

using namespace tmm_m0;

namespace {

String commandBuffer;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr uint8_t MCP23017_IODIRB = 0x01;
constexpr uint8_t MCP23017_OLATB = 0x15;

bool heartbeatReady = false;
uint32_t lastHeartbeatMs = 0;
uint32_t lastWifiAttemptMs = 0;
wl_status_t lastWifiStatus = WL_NO_SHIELD;

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
  Serial.printf("{\"wifi\":{\"connected\":%s", connected ? "true" : "false");
  if (connected) {
    Serial.printf(",\"ip\":\"%s\",\"rssi\":%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  Serial.println(F("}}"));
}

void startWifi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(TMM_WIFI_SSID, TMM_WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
}

void serviceWifi() {
  const wl_status_t status = WiFi.status();
  if (status != lastWifiStatus) {
    lastWifiStatus = status;
    printWifiStatus();
  }
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
  Serial.println(F("Commands: profile, wifi, heartbeat, scan-i2c, check-i2c, gpio, blocked, help"));
}

void runCommand(String command) {
  command.trim();
  if (command == "profile") return printProfile();
  if (command == "wifi") return printWifiStatus();
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
  startWifi();
  printHelp();
}

void loop() {
  serviceHeartbeat();
  serviceWifi();
  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' || character == '\r') {
      if (commandBuffer.length()) runCommand(commandBuffer);
      commandBuffer = "";
      continue;
    }
    if (commandBuffer.length() < 96) commandBuffer += character;
  }
}
