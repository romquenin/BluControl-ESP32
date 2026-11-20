#include "blu-rumble.h"

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_log.h"

#define LOG_TAG "BLU_RUMBLE"

#ifndef CONFIG_BLUCONTROL_RUMBLE_LEFT_GPIO
    #define CONFIG_BLUCONTROL_RUMBLE_LEFT_GPIO -1
#endif
#ifndef CONFIG_BLUCONTROL_RUMBLE_RIGHT_GPIO
    #define CONFIG_BLUCONTROL_RUMBLE_RIGHT_GPIO -1
#endif
#ifndef CONFIG_BLUCONTROL_RUMBLE_MIN_POWER
    #define CONFIG_BLUCONTROL_RUMBLE_MIN_POWER 0
#endif
#ifndef CONFIG_BLUCONTROL_RUMBLE_MAX_ON_MS
    #define CONFIG_BLUCONTROL_RUMBLE_MAX_ON_MS 0
#endif

#define RUMBLE_LEFT_GPIO   CONFIG_BLUCONTROL_RUMBLE_LEFT_GPIO
#define RUMBLE_RIGHT_GPIO  CONFIG_BLUCONTROL_RUMBLE_RIGHT_GPIO

#define RUMBLE_LEDC_MODE   LEDC_LOW_SPEED_MODE
#define RUMBLE_LEDC_TIMER  LEDC_TIMER_3          /* keep away from timers other code may use */
#define RUMBLE_LEDC_LEFT   LEDC_CHANNEL_6
#define RUMBLE_LEDC_RIGHT  LEDC_CHANNEL_7
#define RUMBLE_PWM_FREQ_HZ 20000                 /* above hearing range: no motor whine */

static bool s_ready = false;
static esp_timer_handle_t s_safety_timer = NULL;

static uint8_t apply_min_power(uint8_t v)
{
    /* Small ERM motors do not spin below a certain duty: either jump straight
     * to the minimum usable power or stay off. */
    if (v == 0)
    {
        return 0;
    }
#if CONFIG_BLUCONTROL_RUMBLE_MIN_POWER > 0
    if (v < CONFIG_BLUCONTROL_RUMBLE_MIN_POWER)
    {
        return CONFIG_BLUCONTROL_RUMBLE_MIN_POWER;
    }
#endif
    return v;
}

static void set_channel(ledc_channel_t ch, uint8_t duty)
{
    ledc_set_duty(RUMBLE_LEDC_MODE, ch, duty);
    ledc_update_duty(RUMBLE_LEDC_MODE, ch);
}

static void safety_timeout_cb(void *arg)
{
    ESP_LOGW(LOG_TAG, "Rumble on for too long without update: stopping motor");
    blu_rumble_stop();
}

static void init_channel(int gpio, ledc_channel_t ch)
{
    ledc_channel_config_t cfg = {
        .gpio_num = gpio,
        .speed_mode = RUMBLE_LEDC_MODE,
        .channel = ch,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = RUMBLE_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&cfg));
}

bool blu_rumble_is_enabled(void)
{
    return RUMBLE_LEFT_GPIO >= 0 || RUMBLE_RIGHT_GPIO >= 0;
}

void blu_rumble_init(void)
{
    if (!blu_rumble_is_enabled() || s_ready)
    {
        return;
    }

    ledc_timer_config_t timer = {
        .speed_mode = RUMBLE_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = RUMBLE_LEDC_TIMER,
        .freq_hz = RUMBLE_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    if (RUMBLE_LEFT_GPIO >= 0)
    {
        init_channel(RUMBLE_LEFT_GPIO, RUMBLE_LEDC_LEFT);
    }
    if (RUMBLE_RIGHT_GPIO >= 0)
    {
        init_channel(RUMBLE_RIGHT_GPIO, RUMBLE_LEDC_RIGHT);
    }

    if (CONFIG_BLUCONTROL_RUMBLE_MAX_ON_MS > 0)
    {
        const esp_timer_create_args_t args = {
            .callback = safety_timeout_cb,
            .name = "rumble_safety",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_safety_timer));
    }

    s_ready = true;
    ESP_LOGI(LOG_TAG, "Rumble ready (left GPIO %d, right GPIO %d)", RUMBLE_LEFT_GPIO, RUMBLE_RIGHT_GPIO);
}

/* Lets an app forward rumble to a pad that drives its own motor (GameCube cable) */
__attribute__((weak)) void blu_rumble_ext(uint8_t left, uint8_t right)
{
}

void blu_rumble_set(uint8_t left, uint8_t right)
{
    blu_rumble_ext(left, right);
    if (!s_ready)
    {
        return;
    }

    uint8_t l = left, r = right;

    /* Single-motor controllers: merge both requests */
    if (RUMBLE_RIGHT_GPIO < 0)
    {
        l = left > right ? left : right;
    }
    else if (RUMBLE_LEFT_GPIO < 0)
    {
        r = left > right ? left : right;
    }

    l = apply_min_power(l);
    r = apply_min_power(r);

    if (RUMBLE_LEFT_GPIO >= 0)
    {
        set_channel(RUMBLE_LEDC_LEFT, l);
    }
    if (RUMBLE_RIGHT_GPIO >= 0)
    {
        set_channel(RUMBLE_LEDC_RIGHT, r);
    }

    if (s_safety_timer != NULL)
    {
        esp_timer_stop(s_safety_timer);  /* ignore "not running" error */
        if (l || r)
        {
            esp_timer_start_once(s_safety_timer, (uint64_t)CONFIG_BLUCONTROL_RUMBLE_MAX_ON_MS * 1000ULL);
        }
    }
}

void blu_rumble_stop(void)
{
    blu_rumble_ext(0, 0);
    if (!s_ready)
    {
        return;
    }
    if (RUMBLE_LEFT_GPIO >= 0)
    {
        set_channel(RUMBLE_LEDC_LEFT, 0);
    }
    if (RUMBLE_RIGHT_GPIO >= 0)
    {
        set_channel(RUMBLE_LEDC_RIGHT, 0);
    }
}
