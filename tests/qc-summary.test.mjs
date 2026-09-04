import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import {
  QC_ITEM_IDS,
  QC_MANUAL_REQUIRED,
  QC_STATES,
  buildQcSummary,
  qcItemState
} from "../simulator/qc-summary.mjs";

const inoUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0.ino", import.meta.url);

test("QC summary covers exactly the six mandatory items with the agreed states", () => {
  assert.deepEqual([...QC_ITEM_IDS], ["led", "buttons", "aht10", "ethernet", "sd", "rs485"]);
  assert.deepEqual([...QC_STATES], ["untested", "testing", "pass", "fail", "manual"]);
  assert.deepEqual(QC_MANUAL_REQUIRED, {
    led: true, buttons: true, aht10: false, ethernet: false, sd: false, rs485: false
  });
});

test("LED and buttons are manual observations and never an automated PASS", () => {
  // A completed LED cycle is live evidence for the operator, but the firmware
  // can never watch the LEDs: state must be "manual", never "pass".
  assert.equal(qcItemState("led", { ledTest: { running: false, cyclesCompleted: 3, lastError: "" } }), "manual");
  assert.equal(qcItemState("led", { ledTest: { running: true, cyclesCompleted: 0, lastError: "" } }), "testing");
  assert.equal(qcItemState("led", { ledTest: { running: false, cyclesCompleted: 0, lastError: "" } }), "untested");
  assert.equal(qcItemState("led", { ledTest: { running: false, cyclesCompleted: 1, lastError: "mcp_write_failed" } }), "fail");
  // Confirmed press/release transitions are evidence, but the verdict is human.
  const buttons = { buttons: { boot: { fullCycleConfirmed: true }, changeDisplay: { fullCycleConfirmed: true } } };
  assert.equal(qcItemState("buttons", buttons), "manual");
  assert.equal(qcItemState("buttons", { buttons: { boot: { fullCycleConfirmed: true }, changeDisplay: { fullCycleConfirmed: false } } }), "untested");
});

test("AHT10, Ethernet, SD, and RS485 distinguish untested, testing, pass, and fail", () => {
  assert.equal(qcItemState("aht10", {}), "untested");
  assert.equal(qcItemState("aht10", { aht10: { sampling: false, stage: "idle" } }), "untested");
  assert.equal(qcItemState("aht10", { aht10: { sampling: false, stage: "wait_calibration" } }), "testing");
  assert.equal(qcItemState("aht10", { aht10: { sampling: true, stage: "idle" } }), "testing");
  assert.equal(qcItemState("aht10", { aht10: { sampling: false, stage: "idle", sampleValid: true, stale: false } }), "pass");
  assert.equal(qcItemState("aht10", { aht10: { sampling: false, stage: "idle", sampleValid: true, stale: true } }), "untested",
    "a stale sample is not fresh automated evidence");
  assert.equal(qcItemState("aht10", { aht10: { sampling: false, stage: "idle", sampleValid: false, stale: true, errorCount: 4 } }), "fail");

  assert.equal(qcItemState("ethernet", {}), "untested");
  assert.equal(qcItemState("ethernet", { ethernet: { running: true, stage: "idle" } }), "testing");
  assert.equal(qcItemState("ethernet", { ethernet: { running: false, stage: "acquiring_dhcp" } }), "testing");
  assert.equal(qcItemState("ethernet", { ethernet: { running: false, stage: "passed", testPassed: true } }), "pass");
  assert.equal(qcItemState("ethernet", { ethernet: { running: false, stage: "failed", testPassed: false } }), "fail");

  assert.equal(qcItemState("sd", {}), "untested");
  assert.equal(qcItemState("sd", { sdTest: { running: true, stage: "idle" } }), "testing");
  assert.equal(qcItemState("sd", { sdTest: { running: false, stage: "writing" } }), "testing");
  assert.equal(qcItemState("sd", { sdTest: { running: false, stage: "passed", testPassed: true } }), "pass");
  assert.equal(qcItemState("sd", { sdTest: { running: false, stage: "failed", testPassed: false } }), "fail");

  assert.equal(qcItemState("rs485", {}), "untested");
  assert.equal(qcItemState("rs485", { rs485: { running: true, pass: false } }), "testing");
  assert.equal(qcItemState("rs485", { rs485: { running: false, pass: true } }), "pass");
  assert.equal(qcItemState("rs485", { rs485: { running: false, pass: false, lastError: "rs485_timeout" } }), "fail");
  assert.equal(qcItemState("rs485", { rs485: { running: false, pass: false, lastError: "stopped" } }), "untested",
    "an operator stop is not a failure");
});

