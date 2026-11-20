#include "blu-energy.h"
#include "blu-rumble.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#define LOG_TAG "BLU_ENERGY"

/*
 * Software power control (CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE).
 *
 * The battery is never disconnected: "off" is ESP32 deep sleep, woken up by the
 * power button (Waking-up Button 1, must be an RTC GPIO, active LOW).
 *
 *  - Power on : hold the power button POWER_ON_MS after waking up, otherwise the
 *               controller goes straight back to sleep.
 *  - OTA mode : hold the power button + OTA button from power-on for OTA_MS.
 *  - Power off: hold the power button ALONE for POWER_OFF_MS while running,
 *               or stay POWER_OFF_SECONDS without any connection.
 */

#ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE

#define PWR_BTN POWER_BUTTON_1

#ifdef CONFIG_BLUCONTROL_ENERGY_OTA_BUTTON_GPIO
    #define OTA_BTN CONFIG_BLUCONTROL_ENERGY_OTA_BUTTON_GPIO
#else
    #define OTA_BTN -1
#endif
#ifdef CONFIG_BLUCONTROL_ENERGY_POWER_ON_MS
    #define POWER_ON_MS CONFIG_BLUCONTROL_ENERGY_POWER_ON_MS
#else
    #define POWER_ON_MS 3000
#endif
#ifdef CONFIG_BLUCONTROL_ENERGY_POWER_OFF_MS
    #define POWER_OFF_MS CONFIG_BLUCONTROL_ENERGY_POWER_OFF_MS
#else
    #define POWER_OFF_MS 5000
#endif
#ifdef CONFIG_BLUCONTROL_ENERGY_OTA_MS
    #define OTA_MS CONFIG_BLUCONTROL_ENERGY_OTA_MS
#else
    #define OTA_MS 6000
#endif
#ifdef CONFIG_BLUCONTROL_ENERGY_PERIPH_POWER_GPIO
    #define PERIPH_PWR CONFIG_BLUCONTROL_ENERGY_PERIPH_POWER_GPIO
#else
    #define PERIPH_PWR -1
#endif

/* OTA button = power button: holding the power button alone long enough enters OTA */
#define OTA_IS_PWR (OTA_BTN == PWR_BTN)

/* Power switch of the controller itself (e.g. the 5 V of a Dreamcast pad) */
static void periph_power(bool on)
{
    if (PERIPH_PWR < 0)
    {
        return;
    }
    gpio_hold_dis((gpio_num_t)PERIPH_PWR);
    gpio_reset_pin((gpio_num_t)PERIPH_PWR);
    gpio_set_direction((gpio_num_t)PERIPH_PWR, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PERIPH_PWR, on ? 1 : 0);
    if (!on)
    {
        gpio_hold_en((gpio_num_t)PERIPH_PWR);
    }
}

static const int s_leds[] =
{
#if defined(CONFIG_BLUCONTROL_MODE1_LED_GPIO)
    CONFIG_BLUCONTROL_MODE1_LED_GPIO,
#else
    -1,
#endif
#if defined(CONFIG_BLUCONTROL_MODE2_LED_GPIO)
    CONFIG_BLUCONTROL_MODE2_LED_GPIO,
#else
    -1,
#endif
};
#define LEDS_COUNT (sizeof(s_leds) / sizeof(s_leds[0]))

TaskHandle_t clockTaskHandle = NULL;
static TaskHandle_t s_buttonTaskHandle = NULL;
int64_t clock_start_time = 0;
static volatile bool s_powering_off = false;

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool is_pressed(int gpio)
{
    return gpio >= 0 && gpio_get_level((gpio_num_t)gpio) == 0;
}

static void leds_set_all(bool on)
{
    for (int i = 0; i < LEDS_COUNT; i++)
    {
        if (s_leds[i] >= 0)
        {
            gpio_set_level((gpio_num_t)s_leds[i], on ? LED_POWER_ON : !LED_POWER_ON);
        }
    }
}

