import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { test } from "node:test";

const profileUrl = new URL("../hardware/profiles/TMM_V6_R0_M0.json", import.meta.url);
const headerUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0_pins.h", import.meta.url);
const versionHeaderUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0_version.h", import.meta.url);
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
  // LoRa stays fully passive; the only UART started is the operator-triggered
  // RS485 Modbus RTU QC master (D-020).
  assert.doesNotMatch(sketch, /Serial1\.begin/);
  assert.match(sketch, /rs485Serial\.begin\(RS485_BAUD_RATE, SERIAL_8N1, RS485_RX, RS485_TX\)/);
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
  assert.doesNotMatch(sketch, /action == "(?:lora|attiny)/i);
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

function portalPage(sketch) {
  return sketch.match(/R"QC_HTML\(([\s\S]*?)\)QC_HTML"/)?.[1];
}

function portalScript(page) {
  return page?.match(/<script>([\s\S]*?)<\/script>/)?.[1];
}

test("embedded QC portal JavaScript parses successfully", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  assert.ok(page, "QC portal HTML is embedded");
  const script = portalScript(page);
  assert.ok(script, "QC portal script is present");
  assert.doesNotThrow(() => new Function(script));
  assert.match(page, /Update firmware OTA/);
  assert.match(page, /Langkah berikutnya/);
});

test("QC portal renders the version token without embedding a literal version", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  assert.match(sketch, /FIRMWARE_VERSION_TOKEN\[\] = "\{\{FIRMWARE_VERSION\}\}"/);
  assert.match(sketch, /webServer\.sendContent\(FIRMWARE_VERSION, versionLength\)/);
  const occurrences = page.match(/\{\{FIRMWARE_VERSION\}\}/g) ?? [];
  assert.ok(occurrences.length >= 2, "version token appears in meta and header");
  assert.match(page, /<meta name="firmware-version" content="\{\{FIRMWARE_VERSION\}\}">/);
  assert.match(page, /data-fw="\{\{FIRMWARE_VERSION\}\}"/);
  assert.doesNotMatch(page, /v0\.\d+\.\d+/);
  assert.match(page, /firmwareVersion/);
});

test("FIRMWARE_VERSION stringifies numeric macros into a real version string", async () => {
  const header = await readFile(versionHeaderUrl, "utf8");
  // The stringifier expands the token it receives, so the version components
  // must be object-like #define macros. A constexpr variable would stringify
  // to its own identifier — the OTA regression where /api/status reported
  // vFIRMWARE_VERSION_MAJOR.FIRMWARE_VERSION_MINOR.FIRMWARE_VERSION_PATCH.
  const defines = new Map(
    [...header.matchAll(/^\s*#define\s+([A-Za-z_]\w*)\s+(\d+)\s*$/gm)].map((m) => [m[1], m[2]])
  );
  const initializer = header.match(/FIRMWARE_VERSION\[\]\s*=\s*([\s\S]*?);/)?.[1];
  assert.ok(initializer, "FIRMWARE_VERSION initializer is present");
  const expand = (token) =>
    token
      .split(/(\W+)/)
      .map((part) => defines.get(part) ?? part)
      .join("");
  const quoted = initializer.replace(
    /TMM_M0_VERSION_STRINGIFY\s*\(([^()]*)\)/g,
    (_, arg) => JSON.stringify(expand(arg.trim()))
  );
  const rendered = quoted
    .match(/"(?:[^"\\]|\\.)*"/g)
    ?.map((literal) => JSON.parse(literal))
    .join("");
  assert.ok(rendered, "FIRMWARE_VERSION concatenates into one string literal");
  assert.match(rendered, /^v\d+\.\d+\.\d+$/, "rendered version is a real semantic version");
  assert.equal(
    rendered,
    `v${defines.get("TMM_M0_VERSION_MAJOR")}.${defines.get("TMM_M0_VERSION_MINOR")}.${defines.get("TMM_M0_VERSION_PATCH")}`,
    "rendered version matches the header's version macros"
  );
});

test("QC portal drives only the live backend action names", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  for (const action of [
    "led-start", "led-stop", "boot-arm", "change-display-arm",
    "aht10-retry", "ethernet-start", "ethernet-stop", "sd-start", "wifi-reconnect",
    "rs485-start", "rs485-manual", "rs485-stop"
  ]) {
    assert.match(sketch, new RegExp(`action == "${action}"`), `backend handles ${action}`);
    assert.ok(page.includes(`'${action}'`), `portal invokes ${action}`);
  }
  for (const stale of ["led-sequence", "aht10-read", "ethernet-test", "sd-write"]) {
    assert.ok(!page.includes(stale), `stale action ${stale} removed from portal`);
  }
  assert.doesNotMatch(sketch, /bootObserved|changeDisplayObserved/);
});

