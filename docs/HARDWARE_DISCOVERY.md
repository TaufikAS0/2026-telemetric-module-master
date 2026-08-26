# Hardware Discovery Checklist

Production firmware is blocked until the required fields below are supported by
schematics, PCB inspection, datasheets, or measured evidence.

| Area | Required evidence | Current state |
|---|---|---|
| MCU | Exact part number and module variant | Unknown |
| PCB | Product revision and schematic revision | Unknown |
| Power | Input range, rails, protection, current budget | Unknown |
| Flash | Size and mode | Unknown |
| Ethernet | PHY/controller, interface, pins, clocking | Unknown |
| Wi-Fi | Required or prohibited, antenna constraints | Unknown |
| Field bus | RS485/CAN/other, transceiver, termination | Unknown |
| I/O | Pin map, voltage levels, safe boot states | Unknown |
| Storage | NVS/filesystem requirements and endurance | Unknown |
| Security | Device identity and provisioning method | Unknown |
| Recovery | Bootloader and physical recovery procedure | Unknown |
| QC fixture | Connections and measurable pass limits | Unknown |

## Evidence rules

- A product image is not sufficient pin-map evidence.
- Source code from another product is not proof that TMM uses the same hardware.
- A successful compile proves toolchain consistency, not board compatibility.
- A successful simulator test proves software behavior only at the simulated boundary.

