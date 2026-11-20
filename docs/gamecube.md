# Wireless GameCube controller

Version 1 is released. Version 2 is in development.

Two ways to make a GameCube controller wireless with BluControl:

| | Version 1: Yakara style, with rumble ✅ | Version 2: cable mode 🚧 |
|---|---|---|
| Controller board | components removed, as in Yakara's tutorial | **kept intact** |
| How the ESP32 reads the controller | buttons, sticks and triggers wired directly to the ESP32 | plays the console's role on the controller's data line (Joybus) |
| Rumble | driven by the ESP32 through the **BluPower GC** board (MOSFET) | driven by the controller itself, through the protocol |
| BluPower GC board | **fully populated** (regulator + rumble driver) | regulator only (U1, C1, C2, C3), plus a 5 V boost |

## Version 1: Yakara style, with rumble

**If you followed [Yakara's GameCube tutorial](https://github.com/yakaracolombia/esp32-online-tool/blob/main/gamecube.md) and want rumble back, this is the method to use.** It keeps the same idea and the same wiring as the tutorial, and adds:
- **rumble**, through the [BluPower GC board](blupower.md), which drives the controller's motor from the battery. The board also replaces the ESP32 board's AMS1117 with a low-dropout regulator, which fixes Bluetooth disconnections on battery;
- a **blue LED** for BlueRetro mode;
- **pairing memory**: the controller remembers its host and reconnects to it;
- several fixes in the Switch and BLE libraries: a memory leak in the rumble handler, correct reading of Switch HD rumble, and the motor stopping if Bluetooth is lost.

### Downloads

From the [Releases](https://github.com/romquenin/BluControl-ESP32/releases) (latest release):

| File | Where |
|---|---|
| `BluControl_GameCube_v1.updpkg` | Wi-Fi update |
| `BluControl_GameCube_v1_FULL_0x0.bin` | first install or recovery over USB, at address **0x0** (Flash Download Tool: ESP32, 40 MHz, DIO) |

The partition table is the same as Yakara's firmware, so a controller already running it can be updated over Wi-Fi.

### Wiring

Follow Yakara's tutorial for the controller side (buttons, sticks, triggers). Then add the BluPower GC board as shown on [its page](blupower.md#wiring-gamecube-version-1): desolder the AMS1117 from the ESP32 board, battery → TP4056 → J1, J2 → 3V3 / IO23 / GND of the ESP32, J3 → rumble motor.

Pin assignment used by this firmware:

| Function | GPIO |
|---|---|
| A / B / X / Y | 14 / 27 / 2 / 15 |
| D-pad up / down / left / right | 5 / 19 / 17 / 18 |
| L / R (digital) / Z | 16 / 13 / 12 |
| Start | 21 |
| L / R analog triggers | 34 / 26 |
| Main stick X / Y | 35 / 32 |
| C-stick X / Y | 33 / 25 |
| **Rumble** (gate of the BluPower GC MOSFET) | **23** |
| Red LED (Switch mode) | 4 |
| Blue LED (BlueRetro mode) | 22 |

Do not use IO0 for the LED: the ESP32 reads it at boot, and pulling it low keeps the ESP32 in programming mode.

### Using it

| Action | How |
|---|---|
| Change mode | hold **Z + Start** for 5 s (red LED = Switch, blue LED = BlueRetro) |
| Wi-Fi update | hold **Z + Start** while switching on, about 6 s, then join the "BluControl OTA" network and open http://192.168.4.1 |
| Erase pairings | hold **X + Y** while switching on, about 1 s. The LEDs blink 6 times |

After a Wi-Fi update, the controller restarts in Switch mode. Hold Z + Start for 5 s to go back to BlueRetro mode.

## Version 2: cable mode (in development)

The controller board stays intact. The ESP32 connects to the wires that normally go to the console and talks to the controller over Joybus, like a GameCube does. Rumble is requested through the protocol, and the controller drives its own motor from the 5 V line. The BluPower GC board then only needs its regulator (U1, C1, C2, C3), plus a 5 V boost, as for the [Dreamcast controller](dreamcast.md).

## Credits

[Yakara](https://github.com/yakaracolombia), for the original [GameCube wireless tutorial](https://github.com/yakaracolombia/esp32-online-tool/blob/main/gamecube.md) with BluControl. Version 1 builds on it.
