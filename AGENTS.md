# TMM Repository Rules for AI Agents

Read these files before changing source code, in this order:

1. `docs/AI_HANDOFF.md`
2. `docs/PRD.md`
3. `docs/HARDWARE_DISCOVERY.md`
4. `docs/DECISION_LOG.md`
5. `docs/ACCEPTANCE_TESTS.md`

## Non-negotiable rules

- Do not invent an MCU, PCB revision, pin assignment, Ethernet PHY, electrical limit, or field-bus contract.
- Keep desktop simulation separate from embedded production firmware.
- A passing simulator is not evidence that physical hardware works.
- Every material design decision must be added to `docs/DECISION_LOG.md` with its evidence and consequences.
- Label unverified claims as an assumption or unknown.
- Never commit BIN files, credentials, Wi-Fi passwords, private keys, device secrets, or production data.
- Every TMM firmware build must emit BOTH artifacts together as one bound release
  (`npm run build:bringup`): the app-only BIN for LAN OTA and the merged BIN for USB
  recovery, plus `artifacts.json` recording fileName, byte size, SHA-256, imageType,
  offset, and supported transport for each. The build fails if either BIN or its
  recorded metadata/checksum is unavailable, and the version must come from
  `tmm_v6_r0_m0_version.h` so the two BINs always share one identity.
- Each commit must leave `npm test` passing.
- Do not publish a GitHub release or mark firmware production-ready without hardware-test evidence.
- Preserve compatibility with the Telemetric Hardware Portal manifest contract.

## Working method

- Work on a feature branch.
- Make one bounded change at a time.
- Add or update tests with behavioral changes.
- Report facts, reasonable inferences, assumptions, and unknowns separately.
- Stop and request hardware evidence when the next decision depends on real electrical or protocol details.

