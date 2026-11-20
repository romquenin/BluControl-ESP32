/*
 * Minimal Maple bus HOST for an original Dreamcast pad.
 *
 * Waveforms and receive loop follow BlueRetro's maple.c (Jacques Gagnon,
 * Apache-2.0), which drives the same bus from an ESP32 as a device.
 * Here the roles are swapped: the ESP32 sends the console's requests and
 * reads the pad's answers. Timings use the CPU cycle counter so they do not
 * depend on the CPU frequency (240 MHz recommended).
 *
 * Frame bytes are in bus order: [len words][src][dst][cmd][data...][crc]
 * (same layout as raphnet's dreamcast_usb and BlueRetro).
 */
#include <string.h>
#include <stdlib.h>
#include "dc_maple.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_ipc_isr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_flash.h"

#include "sdkconfig.h"
#ifdef CONFIG_BLUCONTROL_DREAMCAST

#define LOG_TAG "DC_MAPLE"

#define CMD_DEV_INFO       0x01
#define CMD_DEV_INFO_RSP   0x05
#define CMD_DATA_TX        0x08
#define CMD_GET_CONDITION  0x09

#define ADDR_HOST   0x00    /* console, port A */
#define ADDR_MAIN   0x20    /* main peripheral (the pad), port A */
#define FUNC_CTRL   0x00000001

static uint32_t s_a, s_b;                 /* GPIO masks */
static uint32_t c100, c200, c500;         /* cycles */
static uint32_t c_end, c_abort;           /* rx timeouts (cycles) */
static uint32_t s_errors;
static int s_gpio_a, s_gpio_b;
static uint32_t s_mhz;
static int s_variant = -1;            /* bus variant that answered (-1 = none yet) */
static volatile int s_last_stage;     /* 0 = no start seen, 1 = start seen */
static int64_t s_boot_us;

static DRAM_ATTR uint8_t s_rx[600];   /* a VMU block answer is 525 bytes */

#define CC() esp_cpu_get_cycle_count()
#define WAIT(cyc) do { t += (cyc); while ((int32_t)(CC() - t) < 0) {} } while (0)

/*
 * Sends tx[0..txlen-1] + CRC, then receives the answer into s_rx.
 * Returns the number of complete bytes received (0 = no answer).
 * Runs with interrupts off and the other core stalled (as BlueRetro does),
 * for at most resp_us + the length of the answer.
 */
