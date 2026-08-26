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
- the sketch does not initialize unverified Ethernet, MCP, LoRa, or RS485 drivers.

Manual board checks, only after module/flash/power/recovery settings are verified:

1. Flash the bring-up sketch and capture the complete serial boot log.
2. Run `check-i2c`; compare responding addresses with the profile.
3. Run `gpio` while operating each selector/button and record transitions.
4. Insert a non-production SD card and run `probe-sd`.
5. Do not continue to active peripheral drivers when the `blocked` list is non-empty.

