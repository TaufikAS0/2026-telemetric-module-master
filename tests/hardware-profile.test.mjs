import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { test } from "node:test";

const profileUrl = new URL("../hardware/profiles/TMM_V6_R0_M0.json", import.meta.url);
const headerUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0_pins.h", import.meta.url);
const sketchUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0.ino", import.meta.url);

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

test("bring-up sketch stays passive for unverified interfaces", async () => {
  const [header, sketch] = await Promise.all([
    readFile(headerUrl, "utf8"),
    readFile(sketchUrl, "utf8")
  ]);
  assert.match(header, /constexpr int I2C_SDA = 8;/);
  assert.match(header, /constexpr int SD_CS = 47;/);
  assert.match(header, /constexpr int ETH_CS = 10;/);
  assert.doesNotMatch(sketch, /WiFi|W5500|Ethernet\.begin|MCP23017/);
  assert.doesNotMatch(sketch, /Serial1\.begin|Serial2\.begin/);
  assert.match(sketch, /pinMode\(pin, INPUT\)/);
  assert.match(sketch, /digitalWrite\(ETH_CS, HIGH\)/);
});
