#include "esp_timer.h"
#include "esp_bt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include "main.h"
#ifdef CONFIG_BLUCONTROL_DREAMCAST
#include "dc_maple.h"
#endif
#ifdef CONFIG_BLUCONTROL_GC_CABLE
#include "gc_joybus.h"
#endif

#define LOG_TAG "BLU_GENERIC"

#ifdef CONFIG_BLUCONTROL_DREAMCAST
/*
 * VMU copy for BlueRetro (modified BlueRetro firmware on the console side).
 * Service 7d1e0001-5b3a-4c2e-9f61-0d7c5a9e3b20:
 *  - 7d1e0002 (notify): 0x10 block off_lo off_hi data... | 0x11 code
 *    (code 0x01 start, 0x02 end, 0xE0 no VMU, 0xE1 VMU not readable)
 *  - 7d1e0003 (write):  0x01 = send the VMU copy
 * The copy itself is made at power-on (dc_vmu_snapshot), before Bluetooth.
 */
#include "esp_flash.h"
#include "freertos/queue.h"
#define VMU_SVC_UUID  "7d1e0001-5b3a-4c2e-9f61-0d7c5a9e3b20"
#define VMU_DATA_UUID "7d1e0002-5b3a-4c2e-9f61-0d7c5a9e3b20"
#define VMU_CTRL_UUID "7d1e0003-5b3a-4c2e-9f61-0d7c5a9e3b20"

static NimBLECharacteristic *s_vmu_data = NULL;
static volatile uint16_t s_vmu_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_vmu_busy = false;
static void vmu_tx_task(void *arg);

/* step 3, from BlueRetro: 0x02 block phase data[128] (a block's 4 quarters in a row),
 * 0x03 data[192] (VMU screen). Applied to the real VMU by the pad loop. */
#define VMU_WR_LEN (3 + 128)
static QueueHandle_t s_vmu_wr_q = NULL;
static portMUX_TYPE s_lcd_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_lcd_buf[192];
static volatile bool s_lcd_pending = false;

class VmuCtrlCallbacks : public NimBLECharacteristicCallbacks
{
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) override
    {
        std::string v = chr->getValue();
        if (v.size() == VMU_WR_LEN && (uint8_t)v[0] == 0x02 && s_vmu_wr_q != NULL)
        {
            /* waits if the pad loop is behind (a few blocks at most) */
            if (xQueueSend(s_vmu_wr_q, v.data(), pdMS_TO_TICKS(2000)) != pdTRUE)
            {
                ESP_LOGE(LOG_TAG, "VMU write queue full, block %u lost", (uint8_t)v[1]);
            }
            return;
        }
        if (v.size() == 1 + 192 && (uint8_t)v[0] == 0x03)
        {
            portENTER_CRITICAL(&s_lcd_mux);
            memcpy(s_lcd_buf, v.data() + 1, 192);
            s_lcd_pending = true;
            portEXIT_CRITICAL(&s_lcd_mux);
            return;
        }
        if (v.size() >= 1 && (uint8_t)v[0] == 0x01 && !s_vmu_busy)
        {
            s_vmu_busy = true;
            s_vmu_conn = connInfo.getConnHandle();
            if (xTaskCreatePinnedToCore(vmu_tx_task, "VMU_TX", 4096, NULL, tskIDLE_PRIORITY + 1, NULL, 0) != pdPASS)
            {
                s_vmu_busy = false;
            }
        }
    }
};

