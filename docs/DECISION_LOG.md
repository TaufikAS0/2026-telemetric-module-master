# Decision Log

## D-001 — Product-specific repository

- Status: Accepted
- Decision: TMM receives its own repository and independent version history.
- Reason: Product releases, issues, and hardware compatibility must remain traceable.
- Trade-off: Shared behavior must not be duplicated across product repos.

## D-002 — Shared embedded core later

- Status: Accepted in principle
- Decision: Common identity, protocol, logging, diagnostics, and update behavior will live in a separately versioned library.
- Reason: Copying common code into each product creates incompatible forks.
- Unknown: Package mechanism will be selected after the embedded toolchain is confirmed.

## D-003 — Desktop simulation before production firmware

- Status: Accepted
- Decision: Validate state models, APIs, operator workflow, and tests on Windows first.
- Reason: Hardware facts are currently missing.
- Limitation: This does not validate electrical behavior or real-time constraints.

## D-004 — Portal controls approval

- Status: Accepted
- Decision: GitHub stores source and release artifacts; the local hardware portal controls approved/recommended status and QC history.
- Reason: Code publication and hardware release approval are different responsibilities.

## D-005 — M0 passive bring-up before active drivers

- Status: Accepted for MVP bring-up
- Evidence: `Modul Master.xlsx`, tab `TMM_V6_R0_M0`, received 2026-08-26.
- Decision: Capture the verified M0 pin map in a machine-readable profile and begin with passive GPIO reads, I2C discovery, and an operator-triggered SD probe.
- Reason: The workbook confirms bus pins but does not identify several device types or communication contracts.
- Safety boundary: Do not transmit over LoRa/RS485 or write MCP1/MCP2, Ethernet, or ATtiny404 registers until their exact contracts are confirmed or a later bounded decision records a specific bring-up exception.
- Toolchain assumption: The MVP is an Arduino sketch for convenient bring-up only. The production framework remains undecided.
- Consequence: The sketch can establish device presence and wiring continuity but cannot validate every peripheral function.

## D-006 — MCP1B4 watchdog heartbeat precedes Wi-Fi

- Status: Accepted for MVP bring-up
- Hardware evidence: M0 workbook maps `DONE TPL` to MCP1B4; human operator states the board resets when it does not receive a heartbeat within five seconds.
- Component assumption: MCP1 is provisionally treated as MCP23017 at I2C address 0x20 using BANK=0 registers. Confirm from BOM/schematic before powering hardware.
- Decision: Configure MCP1B4 and issue a 100 microsecond DONE pulse every 1000 ms. Initialize it before Serial, SPI, or Wi-Fi.
- Timing evidence: TI TPL5010 specifies a minimum DONE pulse width of 100 ns. The MVP uses a 100 microsecond pulse for margin.
- Wi-Fi rule: Connection and retry logic must remain nonblocking and must never contain a wait-until-connected loop.
- Security rule: Wi-Fi credentials must not be compiled into the sketch or release artifacts.

## D-007 — Runtime Wi-Fi provisioning for bring-up

- Status: Accepted for MVP bring-up only
- Evidence: The generic compiled BIN previously contained the locally supplied SSID and password when `wifi_secrets.h` was included at compile time.
- Decision: Remove compile-time Wi-Fi credentials. Accept `wifi-set <ssid>|<password>` over the local serial console, store the values in ESP32 Preferences/NVS, and provide `wifi-clear` to erase them.
- Watchdog rule: NVS access and Wi-Fi startup remain bounded operations with heartbeat servicing immediately before and after NVS writes. Connection and retry remain nonblocking.
- Disclosure rule: Status output reports only provisioned/connected state, IP address, and RSSI; it never prints the SSID or password.
- Security limitation: Serial provisioning and the NVS partition are not assumed encrypted or production-secure. The production provisioning, device identity, flash-encryption, and credential-rotation contracts remain unknown.
- Consequence: One generic bring-up BIN can be built and distributed without embedding a site credential, while each bench device is configured after flashing.

## D-008 — SSD1306 Wi-Fi/IP bring-up display

