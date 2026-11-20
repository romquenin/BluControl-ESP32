# Submodule patches

`build.sh` applies these small changes to JPZV's submodules, so the submodules can stay on their original repositories.

| Patch | Submodule | Change |
|---|---|---|
| `ESP32-BLE-Gamepad-idf.patch` | ESP32-BLE-Gamepad-idf | optional new Bluetooth address at each boot; fixes a memory leak and an over-read in the rumble handler |
| `esp-nimble-cpp.patch` | esp-nimble-cpp | directed advertising, to reconnect quickly to the last host |
| `HOJA-LIB-ESP32.patch` | HOJA-LIB-ESP32 | Switch HD rumble: reads the amplitude correctly, including rumble carried by sub-command packets |