test("QC portal shows separated arming and press-cycle evidence per button", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  assert.match(page, /Arm BOOT/);
  assert.match(page, /Arm CHANGE DISPLAY/);
  assert.match(page, /id="bootArmed"/);
  assert.match(page, /id="cdArmed"/);
  assert.match(page, /Bukti tekan/);
  assert.match(page, /Bukti siklus penuh/);
  const script = portalScript(page);
  assert.match(script, /fullCycleConfirmed/);
  assert.match(script, /pressCount/);
  assert.match(script, /DITEKAN/);
  assert.match(script, /SIKLUS PENUH/);
});

test("QC portal updates AHT10 automatically and only offers retry as recovery", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  assert.match(page, /tidak ada tombol baca manual/);
  assert.match(page, /id="ahtTemp"/);
  assert.match(page, /id="ahtHum"/);
  assert.match(page, /id="ahtRetry"/);
  const script = portalScript(page);
  assert.match(script, /sampleValid/);
  assert.match(script, /sampleAgeMs/);
  assert.match(script, /stale/);
  assert.doesNotMatch(page, /aht10-read/);
});

test("QC portal visualizes Ethernet and MicroSD stage machines with evidence", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  const script = portalScript(page);
  for (const stage of ["idle", "initializing", "waiting_link", "acquiring_dhcp", "passed", "failed"]) {
    assert.ok(script.includes(`'${stage}'`), `Ethernet stage ${stage} is visualized`);
  }
  for (const stage of ["mounting", "writing", "reading", "cleaning"]) {
    assert.ok(script.includes(`'${stage}'`), `MicroSD stage ${stage} is visualized`);
  }
  for (const field of ["hardwareDetected", "dhcpPassed", "bytesWritten", "testFile", "durationMs", "lastError"]) {
    assert.ok(script.includes(field), `portal renders ${field}`);
  }
  assert.match(sketch, /Ethernet\.hardwareStatus\(\) != EthernetW5500/);
  assert.match(sketch, /SD\.remove\(sdQc\.testFile\)/);
  assert.match(sketch, /sdQcStageName/);
  assert.match(sketch, /ethernetQcStageName/);
});

test("QC portal gates PASS on evidence and forces reasoned bypass through a modal", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  const script = portalScript(page);
  assert.match(script, /function gateReason\(/);
  assert.match(script, /PASS diblokir/);
  assert.match(script, /Tekan Stop untuk menghentikan siklus sebelum PASS/);
  assert.match(script, /penuh belum terbukti/);
  assert.match(script, /function exportQc\(\)/);
  assert.match(script, /pass-with-bypass/);
  assert.match(script, /deviceStatus/);
  assert.match(script, /firmwareVersion/);
  assert.match(script, /decisions/);
  assert.match(page, /id="bypassModal"/);
  assert.match(page, /Bypass ditolak: alasan wajib diisi/);
  assert.doesNotMatch(script, /\bprompt\s*\(/);
  assert.doesNotMatch(script, /innerHTML/);
});

test("QC guide and progress are integrated into the main content", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  assert.match(page, /id="pbarFill"/);
  assert.match(page, /id="pnext"/);
  assert.match(page, /id="pitems"/);
  assert.doesNotMatch(page, /\.tutorial\s*\{[^}]*position:\s*fixed/);
  assert.doesNotMatch(page, /class="tutorial"/);
});