class BluGamepad : public BleGamepad
{
public:
    using BleGamepad::BleGamepad;
    void onStarted(NimBLEServer *pServer) override
    {
        NimBLEService *svc = pServer->createService(VMU_SVC_UUID);
        s_vmu_data = svc->createCharacteristic(VMU_DATA_UUID, NIMBLE_PROPERTY::NOTIFY);
        NimBLECharacteristic *ctrl = svc->createCharacteristic(VMU_CTRL_UUID,
                                         NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
        ctrl->setCallbacks(new VmuCtrlCallbacks());
        svc->start();
    }
};

BluGamepad bleGamepad("BluControl Gamepad", "JPZV");
#else
BleGamepad bleGamepad("BluControl Gamepad", "JPZV");
#endif
BleGamepadConfiguration bleGamepadConfig;

TaskHandle_t loopTaskHandle = NULL;

#if defined(CONFIG_BLUCONTROL_LEFT_STICK_ANALOG) || defined(CONFIG_BLUCONTROL_RIGHT_STICK_ANALOG)
blu_analog_stick_data_t *analog_stick_data;
#endif
#if defined(CONFIG_BLUCONTROL_LEFT_STICK_BUTTONS) || defined(CONFIG_BLUCONTROL_RIGHT_STICK_BUTTONS)
blu_btn_stick_data_t *button_stick_data;
#endif
#if defined(CONFIG_BLUCONTROL_LEFT_STICK_ANALOG) || defined(CONFIG_BLUCONTROL_LEFT_STICK_N64) || defined(CONFIG_BLUCONTROL_LEFT_STICK_BUTTONS)
int old_l_thumb_x_value;
int old_l_thumb_y_value;
#endif
#if defined(CONFIG_BLUCONTROL_RIGHT_STICK_ANALOG) || defined(CONFIG_BLUCONTROL_RIGHT_STICK_N64) || defined(CONFIG_BLUCONTROL_RIGHT_STICK_BUTTONS)
int old_r_thumb_x_value;
int old_r_thumb_y_value;
#endif

#if defined(CONFIG_BLUCONTROL_LEFT_TRIGGER_ANALOG) || defined(CONFIG_BLUCONTROL_RIGHT_TRIGGER_ANALOG)
int trigger_value;
#endif
#ifdef CONFIG_BLUCONTROL_LEFT_TRIGGER_ANALOG
int old_l_trigger_value;
#endif
#ifdef CONFIG_BLUCONTROL_RIGHT_TRIGGER_ANALOG
int old_r_trigger_value;
#endif

int btn_x_axis = 0;
int btn_y_axis = 0;
int8_t dpad_axis = 0;
bool need_report = false;
bool was_connected = false;
int8_t old_dpad_axis = 0;


void app_loop(void *params)
{
    while (true)
    {
        if (bleGamepad.isConnected())
        {
            need_report = false;
            #ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE
            if (blu_energy_is_clock_running())
            {
                blu_energy_stop_clock();
            }
            #endif
            
            btn_x_axis = btn_y_axis = 0;

            blu_refresh_buttons();

            if (BUTTON_A_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_A_NUMBER, blu_buttons.button_A.value);
                need_report |= (blu_buttons.button_A.old_value != blu_buttons.button_A.value);
            }
            if (BUTTON_B_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_B_NUMBER, blu_buttons.button_B.value);
                need_report |= (blu_buttons.button_B.old_value != blu_buttons.button_B.value);
            }
            if (BUTTON_Y_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_Y_NUMBER, blu_buttons.button_Y.value);
                need_report |= (blu_buttons.button_Y.old_value != blu_buttons.button_Y.value);
            }
            if (BUTTON_X_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_X_NUMBER, blu_buttons.button_X.value);
                need_report |= (blu_buttons.button_X.old_value != blu_buttons.button_X.value);
            }

            if (BUTTON_L_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_L_NUMBER, blu_buttons.trigger_l.value);
                need_report |= (blu_buttons.trigger_l.old_value != blu_buttons.trigger_l.value);
            }
            if (BUTTON_ZL_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_ZL_NUMBER, blu_buttons.trigger_zl.value);
                need_report |= (blu_buttons.trigger_zl.old_value != blu_buttons.trigger_zl.value);
            }
            if (BUTTON_R_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_R_NUMBER, blu_buttons.trigger_r.value);
                need_report |= (blu_buttons.trigger_r.old_value != blu_buttons.trigger_r.value);
            }
            if (BUTTON_ZR_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_ZR_NUMBER, blu_buttons.trigger_zr.value);
                need_report |= (blu_buttons.trigger_zr.old_value != blu_buttons.trigger_zr.value);
            }

            if (BUTTON_START_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_START_NUMBER, blu_buttons.special_start.value);
                need_report |= (blu_buttons.special_start.old_value != blu_buttons.special_start.value);
            }
            else if (BUTTON_START_NUMBER == 0)
            {
                bleGamepad.setStateSpecialButton(START_BUTTON, blu_buttons.special_start.value);
                need_report |= (blu_buttons.special_start.old_value != blu_buttons.special_start.value);
            }

            if (BUTTON_SELECT_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_SELECT_NUMBER, blu_buttons.special_select.value);
                need_report |= (blu_buttons.special_select.old_value != blu_buttons.special_select.value);
            }
            else if (BUTTON_SELECT_NUMBER == 0)
            {
                bleGamepad.setStateSpecialButton(SELECT_BUTTON, blu_buttons.special_select.value);
                need_report |= (blu_buttons.special_select.old_value != blu_buttons.special_select.value);
            }

            if (BUTTON_HOME_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_HOME_NUMBER, blu_buttons.special_home.value);
                need_report |= (blu_buttons.special_home.old_value != blu_buttons.special_home.value);
            }
            else if (BUTTON_HOME_NUMBER == 0)
            {
                bleGamepad.setStateSpecialButton(HOME_BUTTON, blu_buttons.special_home.value);
                need_report |= (blu_buttons.special_home.old_value != blu_buttons.special_home.value);
            }

            if (BUTTON_CAPTURE_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_CAPTURE_NUMBER, blu_buttons.special_capture.value);
                need_report |= (blu_buttons.special_capture.old_value != blu_buttons.special_capture.value);
            }

            if (BUTTON_STICK_L_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_STICK_L_NUMBER, blu_buttons.button_stick_left.value);
                need_report |= (blu_buttons.button_stick_left.old_value != blu_buttons.button_stick_left.value);
            }
            if (BUTTON_STICK_R_NUMBER > 0)
            {
                bleGamepad.setState(BUTTON_STICK_R_NUMBER, blu_buttons.button_stick_right.value);
                need_report |= (blu_buttons.button_stick_right.old_value != blu_buttons.button_stick_right.value);
            }

            if (blu_buttons.dpad_up.value)
                btn_y_axis++;
            if (blu_buttons.dpad_down.value)
                btn_y_axis--;
            if (blu_buttons.dpad_left.value)
                btn_x_axis--;
            if (blu_buttons.dpad_right.value)
                btn_x_axis++;

            if (btn_y_axis == 0 && btn_x_axis == 0)
            {
                dpad_axis = 0;
            }
            else
            {
                switch ((int)(atan2(btn_x_axis, btn_y_axis) * 180 / PI))
                {
                    default:
                    case 0:
                        dpad_axis = DPAD_UP;
                        break;
                    case 45:
                        dpad_axis = DPAD_UP_RIGHT;
                        break;
                    case 90:
                        dpad_axis = DPAD_RIGHT;
                        break;
                    case 135:
                        dpad_axis = DPAD_DOWN_RIGHT;
                        break;
                    case 180:
                        dpad_axis = DPAD_DOWN;
                        break;
                    case -135:
                        dpad_axis = DPAD_DOWN_LEFT;
                        break;
                    case -90:
                        dpad_axis = DPAD_LEFT;
                        break;
                    case -45:
                        dpad_axis = DPAD_UP_LEFT;
                        break;
                }
            }
            bleGamepad.setHat1(dpad_axis);
            need_report |= (old_dpad_axis != dpad_axis);
            old_dpad_axis = dpad_axis;

            // Joystick
            #if defined(CONFIG_BLUCONTROL_LEFT_STICK_ANALOG)
            analog_stick_data = blu_analog_stick_get_data(BLU_ANALOG_PAD_LEFT);
            bleGamepad.setLeftThumb(GET_ANALOG_JOYSTICK_X_AXIS(analog_stick_data->x_axis),
                                    GET_ANALOG_JOYSTICK_Y_AXIS(analog_stick_data->y_axis * -1));
            
            need_report |= (old_l_thumb_x_value != analog_stick_data->x_axis) || (old_l_thumb_y_value != analog_stick_data->y_axis);
            old_l_thumb_x_value = analog_stick_data->x_axis;
            old_l_thumb_y_value = analog_stick_data->y_axis;
            #elif defined(CONFIG_BLUCONTROL_LEFT_STICK_N64)
            bleGamepad.setLeftThumb(GET_JOYSTICK_X_AXIS(n64_left_joystick_data.x_axis.value),
                                    GET_JOYSTICK_Y_AXIS(n64_left_joystick_data.y_axis.value * -1));

            need_report |= (old_l_thumb_x_value != n64_left_joystick_data.x_axis.value) || (old_l_thumb_y_value != n64_left_joystick_data.y_axis.value);
            old_l_thumb_x_value = n64_left_joystick_data.x_axis.value;
            old_l_thumb_y_value = n64_left_joystick_data.y_axis.value;
            #elif defined(CONFIG_BLUCONTROL_LEFT_STICK_BUTTONS)
            button_stick_data = blu_buttons_stick_get_data(BLU_BUTTONS_PAD_LEFT);
            bleGamepad.setLeftThumb(GET_JOYSTICK_X_AXIS(button_stick_data->x_axis),
                                    GET_JOYSTICK_Y_AXIS(button_stick_data->y_axis * -1));
            
            need_report |= (old_l_thumb_x_value != button_stick_data->x_axis) || (old_l_thumb_y_value != button_stick_data->y_axis);
            old_l_thumb_x_value = button_stick_data->x_axis;
            old_l_thumb_y_value = button_stick_data->y_axis;
            #else
            bleGamepad.setLeftThumb(GET_JOYSTICK_X_AXIS(0),
                                    GET_JOYSTICK_Y_AXIS(0));
            #endif

            #if defined(CONFIG_BLUCONTROL_RIGHT_STICK_ANALOG)
            analog_stick_data = blu_analog_stick_get_data(BLU_ANALOG_PAD_RIGHT);
            bleGamepad.setRightThumb(GET_ANALOG_JOYSTICK_X_AXIS(analog_stick_data->x_axis),
                                     GET_ANALOG_JOYSTICK_Y_AXIS(analog_stick_data->y_axis * -1));

            need_report |= (old_r_thumb_x_value != analog_stick_data->x_axis) || (old_r_thumb_y_value != analog_stick_data->y_axis);
            old_r_thumb_x_value = analog_stick_data->x_axis;
            old_r_thumb_y_value = analog_stick_data->y_axis;
            #elif defined(CONFIG_BLUCONTROL_RIGHT_STICK_N64)
            bleGamepad.setRightThumb(GET_JOYSTICK_X_AXIS(n64_right_joystick_data.x_axis.value),
                                     GET_JOYSTICK_X_AXIS(n64_right_joystick_data.y_axis.value * -1));

            need_report |= (old_r_thumb_x_value != n64_right_joystick_data.x_axis.value) || (old_r_thumb_y_value != n64_right_joystick_data.y_axis.value);
            old_r_thumb_x_value = n64_right_joystick_data.x_axis.value;
            old_r_thumb_y_value = n64_right_joystick_data.y_axis.value;
            #elif defined(CONFIG_BLUCONTROL_RIGHT_STICK_BUTTONS)
            button_stick_data = blu_buttons_stick_get_data(BLU_BUTTONS_PAD_RIGHT);
            bleGamepad.setRightThumb(GET_JOYSTICK_X_AXIS(button_stick_data->x_axis),
                                     GET_JOYSTICK_Y_AXIS(button_stick_data->y_axis * -1));

            need_report |= (old_r_thumb_x_value != button_stick_data->x_axis) || (old_r_thumb_y_value != button_stick_data->y_axis);
            old_r_thumb_x_value = button_stick_data->x_axis;
            old_r_thumb_y_value = button_stick_data->y_axis;
            #else
            bleGamepad.setRightThumb(GET_JOYSTICK_X_AXIS(0),
                                     GET_JOYSTICK_Y_AXIS(0));
            #endif

            #ifdef CONFIG_BLUCONTROL_LEFT_TRIGGER_ANALOG
            trigger_value = GET_ANALOG_TRIGGER_AXIS(blu_analog_trigger_get_value(BLU_ANALOG_TRIGGER_LEFT));
            need_report |= (old_l_trigger_value != trigger_value);
            old_l_trigger_value = trigger_value;

            bleGamepad.setLeftTrigger(trigger_value);
            if (LEFT_TRIGGER_BTN > 0)
            {
                bleGamepad.setStateSpecialButton(LEFT_TRIGGER_BTN, CHECK_IF_TRIGGER_IS_PRESSED(trigger_value, LEFT_TRIGGER_ACT));
            }
            #else
            bleGamepad.setLeftTrigger(GET_ANALOG_TRIGGER_AXIS(0));
            #endif

            #ifdef CONFIG_BLUCONTROL_RIGHT_TRIGGER_ANALOG
            trigger_value = GET_ANALOG_TRIGGER_AXIS(blu_analog_trigger_get_value(BLU_ANALOG_TRIGGER_RIGHT));
            bleGamepad.setRightTrigger(trigger_value);
            need_report |= (old_r_trigger_value != trigger_value);
            old_r_trigger_value = trigger_value;

            if (RIGHT_TRIGGER_BTN > 0)
            {
                bleGamepad.setStateSpecialButton(RIGHT_TRIGGER_BTN, CHECK_IF_TRIGGER_IS_PRESSED(trigger_value, RIGHT_TRIGGER_ACT));
            }
            #else
            bleGamepad.setRightTrigger(GET_ANALOG_TRIGGER_AXIS(0));
            #endif

            if (need_report)
            {
                bleGamepad.sendReport();
            }
            was_connected = true;
        }
        else
        {
            if (was_connected)
            {
                /* Link lost while rumbling: never leave the motor running */
                blu_rumble_stop();
                was_connected = false;
            }
            #ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE
            if (!blu_energy_is_clock_running())
            {
                blu_energy_start_clock();
            }
            #endif
        }
    }
}

