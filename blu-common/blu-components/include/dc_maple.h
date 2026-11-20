/*
 * Minimal Maple bus HOST for an original Dreamcast pad.
 * The ESP32 plays the console on the pad's 5-pin cable header
 * (pin 1 = SDCKA, pin 5 = SDCKB, ESP32 internal pull-ups).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Buttons of the condition word (bit set = pressed, already inverted) */
#define DC_BTN_C      (1u << 0)
#define DC_BTN_B      (1u << 1)
#define DC_BTN_A      (1u << 2)
#define DC_BTN_START  (1u << 3)
#define DC_BTN_UP     (1u << 4)
#define DC_BTN_DOWN   (1u << 5)
#define DC_BTN_LEFT   (1u << 6)
#define DC_BTN_RIGHT  (1u << 7)
#define DC_BTN_Z      (1u << 8)
#define DC_BTN_Y      (1u << 9)
#define DC_BTN_X      (1u << 10)
#define DC_BTN_D      (1u << 11)

typedef struct
{
    uint16_t buttons;   /* DC_BTN_* , 1 = pressed */
    uint8_t  rtrig;     /* 0 = released .. 255 = fully pressed */
    uint8_t  ltrig;
    uint8_t  joy_x;     /* 0 = left, 128 = centre, 255 = right */
    uint8_t  joy_y;     /* 0 = up,   128 = centre, 255 = down  */
} dc_pad_state_t;

void dc_maple_init(int gpio_a, int gpio_b);

/* Asks the pad who it is (also wakes it up). true if a standard controller answered. */
bool dc_maple_detect(void);

/* Reads the pad. true on a valid answer (CRC OK). */
bool dc_maple_read(dc_pad_state_t *state);

/* Reads one 512-byte block of the VMU in slot 1 (wire byte order) */
bool dc_maple_vmu_read_block(uint8_t block, uint8_t *out);

/* Read-only test: lists the saves of the VMU in slot 1 in the log, and times a full read.
 * Returns the number of saves (< 0 on error). */
int dc_maple_vmu_read_test(int *blocks_ok, int *ms);

/* Draws a test pattern (horizontal bands) on the screen of the VMU in slot 1 */
bool dc_maple_vmu_lcd_test(void);

/* VMU copy made at power-on, before Bluetooth starts (stored in flash) */
#define DC_VMU_OK          0
#define DC_VMU_NONE        1    /* no VMU in slot 1 (or no pad) */
#define DC_VMU_ERROR       2    /* VMU present but not readable */
#define DC_VMU_FLASH_ADDR  0x3E0000
#define DC_VMU_SIZE        (128 * 1024)
int dc_vmu_snapshot(void);
int dc_vmu_state(void);
/* one 512-byte block of the copy, bus (wire) byte order */
bool dc_vmu_copy_read(uint8_t block, uint8_t *out);
/* step 3: write-back of the console's saves, and the VMU screen */
bool dc_maple_vmu_write_block(uint8_t block, const uint8_t *data);
bool dc_maple_vmu_lcd_write(const uint8_t *data);
bool dc_vmu_copy_update(uint8_t block, const uint8_t *data);

/* Power-on with Start + A held (2 s): restart into the OTA (update) app */
void dc_boot_ota_check(void);

/* Call after every read: Start + A held 5 s switches BlueRetro <-> Switch mode */
void dc_mode_switch_check(uint16_t buttons);

/* true once the pad has answered at least once since boot */
bool dc_maple_ever_found(void);

/* Call while no pad answers: after 20 s from power-on, opens the Wi-Fi update mode */
void dc_no_pad_fallback(void);

/* Statistics for the log */
uint32_t dc_maple_errors(void);

#ifdef __cplusplus
}
#endif
