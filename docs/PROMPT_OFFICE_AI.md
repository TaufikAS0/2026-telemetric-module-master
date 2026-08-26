# Prompt for the AI on the Office Computer

Copy the prompt below into the AI working on the office computer.

---

You are working on the Telemetric Module Master repository.

Repository objective: develop TMM through evidence-based phases, beginning with
a hardware-neutral desktop simulator and only later moving to production
embedded firmware after real hardware contracts are confirmed.

Before doing anything:

1. Read `AGENTS.md` completely.
2. Read every document referenced by `AGENTS.md` in the specified order.
3. Inspect the repository and run `npm test` as a baseline.
4. Report the current facts, reasonable inferences, assumptions, and unknowns.

For this session, do not choose an MCU, pin map, Ethernet PHY, RS485 wiring, or
production partition table unless the repository contains direct evidence for
that choice. Do not claim that desktop simulation proves hardware behavior.

Your first task is to review and improve the Phase 0 desktop simulator without
changing its hardware-neutral boundary. Verify health reporting, virtual-module
inventory, polling, offline/online transitions, event retention, invalid input,
and restart behavior. Add tests for every behavioral change.

Work on a feature branch. Keep changes small and reviewable. Do not push,
publish a release, add secrets, or create production firmware unless the human
operator explicitly authorizes it.

At the end, provide:

- what changed and why;
- exact test commands and results;
- unresolved risks and unknowns;
- recommended next task;
- branch name and commit hash if a commit was created.

---

## Prompt for a later hardware phase

Do not use this second prompt until `docs/HARDWARE_DISCOVERY.md` is complete.

---

Read `AGENTS.md` and the full TMM documentation set. Verify that every required
hardware field has evidence and that unresolved fields are explicitly blocked.
Create a proposed embedded architecture and compatibility matrix, but do not
write production firmware until the human operator approves the hardware
profile. Map every proposed driver and interface to a schematic, datasheet, or
measured hardware fact. Add a traceability table from PRD requirement to test.

---

