#include <string.h>
#include "main.h"

#define LOG_TAG "BLU_SWITCH"

#ifdef CONFIG_BLUCONTROL_GC_CABLE
#include "gc_joybus.h"

/* Latest state of the GameCube pad, written by gc_poll_task (core 1) */
static gc_pad_state_t s_gc = { .sx = 128, .sy = 128, .cx = 128, .cy = 128 };
static portMUX_TYPE s_gc_mux = portMUX_INITIALIZER_UNLOCKED;

#define GC_AXIS(v) ((int)(v) * (BLU_JOYSTICK_ABS_MAX * 2) / 255)
#define GC_TRIGGER_ON 100     /* analog trigger -> ZL / ZR above ~40 % */

static void gc_poll_task(void *params)
{
    bool found = false;
    int fails = 0;
    gc_pad_state_t st;
    while (true)
    {
        if (!found)
        {
            found = gc_joybus_detect();
            if (!found)
            {
#if defined(CONFIG_BLUCONTROL_MODE1_LED_GPIO) && CONFIG_BLUCONTROL_MODE1_LED_GPIO >= 0
                static bool blink = false;
                blink = !blink;
                gpio_set_level(CONFIG_BLUCONTROL_MODE1_LED_GPIO, blink ? LED_POWER_ON : !LED_POWER_ON);
#endif
                gc_no_pad_fallback();
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
#if defined(CONFIG_BLUCONTROL_MODE1_LED_GPIO) && CONFIG_BLUCONTROL_MODE1_LED_GPIO >= 0
            gpio_set_level(CONFIG_BLUCONTROL_MODE1_LED_GPIO, LED_POWER_ON);
#endif
        }
        if (gc_joybus_read(&st))
        {
            fails = 0;
            portENTER_CRITICAL(&s_gc_mux);
            s_gc = st;
            portEXIT_CRITICAL(&s_gc_mux);
            gc_mode_switch_check(st.buttons);
        }
        else if (++fails > 50)
        {
            found = false;
            fails = 0;
            portENTER_CRITICAL(&s_gc_mux);
            memset(&s_gc, 0, sizeof(s_gc));
            s_gc.sx = s_gc.sy = s_gc.cx = s_gc.cy = 128;
            portEXIT_CRITICAL(&s_gc_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

static gc_pad_state_t gc_get(void)
{
    gc_pad_state_t st;
    portENTER_CRITICAL(&s_gc_mux);
    st = s_gc;
    portEXIT_CRITICAL(&s_gc_mux);
    return st;
}
#endif

#ifdef CONFIG_BLUCONTROL_DREAMCAST
#include "dc_maple.h"

/* Latest state of the Dreamcast pad, written by dc_poll_task (core 1) */
static dc_pad_state_t s_dc = { .joy_x = 128, .joy_y = 128 };
static portMUX_TYPE s_dc_mux = portMUX_INITIALIZER_UNLOCKED;

#define DC_AXIS(v) ((int)(v) * (BLU_JOYSTICK_ABS_MAX * 2) / 255)
#define DC_TRIGGER_ON 100     /* analog trigger -> ZL / ZR above ~40 % */

static void dc_poll_task(void *params)
{
    bool found = false;
    int fails = 0;
    dc_pad_state_t st;
    while (true)
    {
        if (!found)
        {
            found = dc_maple_detect();
            if (!found)
            {
                /* no pad: red LED blinks; after 20 s from power-on -> Wi-Fi update mode */
#if defined(CONFIG_BLUCONTROL_MODE1_LED_GPIO) && CONFIG_BLUCONTROL_MODE1_LED_GPIO >= 0
                static bool blink = false;
                blink = !blink;
                gpio_set_level(CONFIG_BLUCONTROL_MODE1_LED_GPIO, blink ? LED_POWER_ON : !LED_POWER_ON);
#endif
                dc_no_pad_fallback();
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
#if defined(CONFIG_BLUCONTROL_MODE1_LED_GPIO) && CONFIG_BLUCONTROL_MODE1_LED_GPIO >= 0
            gpio_set_level(CONFIG_BLUCONTROL_MODE1_LED_GPIO, LED_POWER_ON);
#endif
        }
        if (dc_maple_read(&st))
        {
            fails = 0;
            portENTER_CRITICAL(&s_dc_mux);
            s_dc = st;
            portEXIT_CRITICAL(&s_dc_mux);
            dc_mode_switch_check(st.buttons);
        }
        else if (++fails > 50)
        {
            found = false;
            fails = 0;
            portENTER_CRITICAL(&s_dc_mux);
            memset(&s_dc, 0, sizeof(s_dc));
            s_dc.joy_x = s_dc.joy_y = 128;
            portEXIT_CRITICAL(&s_dc_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

static dc_pad_state_t dc_get(void)
{
    dc_pad_state_t st;
    portENTER_CRITICAL(&s_dc_mux);
    st = s_dc;
    portEXIT_CRITICAL(&s_dc_mux);
    return st;
}
#endif

#if defined(CONFIG_BLUCONTROL_LEFT_TRIGGER_ANALOG) || defined(CONFIG_BLUCONTROL_RIGHT_TRIGGER_ANALOG)
#ifdef CONFIG_BLUCONTROL_LEFT_TRIGGER_ANALOG
bool is_left_trigger_pressed = false;
#endif
#ifdef CONFIG_BLUCONTROL_RIGHT_TRIGGER_ANALOG
bool is_right_trigger_pressed = false;
#endif
#endif

#if defined(CONFIG_BLUCONTROL_GC_CABLE)
/* Switch buttons by name, like the wired BluControl GameCube:
 * A, B, X, Y, L/R (click or analog > ~40 %), Z -> ZR, Start -> +.
 * Start + X = Home, Start + Y = Minus. */
void button_task()
{
    gc_pad_state_t st = gc_get();
    uint16_t b = st.buttons;
    bool start = b & GC_BTN_START;
    bool home = false, minus = false;
    if (start && (b & GC_BTN_X))
    {
        home = true;
        start = false;
        b &= ~GC_BTN_X;
    }
    if (start && (b & GC_BTN_Y))
    {
        minus = true;
        start = false;
        b &= ~GC_BTN_Y;
    }
    hoja_button_data.button_right  |= (b & GC_BTN_A) != 0;
    hoja_button_data.button_down   |= (b & GC_BTN_B) != 0;
    hoja_button_data.button_up     |= (b & GC_BTN_X) != 0;
    hoja_button_data.button_left   |= (b & GC_BTN_Y) != 0;
    hoja_button_data.dpad_up       |= (b & GC_BTN_UP) != 0;
    hoja_button_data.dpad_down     |= (b & GC_BTN_DOWN) != 0;
    hoja_button_data.dpad_left     |= (b & GC_BTN_LEFT) != 0;
    hoja_button_data.dpad_right    |= (b & GC_BTN_RIGHT) != 0;
    hoja_button_data.trigger_l     |= ((b & GC_BTN_L) != 0) || st.l > GC_TRIGGER_ON;
    hoja_button_data.trigger_r     |= ((b & GC_BTN_R) != 0) || st.r > GC_TRIGGER_ON;
    hoja_button_data.trigger_zr    |= (b & GC_BTN_Z) != 0;
    hoja_button_data.button_start  |= start;
    hoja_button_data.button_home   |= home;
    hoja_button_data.button_select |= minus;
}
#elif defined(CONFIG_BLUCONTROL_DREAMCAST)
/* Switch buttons by position: DC A (bottom) -> B, DC B (right) -> A,
 * DC X (left) -> Y, DC Y (top) -> X. Start + X = Home, Start + Y = Minus. */
void button_task()
{
    dc_pad_state_t st = dc_get();
    uint16_t b = st.buttons;
    bool start = b & DC_BTN_START;
    bool home = false, minus = false;
    if (start && (b & DC_BTN_X))
    {
        home = true;
        start = false;
        b &= ~DC_BTN_X;
    }
    if (start && (b & DC_BTN_Y))
    {
        minus = true;
        start = false;
        b &= ~DC_BTN_Y;
    }
    hoja_button_data.button_down   |= (b & DC_BTN_A) != 0;
    hoja_button_data.button_right  |= (b & DC_BTN_B) != 0;
    hoja_button_data.button_left   |= (b & DC_BTN_X) != 0;
    hoja_button_data.button_up     |= (b & DC_BTN_Y) != 0;
    hoja_button_data.dpad_up       |= (b & DC_BTN_UP) != 0;
    hoja_button_data.dpad_down     |= (b & DC_BTN_DOWN) != 0;
    hoja_button_data.dpad_left     |= (b & DC_BTN_LEFT) != 0;
    hoja_button_data.dpad_right    |= (b & DC_BTN_RIGHT) != 0;
    hoja_button_data.trigger_zl    |= st.ltrig > DC_TRIGGER_ON;
    hoja_button_data.trigger_zr    |= st.rtrig > DC_TRIGGER_ON;
    hoja_button_data.button_start  |= start;
    hoja_button_data.button_home   |= home;
    hoja_button_data.button_select |= minus;
}
#else
void button_task()
{
    blu_refresh_buttons();

    hoja_button_data.button_right       |= blu_buttons.button_A.value;
    hoja_button_data.button_down        |= blu_buttons.button_B.value;
    hoja_button_data.button_left        |= blu_buttons.button_Y.value;
    hoja_button_data.button_up          |= blu_buttons.button_X.value;

    hoja_button_data.dpad_right         |= blu_buttons.dpad_right.value;
    hoja_button_data.dpad_down          |= blu_buttons.dpad_down.value;
    hoja_button_data.dpad_left          |= blu_buttons.dpad_left.value;
    hoja_button_data.dpad_up            |= blu_buttons.dpad_up.value;

    #ifdef CONFIG_BLUCONTROL_LEFT_TRIGGER_BTN_L
    hoja_button_data.trigger_l          |= blu_buttons.trigger_l.value | is_left_trigger_pressed;
    #else
    hoja_button_data.trigger_l          |= blu_buttons.trigger_l.value;
    #endif
    #ifdef CONFIG_BLUCONTROL_LEFT_TRIGGER_BTN_ZL
    hoja_button_data.trigger_zl         |= blu_buttons.trigger_zl.value | is_left_trigger_pressed;
    #else
    hoja_button_data.trigger_zl         |= blu_buttons.trigger_zl.value;
    #endif
    #ifdef CONFIG_BLUCONTROL_RIGHT_TRIGGER_BTN_R
    hoja_button_data.trigger_r          |= blu_buttons.trigger_r.value | is_right_trigger_pressed;
    #else
    hoja_button_data.trigger_r          |= blu_buttons.trigger_r.value;
    #endif
    #ifdef CONFIG_BLUCONTROL_RIGHT_TRIGGER_BTN_ZR
    hoja_button_data.trigger_zr         |= blu_buttons.trigger_zr.value | is_right_trigger_pressed;
    #else
    hoja_button_data.trigger_zr         |= blu_buttons.trigger_zr.value;
    #endif

    hoja_button_data.button_start       |= blu_buttons.special_start.value;
    hoja_button_data.button_select      |= blu_buttons.special_select.value;
    hoja_button_data.button_home        |= blu_buttons.special_home.value;
    hoja_button_data.button_capture     |= blu_buttons.special_capture.value;

    hoja_button_data.button_stick_left  |= blu_buttons.button_stick_left.value;
    hoja_button_data.button_stick_right |= blu_buttons.button_stick_right.value;
}

#endif /* CONFIG_BLUCONTROL_DREAMCAST */

void event_task(hoja_event_type_t type, uint8_t evt, uint8_t param)
{
    ESP_LOGD(LOG_TAG, "Event: \n\ttype: %d\n\tevent: %d\n\tparam: %d", type, evt, param);
    if (type == HOJA_EVT_BT && evt == HEVT_BT_DISCONNECTED)
    {
        //This shouldn't be needed, but HOJA glitches after disconnecting the Switch... so... ¯\_(ツ)_/¯
        esp_restart();
    }

    if (type == HOJA_EVT_BT)
    {
        #ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE
        if (evt != HEVT_BT_CONNECTED)
        {
            if (!blu_energy_is_clock_running())
            {
                blu_energy_start_clock();
            }
        }
        else
        {
            if (blu_energy_is_clock_running())
            {
                blu_energy_stop_clock();
            }
        }
        #endif
    }
}

#if defined(CONFIG_BLUCONTROL_LEFT_STICK_ANALOG) || defined(CONFIG_BLUCONTROL_RIGHT_STICK_ANALOG)
blu_analog_stick_data_t *analog_stick_data;
#endif
#if defined(CONFIG_BLUCONTROL_LEFT_STICK_BUTTONS) || defined(CONFIG_BLUCONTROL_RIGHT_STICK_BUTTONS)
blu_btn_stick_data_t *button_stick_data;
#endif

// Separate task to read sticks.
// This is essential to have as a separate component as ADC scans typically take more time and this is only
// scanned once between each polling interval. This varies from core to core.
#if defined(CONFIG_BLUCONTROL_GC_CABLE)
void stick_task()
{
    gc_pad_state_t st = gc_get();
    /* GameCube and Switch: up = high */
    hoja_analog_data.ls_x = GC_AXIS(st.sx);
    hoja_analog_data.ls_y = GC_AXIS(st.sy);
    hoja_analog_data.rs_x = GC_AXIS(st.cx);
    hoja_analog_data.rs_y = GC_AXIS(st.cy);
}
#elif defined(CONFIG_BLUCONTROL_DREAMCAST)
void stick_task()
{
    dc_pad_state_t st = dc_get();
    hoja_analog_data.ls_x = DC_AXIS(st.joy_x);
    hoja_analog_data.ls_y = DC_AXIS(255 - st.joy_y);   /* Switch: up = high */
    hoja_analog_data.rs_x = GET_JOYSTICK_X_AXIS(0);
    hoja_analog_data.rs_y = GET_JOYSTICK_Y_AXIS(0);
}
#else
void stick_task()
{
    // Joystick
    #if defined(CONFIG_BLUCONTROL_LEFT_STICK_ANALOG)
    analog_stick_data = blu_analog_stick_get_data(BLU_ANALOG_PAD_LEFT);
    hoja_analog_data.ls_x = GET_ANALOG_JOYSTICK_X_AXIS(analog_stick_data->x_axis);
    hoja_analog_data.ls_y = GET_ANALOG_JOYSTICK_Y_AXIS(analog_stick_data->y_axis);
    #elif defined(CONFIG_BLUCONTROL_LEFT_STICK_N64)
    hoja_analog_data.ls_x = GET_JOYSTICK_X_AXIS(n64_left_joystick_data.x_axis.value);
    hoja_analog_data.ls_y = GET_JOYSTICK_Y_AXIS(n64_left_joystick_data.y_axis.value);
    #elif defined(CONFIG_BLUCONTROL_LEFT_STICK_BUTTONS)
    button_stick_data = blu_buttons_stick_get_data(BLU_BUTTONS_PAD_LEFT);
    hoja_analog_data.ls_x = GET_JOYSTICK_X_AXIS(button_stick_data->x_axis);
    hoja_analog_data.ls_y = GET_JOYSTICK_Y_AXIS(button_stick_data->y_axis);
    #else
    hoja_analog_data.ls_x = GET_JOYSTICK_X_AXIS(0);
    hoja_analog_data.ls_y = GET_JOYSTICK_Y_AXIS(0);
    #endif

    #if defined(CONFIG_BLUCONTROL_RIGHT_STICK_ANALOG)
    analog_stick_data = blu_analog_stick_get_data(BLU_ANALOG_PAD_RIGHT);
    hoja_analog_data.rs_x = GET_ANALOG_JOYSTICK_X_AXIS(analog_stick_data->x_axis);
    hoja_analog_data.rs_y = GET_ANALOG_JOYSTICK_Y_AXIS(analog_stick_data->y_axis);
    #elif defined(CONFIG_BLUCONTROL_RIGHT_STICK_N64)
    hoja_analog_data.rs_x = GET_JOYSTICK_X_AXIS(n64_right_joystick_data.x_axis.value);
    hoja_analog_data.rs_y = GET_JOYSTICK_Y_AXIS(n64_right_joystick_data.y_axis.value);
    #elif defined(CONFIG_BLUCONTROL_RIGHT_STICK_BUTTONS)
    button_stick_data = blu_buttons_stick_get_data(BLU_BUTTONS_PAD_RIGHT);
    hoja_analog_data.rs_x = GET_JOYSTICK_X_AXIS(button_stick_data->x_axis);
    hoja_analog_data.rs_y = GET_JOYSTICK_Y_AXIS(button_stick_data->y_axis);
    #else
    hoja_analog_data.rs_x = GET_JOYSTICK_X_AXIS(0);
    hoja_analog_data.rs_y = GET_JOYSTICK_Y_AXIS(0);
    #endif

    #if defined(CONFIG_BLUCONTROL_LEFT_TRIGGER_ANALOG)
    is_left_trigger_pressed = CHECK_IF_TRIGGER_IS_PRESSED(GET_ANALOG_TRIGGER_AXIS(blu_analog_trigger_get_value(BLU_ANALOG_TRIGGER_LEFT)), LEFT_TRIGGER_ACT);
    #endif
    #if defined(CONFIG_BLUCONTROL_RIGHT_TRIGGER_ANALOG)
    is_right_trigger_pressed = CHECK_IF_TRIGGER_IS_PRESSED(GET_ANALOG_TRIGGER_AXIS(blu_analog_trigger_get_value(BLU_ANALOG_TRIGGER_RIGHT)), RIGHT_TRIGGER_ACT);
    #endif
}

#endif /* CONFIG_BLUCONTROL_DREAMCAST */

void rumble_task(hoja_rumble_data_s* rumble_data)
{
    /* left/right_rumble are decoded HD-rumble amplitudes (0..255) */
    blu_rumble_set(rumble_data->left_rumble, rumble_data->right_rumble);
}

void app_main(void)
{
    ESP_LOGD(LOG_TAG, "HEAP=%#010lx", esp_get_free_heap_size());

    hoja_register_button_callback(button_task);
    hoja_register_analog_callback(stick_task);
    hoja_register_event_callback(event_task);
    hoja_register_rumble_callback(rumble_task);

    blu_energy_init();
    blu_energy_start_clock();
    blu_init_hardware();
    blucontrol_mode_init(true);

#ifdef CONFIG_BLUCONTROL_GC_CABLE
    gc_joybus_init(CONFIG_BLUCONTROL_GC_DATA_GPIO);
    /* Z + Start held while switching on: firmware update mode */
    gc_boot_ota_check();
    xTaskCreatePinnedToCore(gc_poll_task, "GC_POLL", 4096, NULL, tskIDLE_PRIORITY + 2, NULL, 1);
#endif
#ifdef CONFIG_BLUCONTROL_DREAMCAST
    dc_maple_init(CONFIG_BLUCONTROL_MAPLE_A_GPIO, CONFIG_BLUCONTROL_MAPLE_B_GPIO);
    /* Start + A held while switching on: firmware update mode */
    dc_boot_ota_check();
    xTaskCreatePinnedToCore(dc_poll_task, "DC_POLL", 4096, NULL, tskIDLE_PRIORITY + 2, NULL, 1);
#endif

    hoja_init();
    hoja_set_core(HOJA_CORE_NS);
    core_ns_set_subcore(HOJA_CONTROL_TYPE);

    while(hoja_start_core() != HOJA_OK)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        ESP_LOGW(LOG_TAG, "Retrying start...");
    }
    ESP_LOGI(LOG_TAG, "Started!");
}