# BlueRetro patch: Dreamcast VMU over Bluetooth

A patch for [BlueRetro](https://github.com/darthcloud/BlueRetro) **v25.04** (by darthcloud), for the **HW2 Dreamcast** build. It lets a BluControl Dreamcast controller share the VMU plugged into it, and adds 4 VMU pages. Users are described in [../docs/dreamcast.md](../docs/dreamcast.md).

## What it changes

| File | Change |
|---|---|
| `main/bluetooth/att_hid.c` | finds the BluControl VMU service (UUID `7d1e0001-…`), receives the VMU copy, sends written blocks and the VMU screen back |
| `main/adapter/memory_card.c` | the VMU state machine (copy, mirror, pad gone), 4 pages in `/fs/mc.bin` and `/fs/mc2.bin`…`mc4.bin`, file work in a low-priority task |
| `main/wired/maple.c` | hides the VMU while it is being replaced; announces screen and clock while the controller's VMU is used; forwards screen writes |
| `main/adapter/wired/dc.c` | Start + Y / Start + X held 4 s to change page |
| `main/adapter/page_lcd.h` | "PAGE n" images for the VMU screen (made by `page_lcd_gen.py`) |
| `main/bluetooth/att_cfg.c` | diagnostic line after the version on blueretro.io |
| `main/system/crashlog.c` | crash recorder kept in RTC memory |
| `main/system/manager.c` | ignores the power-sense input (IO39) on the Dreamcast build |
| `main/bluetooth/host.c`, `main/main.c`, `main/CMakeLists.txt` | hooks for the above |

With any other controller, BlueRetro behaves like the official v25.04.

## Building

```sh
git clone https://github.com/darthcloud/BlueRetro
cd BlueRetro
git checkout v25.04
git submodule update --init --recursive
git apply /path/to/BlueRetro_v25.04_dreamcast_vmu.patch
echo "v25.04-dc-v1" > version.txt
BR_HW=_hw2 BR_SYS=_dreamcast idf.py build
```

The release binary was built with ESP-IDF v5.5-dev.

**Important:** the Maple bus runs on the ESP32's second core, which cannot run code from flash. Everything reached from `maple_rx()`, such as `mc_write()` and `mc_read()`, must stay in IRAM. Check the disassembly after any change there.

## License

BlueRetro is Apache-2.0. This patch is under the same license.
