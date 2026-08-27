# Acceptance Tests

## Phase 0 automated tests

- Health API returns product identity, simulator mode, semantic version, and uptime.
- Module inventory contains unique module IDs.
- Polling updates last-seen values and records one event per module.
- Offline modules remain visible and report an offline polling result.
- State API accepts boolean `online` only.
- Unknown routes and modules return 404.
- Invalid JSON and invalid state values return 400.
- Event history remains bounded.

## Manual desktop test

1. Run `npm test` and require zero failures.
2. Run `npm start`.
3. Open `http://127.0.0.1:8090`.
4. Confirm three virtual modules are visible.
5. Select **Poll all modules** and verify timestamps and events change.
6. Set one module offline and verify its status changes without disappearing.
7. Restore the module online.
8. Stop the process and confirm port 8090 closes.

## Not yet testable

- Physical bus discovery
- Electrical signal integrity
- Power-failure recovery
- Flashing and bootloader compatibility
- Hardware watchdog behavior
- EMC, thermal, and production QC limits

## TMM V6 R0 M0 bring-up MVP

Host-side contract tests:

- hardware profile identifies `TMM_V6_R0_M0`, ESP32-S3, ATtiny404, and source gid;
- I2C, SPI, LoRa UART, RS485 UART, SD, and Ethernet pins match the workbook;
- unknown part numbers and protocols remain explicit null/blocked values;
- the sketch does not initialize unverified Ethernet, LoRa, or RS485 drivers, and limits provisional MCP access to MCP1B4.
- MCP1B4 heartbeat initialization occurs before Wi-Fi startup;
- heartbeat interval is 1000 ms, below the operator-stated five-second reset window;
- Wi-Fi connection/retry is nonblocking, credentials are provisioned through
  Serial/NVS, and no credential is compiled into the generic BIN.
- SSD1306 writes begin only after a runtime response at 0x3C or 0x3D, and the
  display refresh services the watchdog between bounded I2C chunks.
- ArduinoOTA starts only with Wi-Fi plus a provisioned password, accepts the
  app-only image, and services MCP1B4 around OTA handling and progress.
- the generated QC hotspot uses AP+STA mode, wildcard captive DNS, common OS
  probe routes, and services MCP1B4 around DNS/HTTP handling;
- QC web actions are restricted to I2C scan, OLED refresh, and Wi-Fi reconnect.

Manual board checks, only after module/flash/power/recovery settings are verified:

1. Flash the bring-up sketch and capture the complete serial boot log.
2. Run `check-i2c`; compare responding addresses with the profile.
3. Run `gpio` while operating each selector/button and record transitions.
4. Run `heartbeat` and require `ready: true`; scope MCP1B4 and verify a pulse every second.
5. Run `wifi-set <ssid>|<password>`, then `wifi`; verify connection without any
   heartbeat gap approaching five seconds and verify the status output does not
   disclose either credential.
6. Power-cycle the board, run `wifi`, and verify the NVS configuration reconnects.
7. Run `wifi-clear`, power-cycle again, and verify the board remains unprovisioned.
8. Do not continue to other active peripheral drivers when the `blocked` list is non-empty.
9. Run `oled`; verify the detected address, readable orientation, Wi-Fi status,
   and that the OLED IP equals the IP in the serial `wifi` log.
10. Run `ota-set <password>`, confirm `ota` reports ready, then upload the
    app-only `.ino.bin` from Arduino IDE over the reported IP.
11. During OTA, scope MCP1B4 and verify no heartbeat gap approaches five seconds;
    after reboot, verify the new sketch version and test USB recovery.
12. Join the generated `TMM-M0-xxxxxx` hotspot and verify the captive page opens;
    if it does not auto-open, browse to the AP IP shown on OLED/serial.
13. Verify status and the three bounded controls from both the hotspot and the
    station/LAN IP, while confirming no heartbeat gap approaches five seconds.