test("QC portal polls /api/status without overlapping fetches", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const script = portalScript(portalPage(sketch));
  assert.match(script, /\/api\/status/);
  assert.match(script, /pollRunning/);
  assert.match(script, /pollWake/);
  assert.match(script, /testActive\(/);
  assert.doesNotMatch(script, /setInterval\(\s*refresh/);
});

test("LED2-LED10 drive is explicitly active-low with one-LED-at-a-time levels", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  // Live bench evidence (v0.1.0 session): the INVERT_SNAPSHOT guess was
  // inverted, so the polarity must now be an explicit active-low drive.
  assert.doesNotMatch(sketch, /INVERT_SNAPSHOT/);
  assert.match(sketch, /enum class LedPolarity : uint8_t \{\s+ACTIVE_LOW\s+\}/);
  assert.match(sketch, /constexpr int LED_ON_LEVEL = LOW;/);
  assert.match(sketch, /constexpr int LED_OFF_LEVEL = HIGH;/);
  // Every level change is one masked pass over both LED-owned ports, so a
  // sequence step or manual click can never light two LEDs.
  assert.match(sketch, /bool ledTestApplyLevels\(int litLed\)/);
  assert.match(sketch, /mcp1WriteRegister\(MCP23017_OLATA, \(latchA & ~LED_TEST_MASK_PORTA\) \| levelA\)/);
  assert.match(sketch, /mcp1WriteRegister\(MCP23017_OLATB, \(latchB & ~LED_TEST_MASK_PORTB\) \| levelB\)/);
  assert.match(sketch, /const char \*ledModeName\(LedMode mode\)/);
  assert.match(sketch, /ledTest\.mode != LedMode::SEQUENCE\) return;/);
  // Restoration drives every LED bit OFF (HIGH) before the pins are handed back.
  assert.match(sketch, /if \(!ledTestApplyLevels\(-1\)\) \{[\s\S]*?mcp1_restore_failed/);
  // Status API reports mode, polarity, and the actual lit LED.
  assert.match(sketch, /,\\"mode\\":\\"/);
  assert.match(sketch, /,\\"polarity\\":\\"active-low\\",\\"activeLed\\":/);
});

test("LED manual mode lights exactly one LED and never touches GPB1-GPB7", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  const script = portalScript(page);
  for (const action of ["led-manual", "led-all-off"]) {
    assert.match(sketch, new RegExp(`action == "${action}"`), `backend handles ${action}`);
    assert.ok(page.includes(`'${action}'`), `portal invokes ${action}`);
  }
  assert.match(sketch, /String manualLedJson\(int ledNumber\)/);
  assert.match(sketch, /String manualLedsOffJson\(\)/);
  assert.match(sketch, /invalid_led_number/);
  assert.match(sketch, /litLed/);
  // LED-owned masks only: PORTA full byte and GPB0; GPB1..GPB7 (heartbeat) stay masked out.
  assert.match(sketch, /constexpr uint8_t LED_TEST_MASK_PORTA = 0xFFU;/);
  assert.match(sketch, /constexpr uint8_t LED_TEST_MASK_PORTB = 0x01U;/);
  // Manual buttons per LED number plus a global all-off button.
  assert.match(page, /id="ledManualBtns"/);
  assert.match(script, /ledManual\(i\)/);
  assert.match(script, /ledAllOff/);
  assert.match(script, /activeLed/);
});

test("AHT10 driver follows the datasheet init order and reports stage diagnostics", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  const script = portalScript(page);
  assert.match(sketch, /AHT10_RESET_COMMAND\[\] = \{0xBA\}/);
  assert.match(sketch, /AHT10_CALIBRATE_COMMAND\[\] = \{0xE1, 0x08, 0x00\}/);
  assert.match(sketch, /AHT10_MEASURE_COMMAND\[\] = \{0xAC, 0x33, 0x00\}/);
  assert.match(sketch, /AHT10_STATUS_REGISTER = 0x71/);
  assert.match(sketch, /AHT10_STATUS_CALIBRATED_BIT = 0x08/);
  assert.match(sketch, /AHT10_POWERUP_DELAY_MS = 100;/);
  assert.match(sketch, /bool aht10ReadStatus\(\)/);
  assert.match(sketch, /case Aht10Stage::PROBE:/);
  assert.match(sketch, /case Aht10Stage::CHECK_CALIBRATION:/);
  assert.match(sketch, /case Aht10Stage::WAIT_CALIBRATION:/);
  // Distinct per-stage errors, including the observed probe failure.
  for (const error of [
    "aht10_not_found", "aht10_reset_failed", "aht10_status_read_failed",
    "aht10_calibrate_failed", "aht10_not_calibrated", "aht10_trigger_failed",
    "aht10_read_failed", "aht10_busy_timeout", "aht10_range_unplausible"
  ]) {
    assert.match(sketch, new RegExp(`"${error}"`), `AHT10 reports ${error}`);
  }
  // Diagnostics surface: stage, calibration, raw status byte, error count.
  assert.match(sketch, /,\\"stage\\":\\"/);
  assert.match(sketch, /,\\"calibrated\\":/);
  assert.match(sketch, /,\\"statusByte\\":/);
  assert.match(sketch, /,\\"errorCount\\":/);
  assert.match(script, /ahtStage/);
  assert.match(script, /statusByte/);
  assert.match(script, /errorCount/);
  assert.match(page, /id="ahtStage"/);
});