test("buildQcSummary emits the same block shape the firmware appends", () => {
  const summary = buildQcSummary({
    ledTest: { running: true, cyclesCompleted: 0, lastError: "" },
    buttons: { boot: { fullCycleConfirmed: false }, changeDisplay: { fullCycleConfirmed: false } },
    aht10: { sampling: false, stage: "idle", sampleValid: false, stale: true, errorCount: 0 },
    ethernet: { running: false, stage: "passed", testPassed: true },
    sdTest: { running: false, stage: "idle", testPassed: false },
    rs485: { running: false, pass: false, lastError: "stopped" }
  });
  assert.equal(summary.items.led.state, "testing");
  assert.equal(summary.items.led.manualRequired, true);
  assert.equal(summary.items.buttons.state, "untested");
  assert.equal(summary.items.aht10.state, "untested");
  assert.equal(summary.items.ethernet.state, "pass");
  assert.equal(summary.items.ethernet.manualRequired, false);
  assert.equal(summary.items.sd.state, "untested");
  assert.equal(summary.items.rs485.state, "untested");
  for (const item of QC_ITEM_IDS) {
    assert.ok(QC_STATES.includes(summary.items[item].state), item);
  }
});

test("the firmware status document carries the QC summary and product identity", async () => {
  const ino = await readFile(inoUrl, "utf8");
  for (const marker of [
    'qcSummary\\":{\\"items\\":{',
    '"manualRequired\\":true',
    '"manualRequired\\":false',
    '"productCode\\":\\"TMM\\"',
    '"hardwareRevision\\":\\"TMM_V6_R0_M0\\"',
    "qcSummaryLedState", "qcSummaryButtonsState", "qcSummaryAht10State",
    "qcSummaryEthernetState", "qcSummarySdState", "qcSummaryRs485State"
  ]) {
    assert.ok(ino.includes(marker), `firmware marker missing: ${marker}`);
  }
  // The firmware never fakes an automated PASS: no summary state constant can
  // produce "pass" for the two manual-only items.
  const ledFn = ino.match(/const char \*qcSummaryLedState\(\) \{[\s\S]*?\n\}/)?.[0] ?? "";
  assert.ok(ledFn.includes("manual") && !ledFn.includes('"pass"'),
    "LED summary state must never auto-PASS");
  const buttonsFn = ino.match(/const char \*qcSummaryButtonsState\(\) \{[\s\S]*?\n\}/)?.[0] ?? "";
  assert.ok(buttonsFn.includes("manual") && !buttonsFn.includes('"pass"'),
    "buttons summary state must never auto-PASS");
});

test("the release is the v0.6.2 Hardware QC build and keeps the proven artifacts contract", async () => {
  const [header, script, buildTest] = await Promise.all([
    readFile(new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0_version.h", import.meta.url), "utf8"),
    readFile(new URL("../scripts/build-bringup.ps1", import.meta.url), "utf8"),
    readFile(new URL("../tests/build-metadata.test.mjs", import.meta.url), "utf8")
  ]);
  assert.match(header, /#define TMM_M0_VERSION_MAJOR 0/);
  assert.match(header, /#define TMM_M0_VERSION_MINOR 6/);
  assert.match(header, /#define TMM_M0_VERSION_PATCH 2/);
  assert.match(script, /esp32:esp32:esp32s3:FlashSize=16M,FlashMode=qio,PartitionScheme=custom/,
    "the TMM ESP32-S3 profile stays untouched (never the TGC classic table)");
  assert.match(buildTest, /TMM-0\.6\.2-0a9113e/, "build metadata test tracks the new version");
});