/*
 * Minimal Joybus HOST for an original GameCube pad, read through its cable.
 *
 * Bit = 4 us: '0' = 3 us low + 1 us high, '1' = 1 us low + 3 us high, then a
 * stop bit (1 us low). The pad answers on the same open-drain line; each bit
 * is sampled 2 us after its falling edge. Timings use the CPU cycle counter.
 *
 * Commands: 0x00 identify (3 bytes), 0x41 origin (10 bytes),
 * 0x40 0x03 0x0R poll (8 bytes, R = rumble on).
 */
#include "sdkconfig.h"
#ifdef CONFIG_BLUCONTROL_GC_CABLE

#include <string.h>
#include "gc_joybus.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_ipc_isr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LOG_TAG "GC_JOYBUS"
#define CC() esp_cpu_get_cycle_count()

static uint32_t s_pin;
static uint32_t s_mhz;
static bool s_ever_found;
static int64_t s_boot_us;
static uint32_t s_errors;
static volatile bool s_rumble;
static DRAM_ATTR uint8_t s_rx[16];

/* Sends tx[] + stop bit, then reads rxlen bytes. Returns complete bytes read. */
static IRAM_ATTR int jb_xfer(const uint8_t *tx, int txlen, int rxlen, uint32_t resp_us)
{
    const uint32_t P = s_pin;
    const uint32_t u1 = s_mhz, u2 = 2 * s_mhz, u3 = 3 * s_mhz, u4 = 4 * s_mhz;
    const uint32_t resp_cyc = resp_us * s_mhz;
    uint32_t t, t0;
    int n = 0;
    uint8_t cur = 0;

    portDISABLE_INTERRUPTS();
    esp_ipc_isr_stall_other_cpu();

    GPIO.out_w1tc = P;              /* when enabled, the pin pulls low */
    t = CC();
    for (int i = 0; i < txlen; i++)
    {
        for (int b = 7; b >= 0; b--)
        {
            uint32_t lo = ((tx[i] >> b) & 1) ? u1 : u3;
            GPIO.enable_w1ts = P;
            t += lo;
            while ((int32_t)(CC() - t) < 0) {}
            GPIO.enable_w1tc = P;
            t += u4 - lo;
            while ((int32_t)(CC() - t) < 0) {}
        }
    }
    /* stop bit */
    GPIO.enable_w1ts = P;
    t += u1;
    while ((int32_t)(CC() - t) < 0) {}
    GPIO.enable_w1tc = P;

    /* let the line rise */
    t0 = CC();
    while (!(GPIO.in & P))
    {
        if (CC() - t0 > u4) goto done;
    }
    for (int nbits = 0; nbits < rxlen * 8; nbits++)
    {
        t0 = CC();
        uint32_t limit = (nbits == 0) ? resp_cyc : u4 * 3;
        while (GPIO.in & P)
        {
            if (CC() - t0 > limit) goto done;
        }
        t0 = CC();                  /* falling edge of the bit */
        while ((int32_t)(CC() - t0) < (int32_t)u2) {}
        cur = (uint8_t)((cur << 1) | ((GPIO.in & P) ? 1 : 0));
        if ((nbits & 7) == 7)
        {
            s_rx[n++] = cur;
            cur = 0;
        }
        t0 = CC();
        while (!(GPIO.in & P))
        {
            if (CC() - t0 > u4) goto done;
        }
    }

done:
    esp_ipc_isr_release_other_cpu();
    portENABLE_INTERRUPTS();
    return n;
}

void gc_joybus_init(int gpio)
{
    s_mhz = esp_rom_get_cpu_ticks_per_us();
    s_pin = 1u << gpio;
    s_boot_us = esp_timer_get_time();
    gpio_reset_pin((gpio_num_t)gpio);
    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)gpio, GPIO_PULLUP_ONLY);   /* + external 1k */
    GPIO.out_w1tc = s_pin;
    ESP_LOGI(LOG_TAG, "Reset reason %d", (int)esp_reset_reason());
    ESP_LOGI(LOG_TAG, "Joybus host on GPIO%d, CPU %lu MHz", gpio, (unsigned long)s_mhz);
}

bool gc_joybus_detect(void)
{
    const uint8_t id = 0x00;
    int n = jb_xfer(&id, 1, 3, 500);
    if (n < 3 || s_rx[0] == 0x00 || s_rx[0] == 0xFF)
    {
        ESP_LOGW(LOG_TAG, "No pad answer to identify (%d bytes %02x %02x %02x), line %s",
                 n, s_rx[0], s_rx[1], s_rx[2], (GPIO.in & s_pin) ? "high" : "LOW");
        memset(s_rx, 0, 3);
        return false;
    }
    ESP_LOGI(LOG_TAG, "Pad found: id %02x %02x %02x", s_rx[0], s_rx[1], s_rx[2]);
    vTaskDelay(pdMS_TO_TICKS(2));
    const uint8_t origin = 0x41;
    n = jb_xfer(&origin, 1, 10, 500);
    ESP_LOGI(LOG_TAG, "Origin: %d bytes, stick %u/%u", n, s_rx[2], s_rx[3]);
    vTaskDelay(pdMS_TO_TICKS(2));
    s_ever_found = true;
    return true;
}

