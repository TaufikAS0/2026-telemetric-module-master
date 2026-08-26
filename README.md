# Telemetric Module Master (TMM)

This repository is the product-specific home for the Telemetric Module Master.
Its current phase is **Phase 0: specification and desktop simulation**.

The repository deliberately does not select a microcontroller or define real
pin assignments yet. Those decisions require verified hardware information.

## Run the desktop simulator

Requirements: Node.js 20 or newer.

```powershell
npm test
npm start
```

Open `http://127.0.0.1:8090`.

The simulator provides:

- `GET /api/health`
- `GET /api/modules`
- `GET /api/events`
- `POST /api/poll`
- `POST /api/modules/:id/state`

It models a TMM master and virtual expansion modules. It does not emulate
electrical signaling or prove compatibility with physical hardware.

## Handoff to the office computer

1. Copy or clone this repository on the office computer.
2. Give the AI the prompt in `docs/PROMPT_OFFICE_AI.md`.
3. Require the AI to read `AGENTS.md` and the referenced documents first.
4. Run `npm test` before and after every change.
5. Do not begin production firmware until the hardware discovery checklist is complete.

## Planned repository relationship

- This repo: TMM product behavior and product releases.
- Future `telemetric-embedded-core`: shared protocol, identity, logging, OTA, and diagnostics libraries.
- Existing `telemetric-hardware-portal`: firmware inventory, flashing, QC, reports, and audit history.

