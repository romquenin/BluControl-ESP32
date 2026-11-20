/*
 * Minimal Joybus HOST for an original GameCube pad, read through its cable:
 * the ESP32 plays the console on the data wire (1k pull-up to 3V3).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Buttons (1 = pressed) */
#define GC_BTN_A      (1u << 0)
#define GC_BTN_B      (1u << 1)
#define GC_BTN_X      (1u << 2)
#define GC_BTN_Y      (1u << 3)
#define GC_BTN_START  (1u << 4)
#define GC_BTN_LEFT   (1u << 8)
#define GC_BTN_RIGHT  (1u << 9)
#define GC_BTN_DOWN   (1u << 10)
#define GC_BTN_UP     (1u << 11)
#define GC_BTN_Z      (1u << 12)
#define GC_BTN_R      (1u << 13)   /* trigger fully pressed (click) */
#define GC_BTN_L      (1u << 14)

typedef struct
{
    uint16_t buttons;
    uint8_t sx, sy;     /* main stick, 128 = centre, up = high */
    uint8_t cx, cy;     /* C-stick */
    uint8_t l, r;       /* analog triggers */
} gc_pad_state_t;

void gc_joybus_init(int gpio);
bool gc_joybus_detect(void);
bool gc_joybus_read(gc_pad_state_t *st);
void gc_joybus_set_rumble(bool on);
uint32_t gc_joybus_errors(void);

/* Power-on with Z + Start held 2 s: restart into the OTA (update) app */
void gc_boot_ota_check(void);
/* Call after every read: Z + Start held 5 s switches BlueRetro <-> Switch mode */
void gc_mode_switch_check(uint16_t buttons);
/* Call while no pad answers: 20 s after boot, opens the Wi-Fi update mode */
void gc_no_pad_fallback(void);

#ifdef __cplusplus
}
#endif