- Status: Accepted for MVP bring-up
- Hardware evidence: The M0 workbook identifies an SSD1306 on the GPIO8/GPIO9 I2C bus but leaves its address unknown. Solomon Systech identifies SSD1306 as a 128x64 controller with an I2C interface and internal charge pump.
- Operator request: Turn on the OLED and show the Wi-Fi IP address in addition to the serial log.
- Decision: Probe only the SSD1306 address-select values 0x3C and 0x3D. Initialize and write the display only after one responds; otherwise log `detected:false` and continue without blocking Wi-Fi or heartbeat.
- Watchdog rule: The framebuffer upload services MCP1B4 between bounded I2C chunks.
- Consequence: A connected device shows its IP on OLED and emits it in the serial Wi-Fi status JSON. Orientation and visible output remain bench-test evidence, not compile evidence.

## D-009 — Password-protected Arduino OTA bring-up

- Status: Accepted for MVP bring-up
- Operator request: Allow direct OTA updates after Wi-Fi connection.
- Toolchain evidence: Arduino ESP32 core 3.3.10 includes ArduinoOTA, and the selected default partition table contains `otadata`, `app0`, and `app1` OTA slots.
- Decision: Start ArduinoOTA only after Wi-Fi is connected and a separate 8–63 character password has been provisioned through `ota-set`. Store it in the bring-up NVS namespace; never compile or log it.
- Watchdog rule: Call MCP1B4 heartbeat immediately before and after `ArduinoOTA.handle()` and from OTA progress callbacks.
- Image boundary: Arduino OTA receives the app-only `.ino.bin`; the merged 4 MB image remains a full USB-flash artifact.
- Security limitation: NVS encryption, signed firmware, secure boot, rollback validation, and production credential rotation remain unconfirmed. OTA success and reset timing require a physical board test.

## D-010 — Captive QC portal on AP and station networks

- Status: Accepted for MVP bring-up
- Operator request: Automatically open a local QC web server when an operator joins the device hotspot, and expose it to clients on the same station Wi-Fi network.
- Decision: Run ESP32-S3 in AP+STA mode with a generated `TMM-M0-<MAC suffix>` open hotspot, wildcard DNS, standard captive-portal probe routes, and one port 80 dashboard bound to both interfaces.
- Control boundary: The page may scan I2C, redraw OLED, and request station Wi-Fi reconnect. It must not activate unverified Ethernet, LoRa, RS485, MCP2, or ATtiny interfaces.
- Watchdog rule: Service MCP1B4 before and after DNS and HTTP request handling and inside longer actions.
- Security limitation: The hotspot and HTTP session are unauthenticated and intended only for supervised bench QC. Captive-portal auto-open is client-OS behavior and remains a physical interoperability test.

## D-011 — Portal-first Wi-Fi provisioning and bounded QC workflow

- Status: Accepted for MVP bring-up
- Operator request: Simplify commissioning by selecting a detected Wi-Fi network, connecting to the LAN, and running QC controls from one interactive page.
- Decision: Use the ESP32 asynchronous Wi-Fi scan API so MCP1B4 continues to be serviced while scanning. Accept the selected SSID and password through a bounded HTTP POST, store them with the existing Preferences/NVS routine, and start the existing nonblocking station connection.
- QC scope: Permit heartbeat pulse verification, I2C discovery, passive GPIO snapshot, OLED refresh, and Wi-Fi reconnect. Present Ethernet, LoRa, RS485, MCP2, and ATtiny as locked rather than driving interfaces whose contracts remain unknown.
- Disclosure rule: Scan results may display broadcast SSIDs and RSSI, but status APIs and logs do not echo saved SSIDs or passwords.
- Security limitation: Credentials cross an unauthenticated HTTP hotspot and NVS encryption remains unconfirmed. This workflow is restricted to a supervised bench network.

## D-012 — Mandatory operator QC for confirmed M0 peripherals

- Status: Accepted for bring-up QC
- Human evidence: Operator confirms AHT10, W5500 Ethernet, and MCP1 at 0x20; the engineering workbook maps LED2–LED10 to MCP1A0–A7/B0, SD to SPI CS47, W5500 to SPI CS10, and physical buttons to GPIO0/GPIO42. MCP1 uses the MCP23017 contract already bounded by D-006.
- Decision: Provide a guided mandatory QC sequence for LED visual inspection, physical button transitions, AHT10 temperature/humidity, W5500 link plus DHCP, and SD write verification. LoRa remains skipped.
- Approval rule: Every step must be visually approved or bypassed with a nonempty reason before JSON export is enabled. A bypass is exported as an exception and never converted into hardware PASS.
- LED detail: LED1 is the fixed 3.3 V power indicator. Firmware toggles LED2–LED10 together and sequentially, then restores MCP direction and latch registers.
- Ethernet display: After W5500 link and DHCP succeed, OLED changes to ETH IP mode.
- Evidence boundary: Automated compile/tests do not prove physical QC; exported operator JSON is the evidence record.

