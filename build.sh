#!/bin/bash
# Build BluControl for one controller config, e.g.:  ./build.sh dreamcast
# Needs ESP-IDF v5.0 (export.sh sourced, or IDF_PATH set) and Node.js (OTA web page).
# Output: release/BluControl_<config>_FULL_0x0.bin (USB flash at 0x0)
#         release/BluControl_<config>.updpkg       (Wi-Fi update)
set -e
CFG=${1:-dreamcast}
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

if ! command -v idf.py >/dev/null; then
    . "${IDF_PATH:?set IDF_PATH or source ESP-IDF export.sh}/export.sh"
fi

# submodules from JPZV, plus our small patches (see patches/README.md)
git submodule update --init --recursive
apply_patch() {
    if git -C "$1" apply --reverse --check "$ROOT/patches/$2" 2>/dev/null; then
        echo "patch $2 already applied"
    else
        git -C "$1" apply "$ROOT/patches/$2"
    fi
}
apply_patch blu-generic/components/ESP32-BLE-Gamepad-idf ESP32-BLE-Gamepad-idf.patch
apply_patch blu-generic/components/esp-nimble-cpp esp-nimble-cpp.patch
apply_patch blu-switch/components/HOJA-LIB-ESP32 HOJA-LIB-ESP32.patch

# OTA web page
(cd blu-ota/html && npm install --silent && npm run build --silent)

OUT="$ROOT/release"
TMP="$ROOT/release/tmp_$CFG"
rm -rf "$TMP"; mkdir -p "$TMP"
for app in ota switch generic; do
    cd "$ROOT/blu-$app"
    B=build_$CFG
    rm -rf "$B"; mkdir -p "$B"
    idf.py -B "$B" -D SDKCONFIG="$PWD/$B/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="$PWD/base.sdkconfig;$ROOT/configs/$CFG.sdkconfig" build
    cp "$B/blucontrol-$app.bin" "$TMP/"
    if [ "$app" = ota ]; then
        cp "$B/www.bin" "$B/ota_data_initial.bin" "$B/bootloader/bootloader.bin" \
           "$B/partition_table/partition-table.bin" "$TMP/"
    fi
done

cd "$TMP"
esptool.py --chip esp32 merge_bin -o "$OUT/BluControl_${CFG}_FULL_0x0.bin" \
    0x1000 bootloader.bin 0x8000 partition-table.bin 0xd000 ota_data_initial.bin \
    0x10000 blucontrol-ota.bin 0x110000 blucontrol-switch.bin \
    0x210000 blucontrol-generic.bin 0x310000 www.bin
printf "\x01\x00%07d" "$(stat --printf="%s" blucontrol-switch.bin)" | cat - blucontrol-switch.bin > sw.upd
printf "\x01\x01%07d" "$(stat --printf="%s" blucontrol-generic.bin)" | cat - blucontrol-generic.bin > gen.upd
cat sw.upd gen.upd > "$OUT/BluControl_${CFG}.updpkg"
cd "$ROOT"; rm -rf "$TMP"
ls -l "$OUT"
