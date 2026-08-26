import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { test } from "node:test";

const profileUrl = new URL("../hardware/profiles/TMM_V6_R0_M0.json", import.meta.url);
const headerUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0_pins.h", import.meta.url);
const sketchUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0.ino", import.meta.url);
const secretsExampleUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/wifi_secrets.example.h", import.meta.url);
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

test("bring-up sketch stays passive for unverified interfaces except MCP1B4", async () => {
  const [header, sketch] = await Promise.all([
    readFile(headerUrl, "utf8"),
    readFile(sketchUrl, "utf8")
  ]);
  assert.match(header, /constexpr int I2C_SDA = 8;/);
  assert.match(header, /constexpr int SD_CS = 47;/);
  assert.match(header, /constexpr int ETH_CS = 10;/);
  assert.doesNotMatch(sketch, /W5500|Ethernet\.begin/);
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
  assert.match(sketch, /void loop\(\) \{\s+serviceHeartbeat\(\);\s+serviceWifi\(\);/);
  assert.doesNotMatch(sketch, /while\s*\(\s*WiFi\.status\(\)/);
});

test("Wi-Fi credentials remain local and ignored by Git", async () => {
  const [sketch, example, gitignore] = await Promise.all([
    readFile(sketchUrl, "utf8"),
    readFile(secretsExampleUrl, "utf8"),
    readFile(gitignoreUrl, "utf8")
  ]);
  assert.match(sketch, /#include "wifi_secrets\.h"/);
  assert.match(example, /YOUR_WIFI_SSID/);
  assert.match(example, /YOUR_WIFI_PASSWORD/);
  assert.match(gitignore, /firmware\/\*\*\/wifi_secrets\.h/);
});