test("MicroSD QC probes card presence and reports granular errors and card type", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  const script = portalScript(page);
  assert.match(sketch, /bool sdProbeCardPresent\(\)/);
  assert.match(sketch, /SPI\.transfer\(0x40 \| 0\)/);
  assert.match(sketch, /SPI\.transfer\(0x95\)/);
  assert.match(sketch, /void sdReleaseBus\(\)/);
  assert.match(sketch, /String sdCardTypeName\(uint8_t type\)/);
  assert.match(sketch, /SD\.cardType\(\)/);
  assert.match(sketch, /case SdQcStage::PROBING:/);
  // No card response and a responding card that fails to mount are distinct.
  assert.match(sketch, /failSdQc\(F\("sd_no_card"\), now\)/);
  assert.match(sketch, /failSdQc\(F\("sd_mount_failed"\), now\)/);
  assert.match(sketch, /,\\"cardPresent\\":/);
  assert.match(sketch, /,\\"cardType\\":\\"/);
  assert.ok(script.includes("'probing'"), "probing stage is visualized");
  assert.match(page, /id="sdCard"/);
  assert.match(page, /id="sdType"/);
});

test("QC portal uses a two-column layout with a sticky right-hand guide on desktop", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  const script = portalScript(page);
  assert.match(page, /class="layout"/);
  assert.match(page, /class="colmain"/);
  assert.match(page, /<aside class="side">/);
  assert.match(page, /\.side\{[^}]*position:sticky/);
  assert.match(page, /grid-template-columns:minmax\(0,1fr\) 340px/);
  assert.match(page, /@media\(max-width:920px\)\{\.layout\{grid-template-columns:minmax\(0,1fr\)\}\.side\{position:static;order:-1\}\}/);
  // Progress and tutorial exist exactly once, in the sidebar.
  assert.equal((page.match(/id="card-progress"/g) ?? []).length, 1);
  assert.equal((page.match(/id="card-guide"/g) ?? []).length, 1);
  assert.equal((page.match(/id="pbarFill"/g) ?? []).length, 1);
  assert.equal((page.match(/id="pnext"/g) ?? []).length, 1);
  assert.equal((page.match(/id="pitems"/g) ?? []).length, 1);
  // Secondary disclosures are not duplicated either.
  assert.equal((page.match(/Jaringan Wi-Fi \(provisioning\)/g) ?? []).length, 1);
  assert.equal((page.match(/Update firmware OTA/g) ?? []).length, 1);
  // Manual LED controls are rendered from the LED card, not a second section.
  assert.equal((page.match(/id="ledManualBtns"/g) ?? []).length, 1);
  // Poll cadence tracks the 300 ms LED step.
  assert.match(script, /ledActive\(/);
  assert.match(script, /ledActive\(lastStatus\)\?300:\(testActive\(lastStatus\)\?1000:2000\)/);
});