void rumble_callback(RumbleData *rumble_data)
{
    /* HID PID "Enable Actuators" values are 0..65535 (BlueRetro sends 0 or 65535
     * for a GameCube console): scale to the 8-bit PWM duty. */
    blu_rumble_set(rumble_data->left_rumble >> 8, rumble_data->right_rumble >> 8);
}

/*
 * Pairing like a PlayStation pad:
 *  - normal power-on: the controller "calls" its last host (directed advertising
 *    towards the bonded peers, newest first), then advertises normally;
 *  - X + Y held at power-on (~1 s): pairing mode. All pairings are forgotten and
 *    the controller takes a new Bluetooth identity (new address, kept in NVS), so
 *    any host sees a brand new controller and does a first pairing.
 */
static int blu_first_gpio(const char *s)
{
    if (s == NULL || s[0] == 0)
    {
        return -1;
    }
    return atoi(s);
}

static void blu_set_mode_leds(bool mode1, bool mode2)
{
#ifdef CONFIG_BLUCONTROL_LEDS_MODE_HIGH
    const int on = 1;
#else
    const int on = 0;
#endif
#if defined(CONFIG_BLUCONTROL_MODE1_LED_GPIO) && CONFIG_BLUCONTROL_MODE1_LED_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_BLUCONTROL_MODE1_LED_GPIO, mode1 ? on : !on);
#endif
#if defined(CONFIG_BLUCONTROL_MODE2_LED_GPIO) && CONFIG_BLUCONTROL_MODE2_LED_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_BLUCONTROL_MODE2_LED_GPIO, mode2 ? on : !on);
#endif
}