## D-013 — Guided portal and authenticated web OTA

- Status: Accepted for bring-up QC
- Observed defect: The live portal could render `OFFLINE` while its status endpoint responded because the JSON response contained one extra closing brace.
- Decision: Correct the status JSON, highlight the next incomplete mandatory QC item, show overall progress, and expose authenticated app-BIN upload beside the existing Arduino network OTA.
- Module boundary: Keep hardware pins in `tmm_v6_r0_m0_pins.h` and isolate the reusable HTTP upload lifecycle in `tmm_web_ota.h`. The product-specific page and QC actions remain in the sketch because they encode the TMM workflow.
- Image boundary: Web OTA accepts only the app `.ino.bin`; merged/full-flash images are not supported. Heartbeat servicing brackets flash writes and the device restarts only after `Update.end(true)` succeeds.
- Evidence boundary: Compilation proves the image fits and links. Portal interaction, update recovery, rollback behavior, and physical QC remain hardware tests.

## D-014 — Embedded firmware semantic version source of truth

- Status: Accepted for MVP bring-up
- Evidence gap: Before this decision no explicit embedded semantic version existed, so flashed `.ino.bin` files and exported QC JSON could not be distinguished after an OTA update (acceptance test step 10–11 requires verifying "the new sketch version" but no version surface was defined).
- Decision: Establish `v0.1.0` as the initial feature-bearing bring-up version, explicitly labeled bring-up/non-production. It is defined exactly once in `firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0_version.h` as numeric components from which the single display string is derived at compile time; every runtime surface consumes that constant only.
- Runtime surfaces: `printProfile()` serial JSON adds `"version"`; `/api/status` adds a top-level `"firmwareVersion"` (carried into the operator QC export via `deviceStatus`); the QC portal page receives the same string by substituting the `{{FIRMWARE_VERSION}}` placeholder while streaming the PROGMEM page in bounded chunks that continue MCP1B4 heartbeat servicing.
- Version separation: The desktop simulator identity remains `package.json` (`0.0.1`). Simulator and firmware versions are independent identities on separate evidence levels (decision D-003); firmware must not read the simulator value and the simulator must not claim to represent embedded firmware state. A matching number would be coincidence, never shared truth.
- RAM boundary: The portal page is streamed from flash-mapped PROGMEM with per-chunk substitution instead of being rebuilt in heap memory, preserving the existing single-copy page layout. Portal structure itself is intentionally unchanged (no redesign in this change).
- Evidence limitation: A compile identifies which sources were built, not whether the build works on hardware. Physical verification of `version`/`firmwareVersion` surfaces, portal rendering after streaming, and post-OTA version checks remain required board tests.

## D-015 — LED2–LED10 are active-low with sequence and manual modes

- Status: Accepted for bring-up QC (v0.2.0)
- Hardware evidence: During the live v0.1.0 bench session (2026-08-28), selecting LED9 as the single "active" output under the INVERT_SNAPSHOT polarity drove LED9 OFF while LED2–LED8 and LED10 all lit. The mapped MCP1 latch bits therefore light their LEDs when driven LOW; the earlier snapshot-based polarity guess was inverted.
- Decision: Drive LED2–LED10 explicitly active-low. OFF is HIGH on every LED-owned latch bit and the active level is exactly one bit LOW at a time. Every level change writes both LED-owned ports in one masked read-modify-write pass, so a sequence step or a manual click is atomic and can never light two LEDs. The sequence loops LED2→LED10 at 300 ms until Stop.
- Manual mode: portal buttons 2–10 and "Semua OFF" call `led-manual` / `led-all-off`. Clicking a number lights exactly that LED (mode `manual`, active LED reported); "Semua OFF" parks all LED bits HIGH. Switching from sequence to manual is allowed and stops the loop.
- Watchdog boundary: GPB4 (heartbeat) and GPB1–GPB7 are never written by the LED module; each port write re-reads OLATA/OLATB first, so heartbeat pulses interleaved with LED writes survive.
- Status surface: `/api/status` `ledTest` reports `mode` (`idle|sequence|manual`), `polarity:"active-low"`, `activeLed` (number or null), `running`, `intervalMs`, `cyclesCompleted`, `lastError`.
- Restoration: Stop and every failure path drive all LED bits HIGH before handing the pins back, then restore the START direction/latch snapshots, so restoring an input direction can never leave an LED lit.
- Consequence: The active-low polarity is now bench-observed evidence for LED2–LED10, but the LED electrical schematic (series resistance, sink current per pin) remains unverified; per-pin current budget is still unknown.

