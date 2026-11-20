# BluPower GC board

> 🚧 **Draft:** design files (KiCad, Gerber, BOM) and photos will be added here.

A small board designed for this project. It powers the ESP32 from a single-cell LiPo battery and, in its full version, drives the GameCube controller's rumble motor.

| Part | Role |
|---|---|
| U1 (AP2112K-3.3) + C1, C2 | low-dropout 3.3 V regulator for the ESP32. It keeps working as the battery runs down, where an AMS1117 cannot |
| Rumble driver (MOSFET, gate on GPIO23) | switches the controller's rumble motor from the battery |

## Which parts to solder

| Build | Parts |
|---|---|
| GameCube, version 1 (Yakara style, with rumble) | everything |
| GameCube, version 2 (cable mode) | U1, C1, C2 only, plus an external 5 V boost |
| Dreamcast | U1, C1, C2 only, plus an external 5 V boost |

## Photos

*Coming: front and back of the bare board, as delivered by JLCPCB.*

## Files

*Coming: KiCad project, Gerber files, BOM and placement files for JLCPCB.*