test("RS485 Modbus RTU QC ports the Longhi bench tester onto the workbook pins", async () => {
  const [header, sketch] = await Promise.all([
    readFile(headerUrl, "utf8"),
    readFile(sketchUrl, "utf8")
  ]);
  const page = portalPage(sketch);
  const script = portalScript(page);
  // Workbook mapping: module TX1 -> ESP RX GPIO17, module RX1 -> ESP TX GPIO18.
  assert.match(header, /constexpr int RS485_RX = 17;/);
  assert.match(header, /constexpr int RS485_TX = 18;/);
  // Longhi bench configuration carried over: 9600 8N1, poll 0x03, 300 ms
  // engine interval, 200 ms response timeout, PASS after 3 valid responses.
  assert.match(sketch, /constexpr uint32_t RS485_BAUD_RATE = 9600;/);
  assert.match(sketch, /constexpr uint32_t RS485_POLL_INTERVAL_MS = 300;/);
  assert.match(sketch, /constexpr uint32_t RS485_RESPONSE_TIMEOUT_MS = 200;/);
  assert.match(sketch, /constexpr uint8_t RS485_PASS_STREAK = 3;/);
  assert.match(sketch, /uint16_t rs485Crc16\(/);
  assert.match(sketch, /0xA001U/);
  assert.match(sketch, /frame\[index\+\+\] = 0x03;/);
  assert.match(sketch, /HardwareSerial rs485Serial\(2\);/);
  assert.match(sketch, /void serviceRs485Qc\(\)/);
  assert.match(sketch, /serviceSdQc\(\);\s*serviceRs485Qc\(\);/);
  for (const error of [
    "rs485_timeout", "rs485_crc_mismatch", "rs485_slave_id_mismatch",
    "rs485_modbus_exception", "rs485_frame_too_short"
  ]) {
    assert.match(sketch, new RegExp(`"${error}"`), `RS485 reports ${error}`);
  }
  assert.match(sketch, /rs485_invalid_params/);
  // Status surface and QC gating: streaks, last value, TX/RX hex evidence.
  assert.match(sketch, /,\\"rs485\\":\{\\"running\\":/);
  assert.match(sketch, /,\\"pass\\":/);
  assert.match(sketch, /,\\"lastValue\\":/);
  assert.match(sketch, /,\\"successStreak\\":/);
  assert.match(sketch, /,\\"lastTx\\":\\"/);
  // Portal: sixth mandatory QC card with decision gating.
  assert.equal((page.match(/id="card-rs485"/g) ?? []).length, 1);
  assert.match(page, /id="rsValue"/);
  assert.match(page, /id="rsStreak"/);
  assert.match(page, /id="rs485Pass"/);
  assert.match(script, /renderRs485\(/);
  assert.match(script, /rs485:s\.rs485/);
  assert.match(script, /id==='rs485'/);
  assert.match(script, /RS485_STAGES=\['stopped','polling','passed','failed'\]/);
  assert.match(script, /s\.rs485&&s\.rs485\.running/);
  assert.match(page, /Langkah 6 · RS485:<\/b>/);
  assert.match(page, /keenam item diputuskan/);
});

test("QC tutorial steps are clickable and show live step state for operators", async () => {
  const sketch = await readFile(sketchUrl, "utf8");
  const page = portalPage(sketch);
  const script = portalScript(page);
  // Every tutorial step is bound to its QC card via data-step.
  for (const id of ["led", "buttons", "aht", "ethernet", "sd", "rs485"]) {
    assert.match(page, new RegExp(`data-step="${id}"`), `tutorial step for ${id}`);
  }
  assert.match(page, /id="guideList"/);
  assert.match(script, /function focusCard\(/);
  assert.match(script, /scrollIntoView\(\{behavior:'smooth'/);
  assert.match(script, /li\.dataset\.step/);
  assert.match(script, /SEKARANG/);
  assert.match(script, /SELESAI/);
  assert.match(script, /row\.onclick=\(\)=>focusCard\(id\)/);
  assert.match(script, /pn\.onclick=/);
  // Live running-test indicator so the operator knows what is in progress.
  assert.match(page, /id="pnow"/);
  assert.match(script, /function runningLabel\(/);
  assert.match(script, /Sedang berjalan: /);
  assert.match(script, /RS485 polling '\+\(\(s\.rs485\.successStreak\|\|0\)\+'\/3 valid'\)/);
  // Highlight animation styles exist exactly once.
  assert.equal((page.match(/@keyframes navflash/g) ?? []).length, 1);
  assert.match(page, /\.card\.navflash\{animation:navflash/);
});

test("firmware version is bumped to at least v0.3.0 for the RS485 QC feature", async () => {
  const header = await readFile(versionHeaderUrl, "utf8");
  const macros = new Map(
    [...header.matchAll(/^\s*#define\s+(TMM_M0_VERSION_(?:MAJOR|MINOR|PATCH))\s+(\d+)\s*$/gm)].map((m) => [m[1], Number(m[2])])
  );
  assert.equal(macros.get("TMM_M0_VERSION_MAJOR"), 0);
  assert.ok(macros.get("TMM_M0_VERSION_MINOR") >= 3, "minor version carries the RS485 QC feature");
  assert.ok(macros.get("TMM_M0_VERSION_PATCH") >= 0);
});