static IRAM_ATTR int maple_xfer(const uint8_t *tx, int txlen, uint32_t resp_us)
{
    const uint32_t A = s_a, B = s_b;
    uint32_t t, t0, g;
    uint8_t crc = 0;
    int n = 0;
    uint8_t cur = 0;
    const uint32_t resp_cyc = resp_us * (c100 * 10);
    /* local copies: kept in registers inside the tight loops */
    const uint32_t k100 = c100, k200 = c200, k500 = c500;
    const uint32_t k_end = c_end, k_abort = c_abort;

    portDISABLE_INTERRUPTS();
    esp_ipc_isr_stall_other_cpu();

    /* ---------- transmit ---------- */
    GPIO.out_w1ts = A | B;
    GPIO.enable_w1ts = A | B;
    t = CC();
    WAIT(k500);
    /* start pattern: A low, 4 pulses on B */
    GPIO.out_w1tc = A; WAIT(k500);
    GPIO.out_w1tc = B; WAIT(k500);
    GPIO.out_w1ts = B; WAIT(k500);
    GPIO.out_w1tc = B; WAIT(k500);
    GPIO.out_w1ts = B; WAIT(k500);
    GPIO.out_w1tc = B; WAIT(k500);
    GPIO.out_w1ts = B; WAIT(k500);
    GPIO.out_w1tc = B; WAIT(k500);
    GPIO.out_w1ts = B; WAIT(k200);

    for (int i = 0; i <= txlen; i++)
    {
        uint8_t byte;
        if (i < txlen)
        {
            byte = tx[i];
            crc ^= byte;
        }
        else
        {
            byte = crc;
        }
        for (uint32_t mask = 0x80; mask; mask >>= 2)
        {
            /* A clocks, B carries the bit */
            GPIO.out_w1ts = A; WAIT(k200);
            if (byte & mask) GPIO.out_w1ts = B; else GPIO.out_w1tc = B;
            WAIT(k100);
            GPIO.out_w1tc = A; WAIT(k200);
            /* B clocks, A carries the bit */
            GPIO.out_w1ts = B; WAIT(k200);
            if (byte & (mask >> 1)) GPIO.out_w1ts = A; else GPIO.out_w1tc = A;
            WAIT(k100);
            GPIO.out_w1tc = B; WAIT(k200);
        }
    }
    /* end pattern */
    GPIO.out_w1ts = A; WAIT(k100);
    GPIO.out_w1ts = B; WAIT(k500);
    GPIO.out_w1tc = B; WAIT(k500);
    GPIO.out_w1tc = A; WAIT(k500);
    GPIO.out_w1ts = A; WAIT(k500);
    GPIO.out_w1tc = A; WAIT(k500);
    GPIO.out_w1ts = A; WAIT(k500);
    GPIO.out_w1ts = B; WAIT(k100);
    /* release the bus (external pull-ups keep it high) */
    GPIO.enable_w1tc = A | B;

    /* ---------- receive ---------- */
    /* wait for the start of the answer: A falls */
    s_last_stage = 0;
    t0 = CC();
    while (GPIO.in & A)
    {
        if (CC() - t0 > resp_cyc)
        {
            goto done;
        }
    }
    s_last_stage = 1;
    {
        /* The pad pauses inside its answer (seen after 16 bytes): the frame length
         * comes from the header, timeouts are only a safety net. */
        int expect = 4;               /* until the length byte is known */
        const uint32_t k_gap = s_mhz * 2000;   /* 2 ms */
        while (n < expect)
        {
            for (uint32_t mask = 0x80; mask; )
            {
                t0 = CC();
                while (!(GPIO.in & A)) { if (CC() - t0 > k_gap) goto done; }
                t0 = CC();
                while ((g = GPIO.in) & A) { if (CC() - t0 > k_gap) goto done; }
                if (g & B) cur |= mask;
                mask >>= 1;

                t0 = CC();
                while (!(GPIO.in & B)) { if (CC() - t0 > k_gap) goto done; }
                t0 = CC();
                while ((g = GPIO.in) & B) { if (CC() - t0 > k_gap) goto done; }
                if (g & A) cur |= mask;
                mask >>= 1;
            }
            if (n < (int)sizeof(s_rx))
            {
                s_rx[n] = cur;
            }
            if (n == 0)
            {
                expect = 4 + cur * 4 + 1;   /* header + words + CRC */
            }
            n++;
            cur = 0;
        }
    }
    (void)k_end; (void)k_abort;

done:
    esp_ipc_isr_release_other_cpu();
    portENABLE_INTERRUPTS();
    return n > (int)sizeof(s_rx) ? (int)sizeof(s_rx) : n;
}

/* Checks length, CRC and command of the answer in s_rx */
static bool frame_ok(int n, uint8_t want_cmd, int min_words)
{
    if (n < 5)
    {
        return false;
    }
    int words = s_rx[0];
    int total = 4 + words * 4 + 1;
    if (words < min_words || n < total)
    {
        return false;
    }
    uint8_t crc = 0;
    for (int i = 0; i < total - 1; i++)
    {
        crc ^= s_rx[i];
    }
    if (crc != s_rx[total - 1])
    {
        return false;
    }
    return s_rx[3] == want_cmd;
}