## D-016 — AHT10 datasheet init sequence with stage diagnostics

- Status: Accepted for bring-up QC (v0.2.0)
- Defect evidence: The live v0.1.0 bench session reported `aht10_not_found` persistently at 0x38, although the operator confirms an AHT10 is present. The v0.1.0 driver sent the calibration command as its first ever transaction, with no probe, no reset, no calibration verification, and no power-up settle.
- Decision: Follow the Aosong AHT10 power-up order nonblocking: settle >=100 ms after reset before any bus access, explicit ACK probe at 0x38, soft reset (0xBA) once per boot/operator retry, status read through register 0x71, calibration command (0xE1 0x08 0x00) only while the calibrated status bit (bit3) is clear with a re-check after 20 ms, then triggered measurements (0xAC 0x33 0x00) polled through the busy bit inside a bounded window. Calibration, once confirmed, is trusted for later cycles.
- Diagnostics surface: `/api/status` `aht10` adds `stage` (`idle|probe|wait_reset|check_calibration|wait_calibration|trigger|measure|poll_read`), `calibrated`, `statusByte` (raw hex or null), and `errorCount` beside the existing fields. Errors are distinct per stage: `aht10_not_found`, `aht10_reset_failed`, `aht10_status_read_failed`, `aht10_calibrate_failed`, `aht10_not_calibrated`, `aht10_trigger_failed`, `aht10_read_failed`, `aht10_busy_timeout`, `aht10_range_unplausible`.
- Evidence boundary: No temperature or humidity value is ever fabricated; values come only from a completed, physically plausible conversion and the last valid sample stays visible with its staleness. `aht10-retry` forces the full probe/reset/calibration path.
- Unknown: Whether the 0x38 NACK was a power-up timing issue, a bus electrical issue, or a different AHT part (AHT10 vs AHT20 command sets differ) remains unresolved until this version runs on the bench.

## D-017 — SD QC distinguishes card absence, mount failure, and card type

- Status: Accepted for bring-up QC (v0.2.0)
- Defect evidence: The live v0.1.0 bench session failed in ~3 ms with only `sd_mount_failed`, which cannot distinguish "no card inserted", "card present but mount failed", or "dead signal lines" on the shared SPI bus (GPIO11/12/13, SD CS GPIO47, ETH CS GPIO10).
- Decision: Before `SD.begin`, run a low-level CMD0 (GO_IDLE_STATE) probe at 400 kHz on the shared bus with the W5500 CS held high. No R1 response within the retry budget reports `sd_no_card`; a responding card whose filesystem mount still fails reports `sd_mount_failed`. On success, the SD library's card type is recorded (`none|mmc|sd1|sdhc|unknown`, per the core's `sd_defines.h` enum) as run evidence.
- Safety boundary: The probe runs only while the filesystem is not mounted, releases both chip selects and clocks out extra idle bytes before returning, and services MCP1B4 between retry bursts. The write/readback/delete test is unchanged: one uniquely named 8.3 file per run, exact size and content comparison, removal verified absent, pre-existing files never touched.
- Status surface: `/api/status` `sdTest` adds `cardPresent`, `cardType`, and a `probing` stage beside the existing fields.
- Consequence: A bench operator can now tell whether to re-seat the card, replace it, or suspect the SPI wiring, without a serial console.

## D-018 — Two-column QC portal with sticky guide/progress on desktop

- Status: Accepted for bring-up QC (v0.2.0)
- Operator request: The v0.1.0 page rendered cramped and visually shuffled; the operator wants the tutorial on the right on desktop.
- Decision: One layout: a left content column (five QC cards, log, secondary Wi-Fi/OTA disclosures) and a right sidebar containing the single progress card plus the tutorial/rules card, sticky on desktop (`position:sticky`, top-aligned). Below 920 px the grid collapses to one column and the sidebar moves above the cards (`order:-1`), so mobile shows the guide first with no overlap. Section duplication is removed: progress, guide, log, Wi-Fi, and OTA each exist exactly once.
- Poll cadence: `/api/status` polling is 300 ms while `ledTest.running` (sequence step is 300 ms, so on-screen LED dots track the hardware instead of jumping), 1000 ms while any other QC test runs, and 2000 ms idle; `pollWake` still forces an immediate re-poll after any action and fetches never overlap.
- Manual LED surface: The LED card renders one button per LED 2–10 plus "Semua OFF"; the clicked and the firmware-reported active LED both highlight, so the UI state is always the hardware state from `activeLed`, never a local guess.
- Evidence limitation: Rendering and layout behavior are compile-plus-browser evidence only; the physical LED/button/AHT/Ethernet/SD evidence comes from the operator completing the QC workflow on hardware.

