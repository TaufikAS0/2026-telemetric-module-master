import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { test } from "node:test";

const profileUrl = new URL("../hardware/profiles/TMM_V6_R0_M0.json", import.meta.url);
const headerUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0_pins.h", import.meta.url);
const sketchUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0.ino", import.meta.url);
const webOtaUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_web_ota.h", import.meta.url);
const gitignoreUrl = new URL("../.gitignore", import.meta.url);

async function loadProfile() {
  return JSON.parse(await readFile(profileUrl, "utf8"));
}

test("M0 hardware profile records its evidence and remains bring-up only", async () => {
  const profile = await loadProfile();
  assert.equal(profile.profileId, "TMM_V6_R0_M0");
  assert.equal(profile.controllers.master.family, "ESP32-S3");
  assert.equal(profile.controllers.supervisor.part, "ATtiny404");
  assert.equal(profile.status, "bring-up-only");
  assert.equal(profile.evidence.sheetGid, 638455439);
  assert.equal(profile.controllers.master.exactPartNumber, null);
  assert.equal(profile.expanders.MCP1.exactPart, "MCP23017");
  assert.equal(profile.buses.i2c.devices.find((device) => device.label === "AHT").exactPart, "AHT10");
  assert.equal(profile.buses.spi.devices.find((device) => device.label === "ETHERNET").exactController, "W5500");
  assert.ok(profile.unknownsBlockingProduction.length >= 8);
});

test("M0 bus pin map matches the engineering workbook", async () => {
  const { buses } = await loadProfile();
  assert.deepEqual(buses.i2c, {
    sda: 8,
    scl: 9,
    devices: buses.i2c.devices
  });
  assert.deepEqual(
    buses.i2c.devices.filter((device) => device.address).map((device) => device.address),
    ["0x20", "0x24", "0x28", "0x38", "0x57", "0x68", "0x77"]
  );
  assert.deepEqual(
    { mosi: buses.spi.mosi, sck: buses.spi.sck, miso: buses.spi.miso },
    { mosi: 11, sck: 12, miso: 13 }
  );
  assert.deepEqual(buses.uart.lora, {
    ...buses.uart.lora,
    espRx: 5,
    espTx: 4,
    baud: null,
    protocol: null
  });
  assert.deepEqual(
    { espRx: buses.uart.rs485.espRx, espTx: buses.uart.rs485.espTx },
    { espRx: 17, espTx: 18 }
  );
});

test("M2 is only a planned feature-disable profile", async () => {
  const profile = await loadProfile();
  assert.deepEqual(profile.modes.M0.disabledFeatures, []);
  assert.equal(profile.modes.M2.inherits, "M0");
  assert.equal(profile.modes.M2.disabledFeatures, null);
});

test("bring-up sketch keeps unverified field buses passive", async () => {
  const [header, sketch] = await Promise.all([
    readFile(headerUrl, "utf8"),
    readFile(sketchUrl, "utf8")
  ]);
  assert.match(header, /constexpr int I2C_SDA = 8;/);
  assert.match(header, /constexpr int SD_CS = 47;/);
  assert.match(header, /constexpr int ETH_CS = 10;/);
  assert.match(sketch, /Ethernet\.begin/);
  assert.doesNotMatch(sketch, /Serial1\.begin|Serial2\.begin/);
  assert.match(sketch, /pinMode\(pin, INPUT\)/);
  assert.match(sketch, /digitalWrite\(ETH_CS, HIGH\)/);
});