void dc_maple_init(int gpio_a, int gpio_b)
{
    uint32_t mhz = esp_rom_get_cpu_ticks_per_us();
    s_mhz = mhz;
    s_gpio_a = gpio_a;
    s_gpio_b = gpio_b;
    s_boot_us = esp_timer_get_time();
    c100 = mhz / 10;
    c200 = mhz / 5;
    c500 = mhz / 2;
    c_end = mhz * 3;      /* B high for 3 us: end of frame */
    c_abort = mhz * 20;   /* line stuck 20 us: give up */

    s_a = 1u << gpio_a;
    s_b = 1u << gpio_b;
    int pins[2] = { gpio_a, gpio_b };
    for (int i = 0; i < 2; i++)
    {
        gpio_reset_pin((gpio_num_t)pins[i]);
        gpio_set_direction((gpio_num_t)pins[i], GPIO_MODE_INPUT);
        /* internal pull-ups keep the idle bus high (both ends drive it push-pull) */
        gpio_set_pull_mode((gpio_num_t)pins[i], GPIO_PULLUP_ONLY);
    }
    GPIO.out_w1ts = s_a | s_b;
    ESP_LOGI(LOG_TAG, "Reset reason %d", (int)esp_reset_reason());
    ESP_LOGI(LOG_TAG, "Maple host on GPIO%d (SDCKA) / GPIO%d (SDCKB), CPU %lu MHz",
             gpio_a, gpio_b, (unsigned long)mhz);
}

/* Bus variants tried until the pad answers: speed x1 / x2 slower, lines A/B as wired or swapped */
static void set_variant(int v)
{
    uint32_t mult = (v & 1) ? 2 : 1;
    c100 = s_mhz / 10 * mult;
    c200 = s_mhz / 5 * mult;
    c500 = s_mhz / 2 * mult;
    bool swap = (v & 2) != 0;
    s_a = 1u << (swap ? s_gpio_b : s_gpio_a);
    s_b = 1u << (swap ? s_gpio_a : s_gpio_b);
}

static bool detect_once(int v)
{
    set_variant(v);
    const uint8_t req[4] = { 0, ADDR_HOST, ADDR_MAIN, CMD_DEV_INFO };
    int idle_a = (GPIO.in & s_a) ? 1 : 0;
    int idle_b = (GPIO.in & s_b) ? 1 : 0;
    int n = maple_xfer(req, sizeof(req), 2000);
    if (!frame_ok(n, CMD_DEV_INFO_RSP, 1))
    {
        ESP_LOGW(LOG_TAG, "v%d (%s, %s): idle A=%d B=%d, %s, %d bytes %02x %02x %02x %02x %02x %02x %02x %02x",
                 v, (v & 1) ? "slow" : "fast", (v & 2) ? "swapped" : "as wired", idle_a, idle_b,
                 s_last_stage ? "answer started" : "no answer", n,
                 s_rx[0], s_rx[1], s_rx[2], s_rx[3], s_rx[4], s_rx[5], s_rx[6], s_rx[7]);
        memset(s_rx, 0, 8);
        vTaskDelay(pdMS_TO_TICKS(5));
        return false;
    }
    uint32_t func = s_rx[4] | (s_rx[5] << 8) | (s_rx[6] << 16) | ((uint32_t)s_rx[7] << 24);
    ESP_LOGI(LOG_TAG, "Pad found with v%d (%s, %s): functions 0x%08lx, %d words", v,
             (v & 1) ? "slow" : "fast", (v & 2) ? "swapped" : "as wired", (unsigned long)func, s_rx[0]);
    /* the pad needs a short pause after a long answer */
    vTaskDelay(pdMS_TO_TICKS(2));
    return (func & FUNC_CTRL) != 0;
}

bool dc_maple_detect(void)
{
    if (s_variant >= 0)
    {
        if (detect_once(s_variant))
        {
            return true;
        }
    }
    for (int v = 0; v < 4; v++)
    {
        if (v != s_variant && detect_once(v))
        {
            s_variant = v;
            return true;
        }
    }
    set_variant(s_variant >= 0 ? s_variant : 0);
    return false;
}

bool dc_maple_ever_found(void)
{
    return s_variant >= 0;
}