## D-019 — v0.2.0 firmware version bump

- Status: Accepted for bring-up QC
- Evidence: New operator-facing behavior (manual LED mode, AHT10 diagnostics, SD probe stages, portal layout) plus the live v0.1.0 bench session corrections above. Per D-014 the version is defined only in `tmm_v6_r0_m0_version.h` and every runtime surface consumes that constant.
- Decision: Bump the firmware to `v0.2.0` (minor: new manual LED feature). Still explicitly non-production: no completed hardware QC evidence exists yet.
- Consequence: QC JSON exports from this build carry `firmwareVersion: "v0.2.0"` so v0.1.0 bench evidence and v0.2.0 evidence remain distinguishable.

## D-020 — RS485 QC as a Modbus RTU master ported from the Longhi bench tester

- Status: Accepted for bring-up QC (v0.3.0)
- Operator request: Confirm the onboard RS485 works by reusing the proven Modbus tester from the Longhi `esp32s3_hwtest` sketch; the operator attaches only the A/B differential lines to the device under test.
- Hardware evidence: Workbook sheet `TMM_V6_R0_M0` wires the RS485 module with VCC 5V, GND, module TX1 → ESP RX GPIO17, module RX1 → ESP TX GPIO18, and no DE/RE direction line. The Longhi bench used the same 4-wire module class (auto direction, UART2, 9600 8N1) successfully against real Modbus slaves. `tmm_v6_r0_m0_pins.h` already carries exactly this mapping (`RS485_RX=17`, `RS485_TX=18`).
- Decision: Port the Longhi Modbus tester into the TMM sketch as the sixth mandatory QC item. Nonblocking master on UART2 polls read-holding-registers (0x03) every 300 ms with a 200 ms response timeout. Auto mode targets slave 1, register 0, count 1; manual mode accepts slave 1–247, register 0–65535, count 1–125. PASS = 3 consecutive CRC-valid responses (Longhi streak filter); any failure resets the streak and records its reason.
- Diagnostics surface: `/api/status` `rs485` reports `mode` (`stopped|auto|manual`), `slaveId`, `regAddress`, `regCount`, `pass`, `lastValue`, `successStreak`, `failureStreak`, `pollIntervalMs`, `lastTx`/`lastRx` hex, `durationMs` (null while running), and `lastError` (`rs485_timeout`, `rs485_crc_mismatch`, `rs485_slave_id_mismatch`, `rs485_modbus_exception`, `rs485_frame_too_short`, `rs485_invalid_params`). Actions: `rs485-start`, `rs485-manual`, `rs485-stop`.
- Watchdog boundary: The engine is polled from `loop()` like the other QC services, so MCP1B4 heartbeat keeps its 1 s cadence; a stalled exchange is bounded by the 200 ms timeout and cannot wedge the loop.
- Unknown: The auto-direction assumption comes from the workbook wiring (no DE/RE pin) plus Longhi bench behavior, not from a TMM schematic. If the TMM module needs an external direction control, the bench will show one-way traffic (`rs485_timeout` with correct TX hex); that outcome reopens this decision and requires schematic evidence, not another guess.

## D-021 — Interactive clickable tutorial for QC operators

- Status: Accepted for bring-up QC (v0.3.1)
- Operator request: The tutorial was static text, so operators were unsure which card to work on and what was currently running.
- Decision: The tutorial steps and progress tiles are clickable and scroll to their QC card with a brief highlight animation; each tutorial step carries a live state badge (`SEKARANG` for the next undecided item, `SELESAI`/`BYPASS` once decided, `MENUNGGU` otherwise), the "next step" line in the progress card is itself clickable, and a running-test indicator under the next-step line shows exactly which test is active with its live stage (e.g. `Sedang berjalan: RS485 polling 2/3 valid`). No firmware/API contract changes — this is portal UX only, hence the patch-level version bump.
- Evidence limitation: Click/navigation behavior is compile-plus-browser evidence; the physical QC evidence still comes only from the operator completing the workflow on hardware.

