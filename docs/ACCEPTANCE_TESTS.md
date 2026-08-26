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

