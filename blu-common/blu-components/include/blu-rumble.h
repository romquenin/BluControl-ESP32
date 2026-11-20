#ifndef BLU_RUMBLE_H
#define BLU_RUMBLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Rumble motor driver (PWM through LEDC).
 *
 * The motor itself must NOT be driven straight from the ESP32 pin: the GPIO
 * drives the gate of a logic-level N-MOSFET (e.g. AO3400A) that switches the
 * motor from the battery, with a flyback diode across the motor.
 *
 * When only one motor is configured (GameCube, N64 pak...), left and right
 * requests are merged (the strongest one wins).
 */
void blu_rumble_init(void);

/* 0 = off, 255 = full power */
void blu_rumble_set(uint8_t left, uint8_t right);

/* Immediately stop every motor */
void blu_rumble_stop(void);

bool blu_rumble_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