#if defined(CONFIG_BLUCONTROL_GC_CABLE)
static bool s_gc_pad_found = false;

/* X + Y held 1 s on the GameCube pad at power-on */
static bool blu_pairing_combo_held(void)
{
    gc_pad_state_t st;
    for (int i = 0; i < 10 && !s_gc_pad_found; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        s_gc_pad_found = gc_joybus_detect();
    }
    if (!s_gc_pad_found)
    {
        return false;
    }
    int held = 0;
    for (int i = 0; i < 100; i++)
    {
        if (gc_joybus_read(&st))
        {
            if ((st.buttons & (GC_BTN_X | GC_BTN_Y)) != (GC_BTN_X | GC_BTN_Y))
            {
                return false;
            }
            held++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return held > 50;
}
#elif defined(CONFIG_BLUCONTROL_DREAMCAST)
static bool s_dc_pad_found = false;

/* X + Y held 1 s on the Dreamcast pad at power-on */
static bool blu_pairing_combo_held(void)
{
    dc_pad_state_t st;
    /* the pad has just been powered: give it time to start */
    for (int i = 0; i < 10 && !s_dc_pad_found; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        s_dc_pad_found = dc_maple_detect();
    }
    if (!s_dc_pad_found)
    {
        return false;
    }
    int held = 0;
    for (int i = 0; i < 100; i++)
    {
        if (dc_maple_read(&st))
        {
            if ((st.buttons & (DC_BTN_X | DC_BTN_Y)) != (DC_BTN_X | DC_BTN_Y))
            {
                return false;
            }
            held++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return held > 50;
}
#else
static bool blu_pairing_combo_held(void)
{
#if defined(CONFIG_BLUCONTROL_BUTTON_X_GPIO) && defined(CONFIG_BLUCONTROL_BUTTON_Y_GPIO)
    int gx = blu_first_gpio(CONFIG_BLUCONTROL_BUTTON_X_GPIO);
    int gy = blu_first_gpio(CONFIG_BLUCONTROL_BUTTON_Y_GPIO);
    if (gx < 0 || gy < 0)
    {
        return false;
    }
    /* buttons pull the line low when pressed; must stay held for 1 s */
    for (int i = 0; i < 100; i++)
    {
        if (gpio_get_level((gpio_num_t)gx) != 0 || gpio_get_level((gpio_num_t)gy) != 0)
        {
            return false;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    return true;
#else
    return false;
#endif
}
#endif /* CONFIG_BLUCONTROL_DREAMCAST */

/* Must run before the Bluetooth stack starts */
static void blu_apply_bt_identity(bool new_identity)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_handle_t h;
    uint8_t mac[6];
    size_t len = sizeof(mac);
    bool have = false;
    if (nvs_open("blu_bt", NVS_READWRITE, &h) == ESP_OK)
    {
        if (new_identity)
        {
            esp_fill_random(mac, sizeof(mac));
            mac[0] = (mac[0] & 0xFC) | 0x02;   /* unicast, locally administered */
            if (nvs_set_blob(h, "mac", mac, sizeof(mac)) == ESP_OK)
            {
                nvs_commit(h);
            }
            have = true;
        }
        else if (nvs_get_blob(h, "mac", mac, &len) == ESP_OK && len == sizeof(mac))
        {
            have = true;
        }
        nvs_close(h);
    }
    if (have)
    {
        esp_base_mac_addr_set(mac);
    }
}

static void blu_pairing_mode_feedback(void)
{
    /* both mode LEDs blink 6 times */
    for (int i = 0; i < 6; i++)
    {
        blu_set_mode_leds(true, true);
        vTaskDelay(150 / portTICK_PERIOD_MS);
        blu_set_mode_leds(false, false);
        vTaskDelay(150 / portTICK_PERIOD_MS);
    }
    blu_set_mode_leds(false, true);   /* this app = mode 2 (BlueRetro) */
}

/* BleGamepad starts the BLE stack in its own task: wait until it is synced
 * and advertising before touching bonds or advertising. */
static bool blu_wait_ble_ready(void)
{
    for (int i = 0; i < 500; i++)   /* up to 10 s */
    {
        if (NimBLEDevice::getInitialized() && ble_hs_synced() && ble_gap_adv_active())
        {
            return true;
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    ESP_LOGE(LOG_TAG, "BLE stack not ready");
    return false;
}

/* Highest radio power for advertising and every connection (better link
 * through the controller shell and the console case). */
static void blu_set_max_tx_power(void)
{
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
    for (int t = ESP_BLE_PWR_TYPE_CONN_HDL0; t <= ESP_BLE_PWR_TYPE_CONN_HDL8; t++)
    {
        esp_ble_tx_power_set((esp_ble_power_type_t)t, ESP_PWR_LVL_P9);
    }
}

/* Wait up to ms for a connection, polling often so we react at once */
static bool blu_wait_connected(int ms)
{
    for (int t = 0; t < ms; t += 20)
    {
        if (bleGamepad.isConnected())
        {
            return true;
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    return bleGamepad.isConnected();
}

/*
 * Runs for the whole session. Whenever the controller is not connected (power-on,
 * console turned on later, link lost while playing...), it calls its last host
 * with directed advertising, like a PlayStation pad pages its console:
 *  - the newest bond is called every round, older ones every 4th round;
 *  - between calls, a short normal advertising window lets a new host pair;
 *  - after 1 minute without host, it slows down to save battery.
 */
static void blu_ble_link_task(void *arg)
{
    bool pairing_mode = (arg != NULL);
    if (!blu_wait_ble_ready())
    {
        vTaskDelete(NULL);
    }
    blu_set_max_tx_power();

    if (pairing_mode)
    {
        ESP_LOGW(LOG_TAG, "X + Y held: pairing mode, new Bluetooth identity");
        NimBLEDevice::deleteAllBonds();
        blu_pairing_mode_feedback();
    }

    uint32_t round = 0;
    int64_t lost_since = esp_timer_get_time();
    while (true)
    {
        if (bleGamepad.isConnected())
        {
            round = 0;
            lost_since = esp_timer_get_time();
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
        int n = 0;
        if (ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS) != 0 || n == 0)
        {
            /* never paired: normal advertising only (restarted by the server) */
            if (!ble_gap_adv_active())
            {
                NimBLEDevice::startAdvertising();
            }
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        int oldest = (round % 4 == 3) ? 0 : n - 1;
        for (int i = n - 1; i >= oldest && !bleGamepad.isConnected(); i--)
        {
            if (NimBLEDevice::startDirectedAdvertising(&peers[i], 1280))
            {
                blu_wait_connected(1300);
            }
        }
        if (!bleGamepad.isConnected())
        {
            if (!ble_gap_adv_active())
            {
                NimBLEDevice::startAdvertising();
            }
            bool slow = (esp_timer_get_time() - lost_since) > 60LL * 1000000LL;
            blu_wait_connected(slow ? 2000 : 250);
        }
        round++;
    }
}

#ifdef CONFIG_BLUCONTROL_GC_CABLE
/* 0..255 from the pad -> 0..4080 for the BLE report */
#define GC_AXIS(v) ((int)(v) * (BLU_JOYSTICK_ABS_MAX * 2) / 255)

static void gc_set_button(int number, bool pressed)
{
    if (number > 0)
    {
        bleGamepad.setState(number, pressed);
    }
}

void gc_app_loop(void *params)
{
    gc_pad_state_t st;
    memset(&st, 0, sizeof(st));
    gc_pad_state_t old;
    memset(&old, 0xFF, sizeof(old));
    int fails = 0;
    bool force = true;

    while (true)
    {
        if (!s_gc_pad_found)
        {
            s_gc_pad_found = gc_joybus_detect();
            if (!s_gc_pad_found)
            {
                static bool blink = false;
                blink = !blink;
                blu_set_mode_leds(false, blink);
                gc_no_pad_fallback();
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            blu_set_mode_leds(false, true);
            force = true;
        }

        if (!gc_joybus_read(&st))
        {
            if (++fails > 50)
            {
                ESP_LOGW(LOG_TAG, "Pad lost (%lu errors)", (unsigned long)gc_joybus_errors());
                s_gc_pad_found = false;
                fails = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }
        fails = 0;
        uint16_t btn = st.buttons;
        gc_mode_switch_check(btn);

        if (bleGamepad.isConnected())
        {
            if (!was_connected)
            {
                force = true;
            }
            if (force || memcmp(&st, &old, sizeof(st)) != 0)
            {
                gc_set_button(BUTTON_A_NUMBER, btn & GC_BTN_A);
                gc_set_button(BUTTON_B_NUMBER, btn & GC_BTN_B);
                gc_set_button(BUTTON_X_NUMBER, btn & GC_BTN_X);
                gc_set_button(BUTTON_Y_NUMBER, btn & GC_BTN_Y);
                gc_set_button(BUTTON_L_NUMBER, btn & GC_BTN_L);
                gc_set_button(BUTTON_R_NUMBER, btn & GC_BTN_R);
                gc_set_button(BUTTON_ZR_NUMBER, btn & GC_BTN_Z);
                gc_set_button(BUTTON_START_NUMBER, btn & GC_BTN_START);

                int x = 0, y = 0;
                if (btn & GC_BTN_UP) y++;
                if (btn & GC_BTN_DOWN) y--;
                if (btn & GC_BTN_LEFT) x--;
                if (btn & GC_BTN_RIGHT) x++;
                int8_t hat = 0;
                if (x == 0 && y > 0) hat = DPAD_UP;
                else if (x > 0 && y > 0) hat = DPAD_UP_RIGHT;
                else if (x > 0 && y == 0) hat = DPAD_RIGHT;
                else if (x > 0 && y < 0) hat = DPAD_DOWN_RIGHT;
                else if (x == 0 && y < 0) hat = DPAD_DOWN;
                else if (x < 0 && y < 0) hat = DPAD_DOWN_LEFT;
                else if (x < 0 && y == 0) hat = DPAD_LEFT;
                else if (x < 0 && y > 0) hat = DPAD_UP_LEFT;
                bleGamepad.setHat1(hat);

                /* GameCube: up = high; HID: up = low */
                bleGamepad.setLeftThumb(GC_AXIS(st.sx), GC_AXIS(255 - st.sy));
                bleGamepad.setRightThumb(GC_AXIS(st.cx), GC_AXIS(255 - st.cy));
                bleGamepad.setLeftTrigger(GC_AXIS(st.l));
                bleGamepad.setRightTrigger(GC_AXIS(st.r));
                bleGamepad.sendReport();

                old = st;
                force = false;
            }
            was_connected = true;
        }
        else
        {
            was_connected = false;
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}
#endif /* CONFIG_BLUCONTROL_GC_CABLE */

#ifdef CONFIG_BLUCONTROL_DREAMCAST
/* 0..255 from the pad -> 0..4080 for the BLE report */
#define DC_AXIS(v) ((int)(v) * (BLU_JOYSTICK_ABS_MAX * 2) / 255)

static void dc_set_button(int number, bool pressed)
{
    if (number > 0)
    {
        bleGamepad.setState(number, pressed);
    }
}

/* Sends one notification; waits while the BLE buffers are full */
static bool vmu_notify(const uint8_t *data, size_t len)
{
    const uint16_t h = s_vmu_data->getHandle();
    for (int tries = 0; tries < 2000; tries++)          /* up to ~4 s */
    {
        if (!bleGamepad.isConnected())
        {
            return false;
        }
        if (os_msys_num_free() >= 8)                    /* keep room for the pad reports */
        {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
            if (om != NULL)
            {
                int rc = ble_gattc_notify_custom(s_vmu_conn, h, om);   /* consumes om */
                if (rc == 0)
                {
                    return true;
                }
                if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY)
                {
                    ESP_LOGW(LOG_TAG, "VMU notify error %d", rc);
                    return false;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static void vmu_tx_task(void *arg)
{
    uint8_t st[2] = { 0x11, 0x00 };
    int state = dc_vmu_state();
    uint16_t mtu = ble_att_mtu(s_vmu_conn);
    int chunk = (mtu > 3 + 4) ? (mtu - 3 - 4) : 0;
    if (chunk > 240) chunk = 240;
    chunk &= ~3;
    ESP_LOGI(LOG_TAG, "VMU copy requested (state %d, MTU %u, chunk %d)", state, mtu, chunk);

    if (state != DC_VMU_OK || chunk < 16)
    {
        st[1] = (state == DC_VMU_NONE) ? 0xE0 : 0xE1;
        vmu_notify(st, sizeof(st));
    }
    else
    {
        static uint8_t blk[512];
        static uint8_t pkt[4 + 240];
        int64_t t0 = esp_timer_get_time();
        bool ok = true;
        st[1] = 0x01;
        ok = vmu_notify(st, sizeof(st));
        for (int b = 0; b < 256 && ok; b++)
        {
            if (!dc_vmu_copy_read((uint8_t)b, blk))
            {
                ok = false;
                break;
            }
            for (int off = 0; off < 512 && ok; off += chunk)
            {
                int n = (512 - off < chunk) ? (512 - off) : chunk;
                pkt[0] = 0x10;
                pkt[1] = (uint8_t)b;
                pkt[2] = (uint8_t)(off & 0xFF);
                pkt[3] = (uint8_t)(off >> 8);
                memcpy(&pkt[4], &blk[off], n);
                ok = vmu_notify(pkt, 4 + n);
            }
        }
        st[1] = ok ? 0x02 : 0xE1;
        vmu_notify(st, sizeof(st));
        ESP_LOGI(LOG_TAG, "VMU copy sent: %s in %lld ms", ok ? "OK" : "FAILED",
                 (esp_timer_get_time() - t0) / 1000);
        /* closed pad, no log: 3 short blue blinks = copy received by BlueRetro,
         * red/blue flicker = transfer failed (BlueRetro keeps its own VMU) */
        for (int i = 0; i < (ok ? 3 : 8); i++)
        {
            blu_set_mode_leds(!ok, false);
            vTaskDelay(pdMS_TO_TICKS(150));
            blu_set_mode_leds(false, true);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    s_vmu_busy = false;
    vTaskDelete(NULL);
}

/* Applies what BlueRetro sent: saves to the real VMU, then the screen.
 * Red LED on while a save is written to the VMU. */
static void dc_vmu_apply(void)
{
    static uint8_t item[VMU_WR_LEN];
    static uint8_t blk[512];
    static int cur = -1;
    static uint8_t mask = 0;

    while (s_vmu_wr_q != NULL && xQueueReceive(s_vmu_wr_q, item, 0) == pdTRUE)
    {
        uint8_t block = item[1], phase = item[2] & 3;
        if (block != cur)
        {
            if (cur >= 0 && mask != 0x0F)
            {
                ESP_LOGW(LOG_TAG, "VMU block %d incomplete (0x%x), dropped", cur, mask);
            }
            cur = block;
            mask = 0;
        }
        memcpy(&blk[phase * 128], &item[3], 128);
        mask |= (uint8_t)(1 << phase);
        if (mask == 0x0F)
        {
            blu_set_mode_leds(true, false);
            bool ok = dc_maple_vmu_write_block(block, blk);
            if (ok)
            {
                dc_vmu_copy_update(block, blk);
            }
            else
            {
                /* failure: fast red/blue flicker */
                for (int i = 0; i < 5; i++)
                {
                    blu_set_mode_leds(true, false);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    blu_set_mode_leds(false, true);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            blu_set_mode_leds(false, true);
            cur = -1;
            mask = 0;
            break;      /* one block per loop turn: the pad stays responsive */
        }
    }

    if (s_lcd_pending)
    {
        static uint8_t lcd[192];
        portENTER_CRITICAL(&s_lcd_mux);
        memcpy(lcd, s_lcd_buf, 192);
        s_lcd_pending = false;
        portEXIT_CRITICAL(&s_lcd_mux);
        dc_maple_vmu_lcd_write(lcd);
    }
}

void dc_app_loop(void *params)
{
    dc_pad_state_t st;
    memset(&st, 0, sizeof(st));
    dc_pad_state_t old;
    memset(&old, 0xFF, sizeof(old));
    int fails = 0;
    bool force = true;

    while (true)
    {
        if (!s_dc_pad_found)
        {
            s_dc_pad_found = dc_maple_detect();
            if (!s_dc_pad_found)
            {
                /* no pad: blue LED blinks; after 20 s from power-on -> Wi-Fi update mode */
                static bool blink = false;
                blink = !blink;
                blu_set_mode_leds(false, blink);
                dc_no_pad_fallback();
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            blu_set_mode_leds(false, true);
            force = true;
        }

        if (!dc_maple_read(&st))
        {
            if (++fails > 50)
            {
                ESP_LOGW(LOG_TAG, "Pad lost (%lu errors)", (unsigned long)dc_maple_errors());
                s_dc_pad_found = false;
                fails = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }
        fails = 0;

        /* saves and screen from BlueRetro */
        dc_vmu_apply();

        uint16_t btn = st.buttons;
        /* Start + A held 5 s: go to Switch mode */
        dc_mode_switch_check(btn);

        if (bleGamepad.isConnected())
        {
            #ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE
            if (blu_energy_is_clock_running())
            {
                blu_energy_stop_clock();
            }
            #endif
            if (!was_connected)
            {
                force = true;
            }
            if (force || btn != old.buttons || st.ltrig != old.ltrig || st.rtrig != old.rtrig ||
                st.joy_x != old.joy_x || st.joy_y != old.joy_y)
            {
                dc_set_button(BUTTON_A_NUMBER, btn & DC_BTN_A);
                dc_set_button(BUTTON_B_NUMBER, btn & DC_BTN_B);
                dc_set_button(BUTTON_X_NUMBER, btn & DC_BTN_X);
                dc_set_button(BUTTON_Y_NUMBER, btn & DC_BTN_Y);
                dc_set_button(BUTTON_START_NUMBER, btn & DC_BTN_START);

                int x = 0, y = 0;
                if (btn & DC_BTN_UP) y++;
                if (btn & DC_BTN_DOWN) y--;
                if (btn & DC_BTN_LEFT) x--;
                if (btn & DC_BTN_RIGHT) x++;
                int8_t hat = 0;
                if (x == 0 && y > 0) hat = DPAD_UP;
                else if (x > 0 && y > 0) hat = DPAD_UP_RIGHT;
                else if (x > 0 && y == 0) hat = DPAD_RIGHT;
                else if (x > 0 && y < 0) hat = DPAD_DOWN_RIGHT;
                else if (x == 0 && y < 0) hat = DPAD_DOWN;
                else if (x < 0 && y < 0) hat = DPAD_DOWN_LEFT;
                else if (x < 0 && y == 0) hat = DPAD_LEFT;
                else if (x < 0 && y > 0) hat = DPAD_UP_LEFT;
                bleGamepad.setHat1(hat);

                bleGamepad.setLeftThumb(DC_AXIS(st.joy_x), DC_AXIS(st.joy_y));
                bleGamepad.setRightThumb(BLU_JOYSTICK_ABS_MAX, BLU_JOYSTICK_ABS_MAX);
                bleGamepad.setLeftTrigger(DC_AXIS(st.ltrig));
                bleGamepad.setRightTrigger(DC_AXIS(st.rtrig));
                bleGamepad.sendReport();

                old = st;
                old.buttons = btn;
                force = false;
            }
            was_connected = true;
        }
        else
        {
            was_connected = false;
            #ifdef CONFIG_BLUCONTROL_ENERGY_MODE_SOFTWARE
            if (!blu_energy_is_clock_running())
            {
                blu_energy_start_clock();
            }
            #endif
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}
#endif /* CONFIG_BLUCONTROL_DREAMCAST */

extern "C" void app_main(void)
{
    ESP_LOGD(LOG_TAG, "HEAP=%#010lx", esp_get_free_heap_size());

    blu_energy_init();
    blu_init_hardware();
#ifdef CONFIG_BLUCONTROL_GC_CABLE
    gc_joybus_init(CONFIG_BLUCONTROL_GC_DATA_GPIO);
    /* Z + Start held while switching on: firmware update mode */
    gc_boot_ota_check();
#endif
#ifdef CONFIG_BLUCONTROL_DREAMCAST
    dc_maple_init(CONFIG_BLUCONTROL_MAPLE_A_GPIO, CONFIG_BLUCONTROL_MAPLE_B_GPIO);
    /* Start + A held while switching on: firmware update mode */
    dc_boot_ota_check();
#endif
    blucontrol_mode_init(true);
#ifdef CONFIG_BLUCONTROL_DREAMCAST
    /* copy of the VMU (slot 1) for BlueRetro, before the radio starts:
     * both LEDs on while copying, fast red/blue flicker if the VMU is not readable */
    blu_set_mode_leds(true, true);
    if (dc_vmu_snapshot() == DC_VMU_ERROR)
    {
        for (int i = 0; i < 10; i++)
        {
            blu_set_mode_leds(true, false);
            vTaskDelay(pdMS_TO_TICKS(100));
            blu_set_mode_leds(false, true);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    blu_set_mode_leds(false, true);
    s_vmu_wr_q = xQueueCreate(64, VMU_WR_LEN);
#endif

    bleGamepadConfig.setAutoReport(false);

    if (BUTTONS_LENGTH > 0)
    {
        bleGamepadConfig.setButtonCount(BUTTONS_LENGTH);
    }
    else
    {
        int allButtons[] =
        {
            BUTTON_A_NUMBER,
            BUTTON_B_NUMBER,
            BUTTON_Y_NUMBER,
            BUTTON_X_NUMBER,
            BUTTON_L_NUMBER,
            BUTTON_ZL_NUMBER,
            BUTTON_R_NUMBER,
            BUTTON_ZR_NUMBER,
            BUTTON_START_NUMBER,
            BUTTON_HOME_NUMBER,
            BUTTON_CAPTURE_NUMBER,
            BUTTON_STICK_L_NUMBER,
            BUTTON_STICK_R_NUMBER,
            LEFT_TRIGGER_BTN,
            RIGHT_TRIGGER_BTN
        };
        int maxNumber = 1;
        for (int i = 0; i < 13; i++)
        {
            if (allButtons[i] > maxNumber)
            {
                maxNumber = allButtons[i];
            }
        }
        bleGamepadConfig.setButtonCount(maxNumber);
    }

    ESP_LOGD(LOG_TAG, "Buttons length: %d", bleGamepadConfig.getButtonCount());

    bleGamepadConfig.setAxesMin(0x0000);
    bleGamepadConfig.setAxesMax(BLU_JOYSTICK_ABS_MAX * 2);
    bleGamepadConfig.setWhichSpecialButtons(BUTTON_START_NUMBER <= 0, BUTTON_CAPTURE_NUMBER <= 0, false, BUTTON_HOME_NUMBER <= 0, false, false, false, false);
#if defined(CONFIG_BLUCONTROL_GC_CABLE)
    /* both sticks + both analog triggers */
    bleGamepadConfig.setWhichAxes(true, true, true, true, true, true, false, false);
#elif defined(CONFIG_BLUCONTROL_DREAMCAST)
    /* stick X/Y + both analog triggers. Z/Rz (an unused, centred right stick)
     * must be present: BlueRetro reads Rx/Ry as triggers only when Z follows
     * X/Y in the report, otherwise Rx/Ry become a right stick (same layout
     * as the GameCube pads, whose triggers work). */
    bleGamepadConfig.setWhichAxes(true, true, true, true, true, true, false, false);
#else
    bleGamepadConfig.setWhichAxes(AXIS_HAS_LEFT_STICK, AXIS_HAS_LEFT_STICK, AXIS_HAS_RIGHT_STICK, AXIS_HAS_LEFT_TRIGGER, AXIS_HAS_RIGHT_TRIGGER, AXIS_HAS_RIGHT_STICK, false, false);
#endif
    
    #if LEFT_RUMBLE_GPIO >= 0 || RIGHT_RUMBLE_GPIO >= 0 || defined(CONFIG_BLUCONTROL_GC_CABLE)
    bleGamepadConfig.setHasRumble(true);
    bleGamepadConfig.setRumbleCallBack(rumble_callback);
    #else
    bleGamepadConfig.setHasRumble(false);
    #endif
    
    bool pairing_mode = blu_pairing_combo_held();
    blu_apply_bt_identity(pairing_mode);

    bleGamepad.begin(&bleGamepadConfig);

    xTaskCreatePinnedToCore(blu_ble_link_task, "BLE_LINK", 4096, pairing_mode ? (void *)1 : NULL, tskIDLE_PRIORITY + 1, NULL, 0);

#if defined(CONFIG_BLUCONTROL_GC_CABLE)
    xTaskCreatePinnedToCore(gc_app_loop, "GC_LOOP", 4096, NULL, tskIDLE_PRIORITY + 2, &loopTaskHandle, 1);
#elif defined(CONFIG_BLUCONTROL_DREAMCAST)
    xTaskCreatePinnedToCore(dc_app_loop, "DC_LOOP", 4096, NULL, tskIDLE_PRIORITY + 2, &loopTaskHandle, 1);
#else
    xTaskCreatePinnedToCore(app_loop, "APP_LOOP", 4096, NULL, tskIDLE_PRIORITY, &loopTaskHandle, 1);
#endif
    configASSERT(loopTaskHandle);

    ESP_LOGI(LOG_TAG, "Started!");
}