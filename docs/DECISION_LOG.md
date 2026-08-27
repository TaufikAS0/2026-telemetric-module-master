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