test("MCP1B4 heartbeat starts before Wi-Fi and stays below the reset window", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  assert.match(sketch, /constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;/);
  assert.match(sketch, /MCP23017_IODIRB = 0x01/);
  assert.match(sketch, /MCP23017_OLATB = 0x15/);
  assert.match(sketch, /delayMicroseconds\(100\)/);
  assert.ok(sketch.indexOf("heartbeatReady = initializeHeartbeat()") < sketch.indexOf("startWifi()"));
  assert.match(sketch, /void loop\(\) \{\s+serviceHeartbeat\(\);[\s\S]*?serviceWifi\(\);/);
  assert.doesNotMatch(sketch, /while\s*\(\s*WiFi\.status\(\)/);
});

test("Wi-Fi credentials are provisioned at runtime and excluded from the build source", async () => {
  const [sketch, gitignore] = await Promise.all([
    readFile(sketchUrl, "utf8"),
    readFile(gitignoreUrl, "utf8")
  ]);
  assert.match(sketch, /#include <Preferences\.h>/);
  assert.match(sketch, /preferences\.putString\("ssid", ssid\)/);
  assert.match(sketch, /preferences\.putString\("password", password\)/);
  assert.match(sketch, /command\.startsWith\("wifi-set "\)/);
  assert.match(sketch, /command == "wifi-clear"/);
  assert.doesNotMatch(sketch, /wifi_secrets\.h|TMM_WIFI_SSID|TMM_WIFI_PASSWORD/);
  assert.match(gitignore, /firmware\/\*\*\/wifi_secrets\.h/);
});

test("SSD1306 OLED is detected at runtime and shows the connected IP", async () => {
  const [profile, sketch] = await Promise.all([
    loadProfile(),
    readFile(sketchUrl, "utf8")
  ]);
  const oled = profile.buses.i2c.devices.find((device) => device.label === "OLED_SSD1306");
  assert.equal(oled.exactPart, "SSD1306");
  assert.equal(oled.address, null);
  assert.match(sketch, /OLED_I2C_CANDIDATES\[\] = \{0x3C, 0x3D\}/);
  assert.match(sketch, /if \(!i2cResponds\(candidate\)\) continue;/);
  assert.match(sketch, /oledDrawText\(0, 32, WiFi\.localIP\(\)\.toString\(\)\)/);
  assert.match(sketch, /if \(\(offset & 0x7F\) == 0\) serviceHeartbeat\(\)/);
  assert.ok(sketch.indexOf("heartbeatReady = initializeHeartbeat()") < sketch.indexOf("initializeOled()"));
  assert.ok(sketch.indexOf("initializeOled()") < sketch.indexOf("startWifi()"));
});

test("Arduino OTA is password provisioned and keeps the watchdog serviced", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  assert.match(sketch, /#include <ArduinoOTA\.h>/);
  assert.match(sketch, /preferences\.putString\("password", password\)/);
  assert.match(sketch, /ArduinoOTA\.setPassword\(otaPassword\.c_str\(\)\)/);
  assert.match(sketch, /ArduinoOTA\.setHostname\("tmm-v6-r0-m0"\)/);
  assert.match(sketch, /serviceHeartbeat\(\);\s+ArduinoOTA\.handle\(\);\s+serviceHeartbeat\(\);/);
  assert.match(sketch, /ArduinoOTA\.onProgress/);
  assert.match(sketch, /command\.startsWith\("ota-set "\)/);
  assert.doesNotMatch(sketch, /setPassword\("[^\"]+"\)/);
});

test("web OTA is authenticated, watchdog-aware, and isolated as a reusable module", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const webOta = await readFile(webOtaUrl, "utf8");
  assert.match(sketch, /#include "tmm_web_ota\.h"/);
  assert.match(sketch, /webServer\.on\("\/api\/ota\/password", HTTP_POST/);
  assert.match(sketch, /webOta\.begin\(webServer, otaPassword, serviceHeartbeat\)/);
  assert.match(webOta, /server_->arg\("password"\) == \*password_/);
  assert.match(webOta, /Update\.begin\(UPDATE_SIZE_UNKNOWN, U_FLASH\)/);
  assert.match(webOta, /Update\.write\(upload\.buf, upload\.currentSize\)/);
  assert.match(webOta, /service\(\);\s+if \(Update\.write/);
  assert.doesNotMatch(webOta, /password\s*=\s*"[^"]+"/);
});

test("QC portal supports captive AP and LAN access without unsafe drivers", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  assert.match(sketch, /#include <DNSServer\.h>/);
  assert.match(sketch, /#include <WebServer\.h>/);
  assert.match(sketch, /WiFi\.mode\(WIFI_AP_STA\)/);
  assert.match(sketch, /WiFi\.softAP\(qcApSsid\.c_str\(\)\)/);
  assert.match(sketch, /dnsServer\.start\(53, "\*", WiFi\.softAPIP\(\)\)/);
  assert.match(sketch, /"\/generate_204"/);
  assert.match(sketch, /"\/hotspot-detect\.html"/);
  assert.match(sketch, /dnsServer\.processNextRequest\(\);\s+serviceHeartbeat\(\);\s+webServer\.handleClient\(\);\s+serviceHeartbeat\(\);/);
  assert.match(sketch, /action == "i2c-scan"/);
  assert.match(sketch, /action == "oled-refresh"/);
  assert.match(sketch, /action == "wifi-reconnect"/);
  assert.doesNotMatch(sketch, /action == "(?:lora|rs485|attiny)/i);
});

test("QC portal provisions a selected Wi-Fi network and exposes bounded tests", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  assert.match(sketch, /WiFi\.scanNetworks\(true, false\)/);
  assert.match(sketch, /WiFi\.scanComplete\(\)/);
  assert.match(sketch, /webServer\.on\("\/api\/wifi\/scan", HTTP_POST/);
  assert.match(sketch, /webServer\.on\("\/api\/wifi\/scan", HTTP_GET/);
  assert.match(sketch, /webServer\.on\("\/api\/wifi\/connect", HTTP_POST/);
  assert.match(sketch, /saveWifiConfiguration\(ssid, password\)/);
  assert.match(sketch, /action == "heartbeat-test"/);
  assert.match(sketch, /action == "gpio-snapshot"/);
  assert.match(sketch, /serviceHeartbeat\(\);\s+const int16_t count = WiFi\.scanComplete\(\)/);
  assert.doesNotMatch(sketch, /\"password\"\s*:/);
});

test("embedded QC portal JavaScript parses successfully", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = sketch.match(/R"QC_HTML\(([\s\S]*?)\)QC_HTML"/)?.[1];
  assert.ok(page, "QC portal HTML is embedded");
  const script = page.match(/<script>([\s\S]*?)<\/script>/)?.[1];
  assert.ok(script, "QC portal script is present");
  assert.doesNotThrow(() => new Function(script));
  assert.match(page, /Update firmware OTA/);
  assert.match(page, /Langkah berikutnya/);
});

test("mandatory QC covers confirmed peripherals and exports bypass reasons", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  assert.match(sketch, /action == "led-sequence"/);
  assert.match(sketch, /action == "aht10-read"/);
  assert.match(sketch, /action == "ethernet-test"/);
  assert.match(sketch, /action == "sd-write"/);
  assert.match(sketch, /bootObserved/);
  assert.match(sketch, /changeDisplayObserved/);
  assert.match(sketch, /Bypass ditolak: alasan wajib diisi/);
  assert.match(sketch, /function exportQc\(\)/);
  assert.match(sketch, /Ethernet\.linkStatus\(\) == LinkOFF/);
  assert.match(sketch, /SD\.open\("\/TMM_QC\.TXT", FILE_APPEND\)/);
});