bool gc_joybus_read(gc_pad_state_t *st)
{
    const uint8_t req[3] = { 0x40, 0x03, (uint8_t)(s_rumble ? 0x01 : 0x00) };
    int n = jb_xfer(req, 3, 8, 500);
    /* byte 1 bit 7 is always 1, byte 0 bits 6-7 always 0 */
    if (n < 8 || !(s_rx[1] & 0x80) || (s_rx[0] & 0xC0))
    {
        s_errors++;
        return false;
    }
    st->buttons = (uint16_t)((s_rx[0] & 0x1F) | ((s_rx[1] & 0x7F) << 8));
    st->sx = s_rx[2];
    st->sy = s_rx[3];
    st->cx = s_rx[4];
    st->cy = s_rx[5];
    st->l = s_rx[6];
    st->r = s_rx[7];
    static uint16_t last = 0x5A5A;
    if (st->buttons != last)
    {
        last = st->buttons;
        ESP_LOGI(LOG_TAG, "Buttons 0x%04x  stick %u/%u  C %u/%u  L %u R %u", st->buttons,
                 st->sx, st->sy, st->cx, st->cy, st->l, st->r);
    }
    return true;
}

void gc_joybus_set_rumble(bool on)
{
    s_rumble = on;
}

/* Rumble requests from BlueRetro or the Switch go to the pad's own motor */
void blu_rumble_ext(uint8_t left, uint8_t right)
{
    s_rumble = (left > 0 || right > 0);
}

uint32_t gc_joybus_errors(void)
{
    return s_errors;
}

/* ---------------- boot / mode helpers (same behaviour as the Dreamcast pad) ---------------- */

#define COMBO (GC_BTN_START | GC_BTN_Z)

static bool combo_held(uint16_t b)
{
    if ((b & (GC_BTN_UP | GC_BTN_DOWN)) == (GC_BTN_UP | GC_BTN_DOWN)) return false;
    if ((b & (GC_BTN_LEFT | GC_BTN_RIGHT)) == (GC_BTN_LEFT | GC_BTN_RIGHT)) return false;
    return (b & COMBO) == COMBO;
}

static void boot_partition(esp_partition_subtype_t subtype)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, NULL);
    if (p != NULL && esp_ota_set_boot_partition(p) == ESP_OK)
    {
        esp_restart();
    }
    ESP_LOGE(LOG_TAG, "Could not switch to partition subtype %d", subtype);
}

void gc_boot_ota_check(void)
{
    if (esp_reset_reason() != ESP_RST_POWERON)
    {
        return;
    }
    bool found = false;
    for (int i = 0; i < 10 && !found; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(60));
        found = gc_joybus_detect();
    }
    if (!found)
    {
        return;
    }
    gc_pad_state_t st;
    int64_t start = esp_timer_get_time();
    int good = 0;
    while (esp_timer_get_time() - start < 2000000)
    {
        if (gc_joybus_read(&st))
        {
            if (!combo_held(st.buttons))
            {
                return;
            }
            good++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (good >= 20)
    {
        ESP_LOGI(LOG_TAG, "Z + Start held at power-on: OTA mode");
        boot_partition(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    }
}

void gc_mode_switch_check(uint16_t buttons)
{
    static bool armed = false;
    static int64_t since = 0;
    if (!combo_held(buttons))
    {
        armed = true;
        since = 0;
        return;
    }
    if (!armed)
    {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (since == 0)
    {
        since = now;
    }
    else if (now - since >= 5000000)
    {
        const esp_partition_t *run = esp_ota_get_running_partition();
        esp_partition_subtype_t next = (run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
                                       ? ESP_PARTITION_SUBTYPE_APP_OTA_2
                                       : ESP_PARTITION_SUBTYPE_APP_OTA_1;
        ESP_LOGI(LOG_TAG, "Z + Start held 5 s: switching mode");
        boot_partition(next);
    }
}

void gc_no_pad_fallback(void)
{
    if (s_ever_found)
    {
        return;
    }
    if (esp_timer_get_time() - s_boot_us > 20000000)
    {
        ESP_LOGW(LOG_TAG, "No pad for 20 s: OTA mode");
        boot_partition(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    }
}

#endif /* CONFIG_BLUCONTROL_GC_CABLE */
