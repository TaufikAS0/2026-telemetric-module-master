# Product Requirements Document — TMM

Status: Draft, Phase 0  
Product: Telemetric Module Master  
Code: TMM

## Problem

Telemetric products need a master unit that can supervise subordinate modules,
present understandable status, retain diagnostic events, and provide a stable
integration point for commissioning and quality control.

## Phase 0 goal

Validate the software boundaries and operator workflow on a normal computer
before committing to production hardware details.

## Phase 0 functional requirements

- Report master identity, software version, uptime, and health.
- Maintain an inventory of subordinate virtual modules.
- Poll all modules and retain the resulting events.
- Represent online and offline module states.
- Reject malformed state-change requests.
- Keep a bounded event history to prevent unlimited memory growth.
- Provide a simple dashboard understandable by a first-time operator.
- Expose deterministic APIs that can be covered by automated tests.

## Future production requirements

These are intentions, not confirmed implementation contracts:

- Discover and supervise compatible physical expansion modules.
- Continue essential operation during temporary network loss.
- Provide local commissioning and diagnostics.
- Provide authenticated configuration changes.
- Support manufacturing self-test and QC evidence export.
- Produce traceable release artifacts compatible with the hardware portal.
- Recover safely from communication faults and unexpected resets.

## Explicit non-goals for Phase 0

- Electrical RS485, CAN, Ethernet, or GPIO emulation
- Selecting the MCU or PCB
- Flashing physical hardware
- Production authentication or cryptographic identity
- Declaring a production-ready firmware version

## Success criteria

- All automated tests pass on the development computer.
- The dashboard loads locally and displays three virtual modules.
- Polling records events and updates module timestamps.
- A module can be changed offline and online through the API.
- Invalid input returns a clear 4xx response.
- Documentation distinguishes evidence from assumptions.

