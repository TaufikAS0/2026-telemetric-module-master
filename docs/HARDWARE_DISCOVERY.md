# Hardware Discovery Checklist

Production firmware is blocked until the required fields below are supported by
schematics, PCB inspection, datasheets, or measured evidence.

| Area | Required evidence | Current state |
|---|---|---|
| MCU | Exact part number and module variant | Partial: workbook identifies ESP32-S3 and ATtiny404; exact ESP32-S3 part/module is unknown |
| PCB | Product revision and schematic revision | Partial: workbook tab identifies TMM V6 R0 M0; schematic revision/file is unknown |
| Power | Input range, rails, protection, current budget | Unknown |
| Flash | Size and mode | Partial: workbook warns GPIO35-GPIO37 are used by 8 MB RAM; RAM type, flash size/mode and memory parts remain unknown |
| Ethernet | PHY/controller, interface, pins, clocking | Partial: operator confirms W5500; SPI GPIO11/12/13, CS GPIO10, INT GPIO48 and reset MCP1B3 are mapped; physical link/DHCP remain untested |
| Wi-Fi | Required or prohibited, antenna constraints | Partial: bring-up uses concurrent open QC AP and provisioned station mode; production antenna, authentication and RF constraints remain unknown |
| Field bus | RS485/CAN/other, transceiver, termination | Partial: RS485 ESP RX GPIO17/TX GPIO18; transceiver, direction control, termination and protocol unknown |
| I/O | Pin map, voltage levels, safe boot states | Partial: M0 workbook pin map captured in `hardware/profiles/TMM_V6_R0_M0.json`; safe states and ATtiny voltage conflict unresolved |
| Storage | NVS/filesystem requirements and endurance | Partial: bring-up Wi-Fi settings use Preferences/NVS; encryption, endurance and production layout remain unknown |
| Display | Controller, address, geometry, orientation | Partial: workbook identifies SSD1306; controller geometry is 128x64, address is detected at runtime from 0x3C/0x3D, orientation remains unverified |
| Security | Device identity and provisioning method | Partial: OTA has a separately provisioned password; the captive QC AP/HTTP page is explicitly unauthenticated bench-only behavior, while production identity and provisioning remain unknown |
| Recovery | Bootloader and physical recovery procedure | Partial: build uses dual OTA app slots and password-protected ArduinoOTA; USB recovery, signed images and rollback behavior remain unknown |
| QC fixture | Connections and measurable pass limits | Unknown |

## Evidence rules

- A product image is not sufficient pin-map evidence.
- Source code from another product is not proof that TMM uses the same hardware.
- A successful compile proves toolchain consistency, not board compatibility.
- A successful simulator test proves software behavior only at the simulated boundary.

## M0 evidence received 2026-08-26

Source: `Modul Master.xlsx`, tab `TMM_V6_R0_M0`, gid `638455439`.

Confirmed by the workbook:

- ESP32-S3 master and ATtiny404 supervisor;
- I2C GPIO8/GPIO9 and stated addresses 0x20, 0x24, 0x28, 0x38, 0x57, 0x68 and 0x77;
- shared SPI GPIO11/GPIO12/GPIO13, SD CS GPIO47, Ethernet CS GPIO10 and INT GPIO48;
- LoRa ESP RX GPIO5/TX GPIO4 and RS485 ESP RX GPIO17/TX GPIO18;
- direct selector/button/status pins and MCP1/MCP2 logical signal names.

Conflict requiring schematic or measurement: the ATtiny404 connection table says
3.3 V, while its PA1/PA2 notes describe 1.8 V I2C behind a 1.8 V-to-3.3 V
level shifter. Do not program or actively drive the supervisor until resolved.