/* No pad ever answered for 20 s after a power-on: open the Wi-Fi update mode,
 * so a firmware that cannot read the pad can always be replaced over the air. */
void dc_no_pad_fallback(void)
{
    /* whatever the reset reason: the Switch mode restarts the ESP32 on its own */
    if (s_variant >= 0)
    {
        return;
    }
    if (esp_timer_get_time() - s_boot_us > 20000000)
    {
        ESP_LOGW(LOG_TAG, "No pad for 20 s: OTA mode");
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (p != NULL && esp_ota_set_boot_partition(p) == ESP_OK)
        {
            esp_restart();
        }
    }
}

bool dc_maple_read(dc_pad_state_t *st)
{
    const uint8_t req[8] = { 1, ADDR_HOST, ADDR_MAIN, CMD_GET_CONDITION,
                             FUNC_CTRL & 0xFF, 0, 0, 0 };
    int n = maple_xfer(req, sizeof(req), 1000);
    if (!frame_ok(n, CMD_DATA_TX, 3))
    {
        s_errors++;
        return false;
    }
    /* Layout checked against BlueRetro (struct dc_map, which feeds real consoles):
     * [8] L trigger, [9] R trigger, [10..11] buttons (uint16 LE, active low,
     * [11] = C B A Start Up Down Left Right, [10] = Z Y X D ...),
     * [12] stick 2 Y, [13] stick 2 X, [14] stick Y, [15] stick X */
    st->ltrig = s_rx[8];
    st->rtrig = s_rx[9];
    st->buttons = (uint16_t)~(s_rx[11] | (s_rx[10] << 8));
    st->joy_y = s_rx[14];
    st->joy_x = s_rx[15];
    static uint16_t last = 0x5A5A;
    if (st->buttons != last)
    {
        last = st->buttons;
        ESP_LOGI(LOG_TAG, "Buttons 0x%04x  L=%u R=%u X=%u Y=%u", st->buttons,
                 st->ltrig, st->rtrig, st->joy_x, st->joy_y);
    }
    return true;
}

/*
 * VMU screen test: the LCD of the VMU in slot 1 shows 8 horizontal bands.
 * Bands are 2 rows high (12 bytes = 3 whole words), so the picture is the same
 * whatever the byte order inside a word: a safe first test of the bus.
 */
bool dc_maple_vmu_lcd_test(void)
{
    const uint8_t sub = 0x01;   /* expansion slot 1, port A */
    const uint8_t info[4] = { 0, ADDR_HOST, sub, CMD_DEV_INFO };
    int n = maple_xfer(info, sizeof(info), 2000);
    if (!frame_ok(n, CMD_DEV_INFO_RSP, 1))
    {
        ESP_LOGI(LOG_TAG, "No VMU in slot 1 (%d bytes)", n);
        return false;
    }
    uint32_t func = s_rx[4] | (s_rx[5] << 8) | (s_rx[6] << 16) | ((uint32_t)s_rx[7] << 24);
    ESP_LOGI(LOG_TAG, "Slot 1: functions 0x%08lx", (unsigned long)func);
    vTaskDelay(pdMS_TO_TICKS(2));
    if (!(func & 0x04))
    {
        return false;
    }
    static uint8_t req[4 + 8 + 192];
    req[0] = 2 + 48;            /* words after the header */
    req[1] = ADDR_HOST;
    req[2] = sub;
    req[3] = 0x0C;              /* BLOCK_WRITE */
    req[4] = 0x04; req[5] = 0; req[6] = 0; req[7] = 0;    /* function: LCD */
    memset(&req[8], 0, 4);                                  /* block 0, phase 0 */
    for (int i = 0; i < 192; i++)
    {
        req[12 + i] = ((i / 12) & 1) ? 0x00 : 0xFF;
    }
    n = maple_xfer(req, sizeof(req), 2000);
    bool ok = frame_ok(n, 0x07, 0);   /* ACK */
    ESP_LOGI(LOG_TAG, "VMU screen test: %s", ok ? "ACK" : "no ACK");
    return ok;
}