/* LED of the app that is running (same rule as blucontrol_mode_init, with OTA) */
static int current_mode_led(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    int mode = (int)p->subtype - (ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 1);
    if (mode >= 0 && mode < LEDS_COUNT)
    {
        return s_leds[mode];
    }
    return -1;
}

/* Lets an app park its own pins before sleeping (e.g. the Maple bus lines) */
__attribute__((weak)) void blu_energy_before_sleep(void)
{
}

static void enter_deep_sleep(void)
{
    blu_rumble_stop();
    blu_energy_before_sleep();
    periph_power(false);

    /* Wait for the power button to be released, or it would wake us up at once */
    int64_t released_since = 0;
    int64_t start = now_ms();
    while (now_ms() - start < 15000)
    {
        if (!is_pressed(PWR_BTN))
        {
            if (released_since == 0)
            {
                released_since = now_ms();
            }
            else if (now_ms() - released_since >= 100)
            {
                break;
            }
        }
        else
        {
            released_since = 0;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    /* Keep the LEDs off while sleeping (their pins would float otherwise) */
    for (int i = 0; i < LEDS_COUNT; i++)
    {
        if (s_leds[i] >= 0)
        {
            gpio_reset_pin((gpio_num_t)s_leds[i]);
            gpio_set_direction((gpio_num_t)s_leds[i], GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)s_leds[i], !LED_POWER_ON);
            gpio_hold_en((gpio_num_t)s_leds[i]);
        }
    }
    gpio_deep_sleep_hold_en();

    if (PWR_BTN >= 0 && rtc_gpio_is_valid_gpio((gpio_num_t)PWR_BTN))
    {
        esp_sleep_enable_ext0_wakeup((gpio_num_t)PWR_BTN, 0);
        rtc_gpio_pullup_en((gpio_num_t)PWR_BTN);
        rtc_gpio_pulldown_dis((gpio_num_t)PWR_BTN);
    }
    else
    {
        ESP_LOGE(LOG_TAG, "Power button %d is not an RTC GPIO: only a reset will wake up the controller", PWR_BTN);
    }

    ESP_LOGI(LOG_TAG, "Going to deep sleep");
    esp_deep_sleep_start();
}

void blu_energy_power_off(void)
{
    if (s_powering_off)
    {
        return;
    }
    s_powering_off = true;
    ESP_LOGI(LOG_TAG, "Turning off");
    blu_rumble_stop();

    /* 3 quick blinks: "turning off" */
    for (int i = 0; i < 3; i++)
    {
        leds_set_all(false);
        vTaskDelay(150 / portTICK_PERIOD_MS);
        int led = current_mode_led();
        if (led >= 0)
        {
            gpio_set_level((gpio_num_t)led, LED_POWER_ON);
        }
        vTaskDelay(150 / portTICK_PERIOD_MS);
    }
    leds_set_all(false);
    enter_deep_sleep();
}

static void switch_to_ota_and_restart(void)
{
    const esp_partition_t *ota = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (ota != NULL && esp_ota_set_boot_partition(ota) == ESP_OK)
    {
        periph_power(false);
        ESP_LOGI(LOG_TAG, "Power button + OTA button held: switching to OTA mode");
        esp_restart();
    }
    ESP_LOGE(LOG_TAG, "Could not switch to OTA mode");
}

/* Runs at boot. Returns only if the controller must really start. */
static void power_on_check(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool cold = (reason == ESP_RST_DEEPSLEEP || reason == ESP_RST_POWERON ||
                 reason == ESP_RST_BROWNOUT || reason == ESP_RST_EXT);
    if (!cold)
    {
        /* Software restart (mode switch, OTA exit, Switch reconnection...): keep running */
        return;
    }

    /* Power on: the button must stay pressed POWER_ON_MS */
    int64_t released_since = 0;
    while (now_ms() < POWER_ON_MS)
    {
        if (!is_pressed(PWR_BTN))
        {
            if (released_since == 0)
            {
                released_since = now_ms();
            }
            else if (now_ms() - released_since >= 50)
            {
                ESP_LOGI(LOG_TAG, "Power button released too early: back to sleep");
                enter_deep_sleep();
            }
        }
        else
        {
            released_since = 0;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    int led = current_mode_led();
    if (led >= 0)
    {
        gpio_set_level((gpio_num_t)led, LED_POWER_ON);
    }
    ESP_LOGI(LOG_TAG, "Powered on");

    /* Power + OTA buttons still held: wait for OTA_MS (or a release) */
    while (is_pressed(PWR_BTN) && (OTA_IS_PWR || is_pressed(OTA_BTN)))
    {
        if (now_ms() >= OTA_MS)
        {
            switch_to_ota_and_restart();
            break;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

static void power_button_task(void *params)
{
    /* The button may still be held from the power-on: wait for a release first */
    bool wait_release = true;
    int64_t hold_start = 0;
    while (true)
    {
        bool pwr = is_pressed(PWR_BTN);
        if (wait_release)
        {
            if (!pwr)
            {
                wait_release = false;
            }
        }
        else if (pwr && (OTA_IS_PWR || !is_pressed(OTA_BTN)))
        {
            if (hold_start == 0)
            {
                hold_start = now_ms();
            }
            else if (now_ms() - hold_start >= POWER_OFF_MS)
            {
                blu_energy_power_off();
            }
        }
        else
        {
            hold_start = 0;
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

void deep_sleep_clock(void *params)
{
    /* Deleted by blu_energy_stop_clock() */
    while (true)
    {
        if ((esp_timer_get_time() - clock_start_time) / 1000000LL >= POWER_OFF_SECONDS)
        {
            ESP_LOGI(LOG_TAG, "No connection for %d s", POWER_OFF_SECONDS);
            blu_energy_power_off();
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}
#endif

void blu_energy_init()
{
    #ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE
    /* Undo what the previous deep sleep set up */
    gpio_deep_sleep_hold_dis();
    for (int i = 0; i < LEDS_COUNT; i++)
    {
        if (s_leds[i] >= 0)
        {
            gpio_hold_dis((gpio_num_t)s_leds[i]);
            gpio_reset_pin((gpio_num_t)s_leds[i]);
            gpio_set_direction((gpio_num_t)s_leds[i], GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)s_leds[i], !LED_POWER_ON);
        }
    }
    int buttons[2] = { PWR_BTN, OTA_BTN };
    for (int i = 0; i < 2; i++)
    {
        if (buttons[i] < 0)
        {
            continue;
        }
        if (rtc_gpio_is_valid_gpio((gpio_num_t)buttons[i]))
        {
            rtc_gpio_deinit((gpio_num_t)buttons[i]);
        }
        gpio_reset_pin((gpio_num_t)buttons[i]);
        gpio_set_direction((gpio_num_t)buttons[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)buttons[i], GPIO_PULLUP_ONLY);
    }
    vTaskDelay(5 / portTICK_PERIOD_MS);

    power_on_check();
    periph_power(true);

    xTaskCreatePinnedToCore(power_button_task, "BLU_POWER_BTN", 3072, NULL, tskIDLE_PRIORITY + 1, &s_buttonTaskHandle, 1);
    configASSERT(s_buttonTaskHandle);
    #endif
}

bool blu_energy_is_clock_running()
{
    #ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE
    return clockTaskHandle != NULL;
    #else
    return false;
    #endif
}

void blu_energy_start_clock()
{
    #ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE
    if (clockTaskHandle == NULL)
    {
        clock_start_time = esp_timer_get_time();
        xTaskCreatePinnedToCore(deep_sleep_clock, "DEEP_SLEEP_CLOCK", 3072, NULL, tskIDLE_PRIORITY, &clockTaskHandle, 1);
        configASSERT(clockTaskHandle);
    }
    #endif
}

void blu_energy_stop_clock()
{
    #ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE
    if (clockTaskHandle != NULL)
    {
        vTaskDelete(clockTaskHandle);
        clockTaskHandle = NULL;
    }
    clock_start_time = 0;
    #endif
}