/* ---------------- VMU memory (slot 1), read-only for now ---------------- */

#define CMD_BLOCK_READ 0x0B
#define VMU_SUB        0x01     /* expansion slot 1, port A */

/* Reads one 512-byte block of the VMU in slot 1, in bus (wire) byte order,
 * which is also the order BlueRetro keeps its own virtual VMU in. */
bool dc_maple_vmu_read_block(uint8_t block, uint8_t *out)
{
    const uint8_t req[12] = { 2, ADDR_HOST, VMU_SUB, CMD_BLOCK_READ,
                              0x02, 0, 0, 0,          /* function: memory */
                              block, 0, 0, 0 };       /* block, phase 0 */
    for (int attempt = 0; attempt < 3; attempt++)
    {
        int n = maple_xfer(req, sizeof(req), 3000);
        /* [4..7] function, [8..11] address echo (block in byte 8), [12..523] data */
        if (frame_ok(n, CMD_DATA_TX, 130) && s_rx[8] == block)
        {
            memcpy(out, &s_rx[12], 512);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    return false;
}

/* wire order -> file order: each 32-bit word is sent with its bytes reversed */
static inline uint8_t fb(const uint8_t *w, int i)
{
    return w[(i & ~3) + (3 - (i & 3))];
}

/* Test: reads the VMU in slot 1 and lists its saves in the log. Read-only. */
int dc_maple_vmu_read_test(int *blocks_ok, int *ms)
{
    static uint8_t blk[512];
    int64_t t0 = esp_timer_get_time();
    if (!dc_maple_vmu_read_block(255, blk))
    {
        ESP_LOGW(LOG_TAG, "VMU: root block 255 not readable");
        return -1;
    }
    /* root block: FAT block at 0x46, directory at 0x4A (size 0x4C), user blocks 0x50 */
    bool swap;
    uint16_t fat_w = blk[0x46] | (blk[0x47] << 8), dir_w = blk[0x4A] | (blk[0x4B] << 8);
    uint16_t fat_s = fb(blk, 0x46) | (fb(blk, 0x47) << 8), dir_s = fb(blk, 0x4A) | (fb(blk, 0x4B) << 8);
    if (fat_s == 254 && dir_s == 253) swap = true;
    else if (fat_w == 254 && dir_w == 253) swap = false;
    else
    {
        ESP_LOGW(LOG_TAG, "VMU: root block not understood (fat %u/%u dir %u/%u), first bytes %02x %02x %02x %02x",
                 fat_w, fat_s, dir_w, dir_s, blk[0], blk[1], blk[2], blk[3]);
        return -2;
    }
#define RB(i) (swap ? fb(blk, (i)) : blk[(i)])
    uint16_t dir_blk = RB(0x4A) | (RB(0x4B) << 8);
    uint16_t dir_len = RB(0x4C) | (RB(0x4D) << 8);
    ESP_LOGI(LOG_TAG, "VMU: formatted, byte order %s, directory %u (%u blocks)",
             swap ? "swapped per word" : "as sent", dir_blk, dir_len);
    int files = 0, used = 0;
    for (int b = 0; b < dir_len && b < 16; b++)
    {
        if (!dc_maple_vmu_read_block((uint8_t)(dir_blk - b), blk))
        {
            ESP_LOGW(LOG_TAG, "VMU: directory block %d not readable", dir_blk - b);
            return -3;
        }
        for (int e = 0; e < 16; e++)
        {
            int o = e * 32;
            uint8_t type = RB(o);
            if (type != 0x33 && type != 0xCC)
            {
                continue;
            }
            char name[13];
            for (int k = 0; k < 12; k++)
            {
                char c = (char)RB(o + 4 + k);
                name[k] = (c >= 32 && c < 127) ? c : '?';
            }
            name[12] = 0;
            uint16_t size = RB(o + 0x18) | (RB(o + 0x19) << 8);
            used += size;
            files++;
            ESP_LOGI(LOG_TAG, "VMU file %2d: %-12s %3u blocks (%s)", files, name, size,
                     type == 0xCC ? "mini-game" : "save");
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
#undef RB
    ESP_LOGI(LOG_TAG, "VMU: %d files, %d blocks used", files, used);

    /* speed test: read the 256 blocks, as a full copy would */
    int ok = 0;
    uint32_t sum = 0;
    int64_t t1 = esp_timer_get_time();
    for (int b = 0; b < 256; b++)
    {
        if (dc_maple_vmu_read_block((uint8_t)b, blk))
        {
            ok++;
            for (int i = 0; i < 512; i++) sum += blk[i];
        }
        vTaskDelay(1);
    }
    *ms = (int)((esp_timer_get_time() - t1) / 1000);
    *blocks_ok = ok;
    ESP_LOGI(LOG_TAG, "VMU: full read %d/256 blocks in %d ms (checksum %08lx), total test %lld ms",
             ok, *ms, (unsigned long)sum, (esp_timer_get_time() - t0) / 1000);
    return files;
}

/* ---------------- VMU copy at power-on ---------------- */

/* The copy is kept in the free flash after the last partition (www ends at
 * 0x317000): 128 KB at 0x3E0000, only rewritten where it changed. Reading
 * the VMU happens before Bluetooth starts, so the bus transfers never
 * disturb the radio. */
static int s_vmu_state = DC_VMU_NONE;

int dc_vmu_state(void)
{
    return s_vmu_state;
}

static bool vmu_present(void)
{
    const uint8_t info[4] = { 0, ADDR_HOST, VMU_SUB, CMD_DEV_INFO };
    for (int attempt = 0; attempt < 3; attempt++)
    {
        int n = maple_xfer(info, sizeof(info), 2000);
        if (frame_ok(n, CMD_DEV_INFO_RSP, 1))
        {
            uint32_t func = s_rx[4] | (s_rx[5] << 8) | (s_rx[6] << 16) | ((uint32_t)s_rx[7] << 24);
            ESP_LOGI(LOG_TAG, "Slot 1: functions 0x%08lx", (unsigned long)func);
            return (func & 0x02) != 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

int dc_vmu_snapshot(void)
{
    s_vmu_state = DC_VMU_NONE;
    if (!dc_maple_ever_found() && !dc_maple_detect())
    {
        ESP_LOGW(LOG_TAG, "VMU copy: no pad");
        return s_vmu_state;
    }
    uint32_t size = 0;
    if (esp_flash_get_size(NULL, &size) != ESP_OK || size < DC_VMU_FLASH_ADDR + DC_VMU_SIZE)
    {
        ESP_LOGE(LOG_TAG, "VMU copy: flash too small (%lu)", (unsigned long)size);
        s_vmu_state = DC_VMU_ERROR;
        return s_vmu_state;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    if (!vmu_present())
    {
        ESP_LOGI(LOG_TAG, "VMU copy: no VMU in slot 1");
        return s_vmu_state;
    }
    uint8_t *sec = malloc(4096);
    uint8_t *old = malloc(4096);
    if (sec == NULL || old == NULL)
    {
        free(sec);
        free(old);
        s_vmu_state = DC_VMU_ERROR;
        return s_vmu_state;
    }
    int64_t t0 = esp_timer_get_time();
    int written = 0;
    s_vmu_state = DC_VMU_OK;
    for (int s = 0; s < DC_VMU_SIZE / 4096 && s_vmu_state == DC_VMU_OK; s++)
    {
        for (int k = 0; k < 8; k++)
        {
            if (!dc_maple_vmu_read_block((uint8_t)(s * 8 + k), sec + k * 512))
            {
                ESP_LOGE(LOG_TAG, "VMU copy: block %d not readable", s * 8 + k);
                s_vmu_state = DC_VMU_ERROR;
                break;
            }
        }
        if (s_vmu_state != DC_VMU_OK)
        {
            break;
        }
        uint32_t addr = DC_VMU_FLASH_ADDR + s * 4096;
        if (esp_flash_read(NULL, old, addr, 4096) != ESP_OK || memcmp(old, sec, 4096) != 0)
        {
            if (esp_flash_erase_region(NULL, addr, 4096) != ESP_OK ||
                esp_flash_write(NULL, sec, addr, 4096) != ESP_OK ||
                esp_flash_read(NULL, old, addr, 4096) != ESP_OK || memcmp(old, sec, 4096) != 0)
            {
                ESP_LOGE(LOG_TAG, "VMU copy: flash write failed at 0x%06lx", (unsigned long)addr);
                s_vmu_state = DC_VMU_ERROR;
                break;
            }
            written++;
        }
        vTaskDelay(1);
    }
    free(sec);
    free(old);
    ESP_LOGI(LOG_TAG, "VMU copy: %s in %lld ms (%d sectors rewritten)",
             s_vmu_state == DC_VMU_OK ? "OK" : "FAILED", (esp_timer_get_time() - t0) / 1000, written);
    return s_vmu_state;
}

bool dc_vmu_copy_read(uint8_t block, uint8_t *out)
{
    if (s_vmu_state != DC_VMU_OK)
    {
        return false;
    }
    return esp_flash_read(NULL, out, DC_VMU_FLASH_ADDR + block * 512, 512) == ESP_OK;
}

/* ---------------- VMU writes (step 3) ---------------- */

#define CMD_BLOCK_WRITE    0x0C
#define CMD_WRITE_COMPLETE 0x0D
#define CMD_ACK            0x07

static bool vmu_cmd_ack(const uint8_t *req, int len)
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        int n = maple_xfer(req, len, 3000);
        if (frame_ok(n, CMD_ACK, 0))
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

/* Writes one 512-byte block (bus byte order) to the VMU in slot 1, the way
 * the console does: 4 phases of 128 bytes, then "write complete". */
bool dc_maple_vmu_write_block(uint8_t block, const uint8_t *data)
{
    static uint8_t req[12 + 128];
    for (int phase = 0; phase < 4; phase++)
    {
        req[0] = 2 + 32;
        req[1] = ADDR_HOST;
        req[2] = VMU_SUB;
        req[3] = CMD_BLOCK_WRITE;
        req[4] = 0x02; req[5] = 0; req[6] = 0; req[7] = 0;       /* function: memory */
        req[8] = block; req[9] = 0; req[10] = (uint8_t)phase; req[11] = 0;
        memcpy(&req[12], data + phase * 128, 128);
        if (!vmu_cmd_ack(req, sizeof(req)))
        {
            ESP_LOGE(LOG_TAG, "VMU write: block %u phase %d not acknowledged", block, phase);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));    /* let the VMU program its flash */
    }
    const uint8_t done[12] = { 2, ADDR_HOST, VMU_SUB, CMD_WRITE_COMPLETE,
                               0x02, 0, 0, 0,
                               block, 0, 4, 0 };
    if (!vmu_cmd_ack(done, sizeof(done)))
    {
        ESP_LOGW(LOG_TAG, "VMU write: block %u, no answer to write complete", block);
    }
    /* check: read the block back */
    static uint8_t chk[512];
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!dc_maple_vmu_read_block(block, chk) || memcmp(chk, data, 512) != 0)
    {
        ESP_LOGE(LOG_TAG, "VMU write: block %u read back differs", block);
        return false;
    }
    ESP_LOGI(LOG_TAG, "VMU write: block %u OK", block);
    return true;
}

/* VMU screen: 192 bytes (48 x 32 pixels), bus byte order as sent by the console */
bool dc_maple_vmu_lcd_write(const uint8_t *data)
{
    static uint8_t req[12 + 192];
    req[0] = 2 + 48;
    req[1] = ADDR_HOST;
    req[2] = VMU_SUB;
    req[3] = CMD_BLOCK_WRITE;
    req[4] = 0x04; req[5] = 0; req[6] = 0; req[7] = 0;           /* function: LCD */
    memset(&req[8], 0, 4);
    memcpy(&req[12], data, 192);
    int n = maple_xfer(req, sizeof(req), 3000);
    return frame_ok(n, CMD_ACK, 0);
}

/* Keeps the flash copy equal to the VMU after a write */
bool dc_vmu_copy_update(uint8_t block, const uint8_t *data)
{
    if (s_vmu_state != DC_VMU_OK)
    {
        return false;
    }
    uint32_t sec = DC_VMU_FLASH_ADDR + (block / 8) * 4096;
    uint8_t *buf = malloc(4096);
    if (buf == NULL)
    {
        return false;
    }
    bool ok = esp_flash_read(NULL, buf, sec, 4096) == ESP_OK;
    if (ok)
    {
        memcpy(buf + (block % 8) * 512, data, 512);
        ok = esp_flash_erase_region(NULL, sec, 4096) == ESP_OK &&
             esp_flash_write(NULL, buf, sec, 4096) == ESP_OK;
    }
    free(buf);
    if (!ok)
    {
        /* copy no longer trusted: BlueRetro will use its own VMU next time */
        s_vmu_state = DC_VMU_ERROR;
    }
    return ok;
}

/* Called by blu-energy before deep sleep: no pull-up may feed the unpowered pad */
void blu_energy_before_sleep(void)
{
    uint32_t pins = s_a | s_b;
    for (int i = 0; i < 32; i++)
    {
        if (pins & (1u << i))
        {
            gpio_set_direction((gpio_num_t)i, GPIO_MODE_INPUT);
            gpio_set_pull_mode((gpio_num_t)i, GPIO_FLOATING);
        }
    }
}

uint32_t dc_maple_errors(void)
{
    return s_errors;
}

/* ---------------- shared by the Switch and BlueRetro apps ---------------- */

#define COMBO_START_A (DC_BTN_START | DC_BTN_A)

/* A real hand cannot press Up+Down or Left+Right, nor 6+ buttons at once:
 * such states come from a pad that is still starting up. */
static bool plausible(uint16_t b)
{
    if ((b & (DC_BTN_UP | DC_BTN_DOWN)) == (DC_BTN_UP | DC_BTN_DOWN)) return false;
    if ((b & (DC_BTN_LEFT | DC_BTN_RIGHT)) == (DC_BTN_LEFT | DC_BTN_RIGHT)) return false;
    return __builtin_popcount(b) <= 5;
}

static bool combo_held(uint16_t b)
{
    return plausible(b) && (b & COMBO_START_A) == COMBO_START_A;
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

void dc_boot_ota_check(void)
{
    /* only a real power-on with the switch: a brownout reset while Start + A
     * are held (mode switch attempt) must not open the update mode */
    if (esp_reset_reason() != ESP_RST_POWERON)
    {
        return;
    }
    bool found = false;
    for (int i = 0; i < 10 && !found; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(60));
        found = dc_maple_detect();
    }
    if (!found)
    {
        return;
    }
    dc_pad_state_t st;
    int64_t start = esp_timer_get_time();
    int good = 0;
    while (esp_timer_get_time() - start < 2000000)
    {
        if (dc_maple_read(&st))
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
        ESP_LOGI(LOG_TAG, "Start + A held at power-on: OTA mode");
        boot_partition(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    }
}

void dc_mode_switch_check(uint16_t buttons)
{
    static bool armed = false;      /* combo must be released once after boot */
    static int64_t since = 0;
    bool held = combo_held(buttons);
    if (!held)
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
        ESP_LOGI(LOG_TAG, "Start + A held 5 s: switching mode");
        boot_partition(next);
    }
}

#endif /* CONFIG_BLUCONTROL_DREAMCAST */
