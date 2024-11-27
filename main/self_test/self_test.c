#include <string.h>

// #include "freertos/event_groups.h"
// #include "freertos/timers.h"
// #include "driver/gpio.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "i2c_bitaxe.h"
#include "DS4432U.h"
#include "EMC2101.h"
#include "INA260.h"
#include "adc.h"
#include "global_state.h"
#include "nvs_config.h"
#include "nvs_flash.h"
#include "oled.h"
#include "vcore.h"
#include "utils.h"
#include "TPS546.h"


#define BUTTON_BOOT GPIO_NUM_0
#define LONG_PRESS_DURATION_MS 2000 // Define what constitutes a long press
#define ESP_INTR_FLAG_DEFAULT 0 //wtf is this for esp-idf?

#define TESTS_FAILED 0
#define TESTS_PASSED 1

// Define event bits
#define EVENT_SHORT_PRESS   1
#define EVENT_LONG_PRESS    2

/////Test Constants/////
//Test Fan Speed
#define FAN_SPEED_TARGET_MIN 1000 //RPM

//Test Core Voltage
#define CORE_VOLTAGE_TARGET_MIN 1000 //mV
#define CORE_VOLTAGE_TARGET_MAX 1300 //mV

//Test Power Consumption
#define POWER_CONSUMPTION_TARGET_SUB_402 12     //watts
#define POWER_CONSUMPTION_TARGET_402 5          //watts
#define POWER_CONSUMPTION_TARGET_GAMMA 11       //watts
#define POWER_CONSUMPTION_MARGIN 3              //+/- watts

static const char * TAG = "self_test";

// Create an event group
EventGroupHandle_t xSystemEventGroup;
TimerHandle_t xButtonTimer;
bool button_pressed = false;

//local function prototypes
static void tests_done(GlobalState * GLOBAL_STATE, bool test_result);
static void configure_button_boot_interrupt(void);
void vButtonTimerCallback(TimerHandle_t xTimer);

bool should_test(GlobalState * GLOBAL_STATE) {
    bool is_max = GLOBAL_STATE->asic_model == ASIC_BM1397;
    uint64_t best_diff = nvs_config_get_u64(NVS_CONFIG_BEST_DIFF, 0);
    uint16_t should_self_test = nvs_config_get_u16(NVS_CONFIG_SELF_TEST, 0);
    if (should_self_test == 1 && !is_max && best_diff < 1) {
        return true;
    }
    return false;
}

static void display_msg(char * msg, GlobalState * GLOBAL_STATE) {
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
        case DEVICE_GAMMA:
            if (OLED_status()) {
                memset(module->oled_buf, 0, 20);
                snprintf(module->oled_buf, 20, msg);
                OLED_writeString(0, 2, module->oled_buf);
            }
            break;
        default:
    }
}

static bool fan_sense_pass(GlobalState * GLOBAL_STATE)
{
    uint16_t fan_speed = 0;
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
        case DEVICE_GAMMA:
            fan_speed = EMC2101_get_fan_speed();
            break;
        default:
    }
    ESP_LOGI(TAG, "fanSpeed: %d", fan_speed);
    if (fan_speed > FAN_SPEED_TARGET_MIN) {
        return true;
    }
    return false;
}

static bool INA260_power_consumption_pass(int target_power, int margin)
{
    float power = INA260_read_power() / 1000;
    ESP_LOGI(TAG, "Power: %f", power);
    if (power > target_power -margin && power < target_power +margin) {
        return true;
    }
    return false;
}

static bool TPS546_power_consumption_pass(int target_power, int margin)
{
    float voltage = TPS546_get_vout();
    float current = TPS546_get_iout();
    float power = voltage * current;
    ESP_LOGI(TAG, "Power: %f, Voltage: %f, Current %f", power, voltage, current);
    if (power > target_power -margin && power < target_power +margin) {
        return true;
    }
    return false;
}

static bool core_voltage_pass(GlobalState * GLOBAL_STATE)
{
    uint16_t core_voltage = VCORE_get_voltage_mv(GLOBAL_STATE);
    ESP_LOGI(TAG, "Voltage: %u", core_voltage);

    if (core_voltage > CORE_VOLTAGE_TARGET_MIN && core_voltage < CORE_VOLTAGE_TARGET_MAX) {
        return true;
    }
    return false;
}



/**
 * @brief Perform a self-test of the system.
 *
 * This function is intended to be run as a task and will execute a series of 
 * diagnostic tests to ensure the system is functioning correctly.
 *
 * @param pvParameters Pointer to the parameters passed to the task (if any).
 */
void self_test(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    ESP_LOGI(TAG, "Running Self Tests");

    //create the button timer for long press detection
    xButtonTimer = xTimerCreate("ButtonTimer", pdMS_TO_TICKS(LONG_PRESS_DURATION_MS), pdFALSE, (void*)0, vButtonTimerCallback);

    configure_button_boot_interrupt();

    // Display testing
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
        case DEVICE_GAMMA:
            if (!OLED_init()) {
                ESP_LOGE(TAG, "OLED init failed!");
                tests_done(GLOBAL_STATE, TESTS_FAILED);
            } else {
                ESP_LOGI(TAG, "OLED init success!");
                // clear the oled screen
                OLED_fill(0);
                OLED_writeString(0, 0, "BITAXE SELF TESTING");
            }
            break;
        default:
    }

    GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs = malloc(sizeof(bm_job *) * 128);
    GLOBAL_STATE->valid_jobs = malloc(sizeof(uint8_t) * 128);

    for (int i = 0; i < 128; i++) {

        GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[i] = NULL;
        GLOBAL_STATE->valid_jobs[i] = 0;
    }

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
        case DEVICE_GAMMA:
            // turn ASIC on
            gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
            gpio_set_level(GPIO_NUM_10, 0);
            break;
        default:
    }

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
            EMC2101_init(nvs_config_get_u16(NVS_CONFIG_INVERT_FAN_POLARITY, 1));
            EMC2101_set_fan_speed(1);
            break;
        case DEVICE_GAMMA:
            EMC2101_init(nvs_config_get_u16(NVS_CONFIG_INVERT_FAN_POLARITY, 1));
            EMC2101_set_fan_speed(1);
            EMC2101_set_ideality_factor(EMC2101_IDEALITY_1_0319);
            EMC2101_set_beta_compensation(EMC2101_BETA_11);
            break;
        default:
    }

    uint8_t result = VCORE_init(GLOBAL_STATE);
    VCORE_set_voltage(nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE, CONFIG_ASIC_VOLTAGE) / 1000.0, GLOBAL_STATE);

    // VCore regulator testing
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
            if (GLOBAL_STATE->board_version >= 402 && GLOBAL_STATE->board_version <= 499){
                if (result != 0) {
                    ESP_LOGE(TAG, "TPS546 test failed!");
                    display_msg("TPS546:FAIL", GLOBAL_STATE);
                    tests_done(GLOBAL_STATE, TESTS_FAILED);
                }
            } else {
                if(!DS4432U_test()) {
                    ESP_LOGE(TAG, "DS4432 test failed!");
                    display_msg("DS4432U:FAIL", GLOBAL_STATE);
                    tests_done(GLOBAL_STATE, TESTS_FAILED);
                }
            }
            break;
        case DEVICE_GAMMA:
                if (result != 0) {
                    ESP_LOGE(TAG, "TPS546 test failed!");
                    display_msg("TPS546:FAIL", GLOBAL_STATE);
                    tests_done(GLOBAL_STATE, TESTS_FAILED);
                }
            break;
        default:
    }

    //initialize the INA260, if we have one.
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
            if (GLOBAL_STATE->board_version < 402) {
                // Initialize the LED controller
                INA260_init();
            }
            break;
        case DEVICE_GAMMA:
        default:
    }


    SERIAL_init();
    uint8_t chips_detected = (GLOBAL_STATE->ASIC_functions.init_fn)(GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value, GLOBAL_STATE->asic_count);
    ESP_LOGI(TAG, "%u chips detected, %u expected", chips_detected, GLOBAL_STATE->asic_count);

    if (chips_detected != GLOBAL_STATE->asic_count) {
        ESP_LOGE(TAG, "SELF TEST FAIL, %d of %d CHIPS DETECTED", chips_detected, GLOBAL_STATE->asic_count);
        char error_buf[20];
        snprintf(error_buf, 20, "ASIC:FAIL %d CHIPS", chips_detected);
        display_msg(error_buf, GLOBAL_STATE);
        tests_done(GLOBAL_STATE, TESTS_FAILED);
    }

    int baud = (*GLOBAL_STATE->ASIC_functions.set_max_baud_fn)();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    SERIAL_set_baud(baud);

    vTaskDelay(1000 / portTICK_PERIOD_MS);

    mining_notify notify_message;
    notify_message.job_id = 0;
    notify_message.prev_block_hash = "0c859545a3498373a57452fac22eb7113df2a465000543520000000000000000";
    notify_message.version = 0x20000004;
    notify_message.version_mask = 0x1fffe000;
    notify_message.target = 0x1705ae3a;
    notify_message.ntime = 0x647025b5;
    notify_message.difficulty = 1000000;

    const char * coinbase_tx = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4b0389130cfab"
                               "e6d6d5cbab26a2599e92916edec"
                               "5657a94a0708ddb970f5c45b5d12905085617eff8e010000000000000031650707758de07b010000000000001cfd703"
                               "8212f736c7573682f0000000003"
                               "79ad0c2a000000001976a9147c154ed1dc59609e3d26abb2df2ea3d587cd8c4188ac00000000000000002c6a4c29525"
                               "34b424c4f434b3ae725d3994b81"
                               "1572c1f345deb98b56b465ef8e153ecbbd27fa37bf1b005161380000000000000000266a24aa21a9ed63b06a7946b19"
                               "0a3fda1d76165b25c9b883bcc66"
                               "21b040773050ee2a1bb18f1800000000";
    uint8_t merkles[13][32];
    int num_merkles = 13;

    hex2bin("2b77d9e413e8121cd7a17ff46029591051d0922bd90b2b2a38811af1cb57a2b2", merkles[0], 32);
    hex2bin("5c8874cef00f3a233939516950e160949ef327891c9090467cead995441d22c5", merkles[1], 32);
    hex2bin("2d91ff8e19ac5fa69a40081f26c5852d366d608b04d2efe0d5b65d111d0d8074", merkles[2], 32);
    hex2bin("0ae96f609ad2264112a0b2dfb65624bedbcea3b036a59c0173394bba3a74e887", merkles[3], 32);
    hex2bin("e62172e63973d69574a82828aeb5711fc5ff97946db10fc7ec32830b24df7bde", merkles[4], 32);
    hex2bin("adb49456453aab49549a9eb46bb26787fb538e0a5f656992275194c04651ec97", merkles[5], 32);
    hex2bin("a7bc56d04d2672a8683892d6c8d376c73d250a4871fdf6f57019bcc737d6d2c2", merkles[6], 32);
    hex2bin("d94eceb8182b4f418cd071e93ec2a8993a0898d4c93bc33d9302f60dbbd0ed10", merkles[7], 32);
    hex2bin("5ad7788b8c66f8f50d332b88a80077ce10e54281ca472b4ed9bbbbcb6cf99083", merkles[8], 32);
    hex2bin("9f9d784b33df1b3ed3edb4211afc0dc1909af9758c6f8267e469f5148ed04809", merkles[9], 32);
    hex2bin("48fd17affa76b23e6fb2257df30374da839d6cb264656a82e34b350722b05123", merkles[10], 32);
    hex2bin("c4f5ab01913fc186d550c1a28f3f3e9ffaca2016b961a6a751f8cca0089df924", merkles[11], 32);
    hex2bin("cff737e1d00176dd6bbfa73071adbb370f227cfb5fba186562e4060fcec877e1", merkles[12], 32);

    char * merkle_root = calculate_merkle_root_hash(coinbase_tx, merkles, num_merkles);

    bm_job job = construct_bm_job(&notify_message, merkle_root, 0x1fffe000);

    uint8_t difficulty_mask = 8;

    (*GLOBAL_STATE->ASIC_functions.set_difficulty_mask_fn)(difficulty_mask);

    ESP_LOGI(TAG, "Sending work");

    (*GLOBAL_STATE->ASIC_functions.send_work_fn)(GLOBAL_STATE, &job);
    
     double start = esp_timer_get_time();
     double sum = 0;
     double duration = 0;
     double hash_rate = 0;

    while(duration < 3){
        task_result * asic_result = (*GLOBAL_STATE->ASIC_functions.receive_result_fn)(GLOBAL_STATE);
        if (asic_result != NULL) {
            // check the nonce difficulty
            double nonce_diff = test_nonce_value(&job, asic_result->nonce, asic_result->rolled_version);
            sum += difficulty_mask;
            duration = (double) (esp_timer_get_time() - start) / 1000000;
            hash_rate = (sum * 4294967296) / (duration * 1000000000);
            ESP_LOGI(TAG, "Nonce %lu Nonce difficulty %.32f.", asic_result->nonce, nonce_diff);
            ESP_LOGI(TAG, "%f Gh/s  , duration %f",hash_rate, duration);
        }
    }

    ESP_LOGI(TAG, "Hashrate: %f", hash_rate);

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
            break;
        case DEVICE_SUPRA:
            if(hash_rate < 500){
                display_msg("HASHRATE:FAIL", GLOBAL_STATE);
                tests_done(GLOBAL_STATE, TESTS_FAILED);
            }
            break;
        case DEVICE_GAMMA:
            if(hash_rate < 900){
                display_msg("HASHRATE:FAIL", GLOBAL_STATE);
                tests_done(GLOBAL_STATE, TESTS_FAILED);
            }
            break;
        default:
    }



    free(GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs);
    free(GLOBAL_STATE->valid_jobs);

    if (!core_voltage_pass(GLOBAL_STATE)) {
        ESP_LOGE(TAG, "SELF TEST FAIL, INCORRECT CORE VOLTAGE");
        display_msg("VCORE:FAIL", GLOBAL_STATE);
        tests_done(GLOBAL_STATE, TESTS_FAILED);
    }

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
            if(GLOBAL_STATE->board_version >= 402 && GLOBAL_STATE->board_version <= 499){
                if (!TPS546_power_consumption_pass(POWER_CONSUMPTION_TARGET_402, POWER_CONSUMPTION_MARGIN)) {
                    ESP_LOGE(TAG, "TPS546 Power Draw Failed, target %.2f", (float)POWER_CONSUMPTION_TARGET_402);
                    display_msg("POWER:FAIL", GLOBAL_STATE);
                    tests_done(GLOBAL_STATE, TESTS_FAILED);
                }
            } else {
                if (!INA260_power_consumption_pass(POWER_CONSUMPTION_TARGET_SUB_402, POWER_CONSUMPTION_MARGIN)) {
                    ESP_LOGE(TAG, "INA260 Power Draw Failed, target %.2f", (float)POWER_CONSUMPTION_TARGET_SUB_402);
                    display_msg("POWER:FAIL", GLOBAL_STATE);
                    tests_done(GLOBAL_STATE, TESTS_FAILED);
                }
            }
            break;
        case DEVICE_GAMMA:
                if (!TPS546_power_consumption_pass(POWER_CONSUMPTION_TARGET_GAMMA, POWER_CONSUMPTION_MARGIN)) {
                    ESP_LOGE(TAG, "TPS546 Power Draw Failed, target %.2f", (float)POWER_CONSUMPTION_TARGET_GAMMA);
                    display_msg("POWER:FAIL", GLOBAL_STATE);
                    tests_done(GLOBAL_STATE, TESTS_FAILED);
                }
            break;
        default:
    }

    if (!fan_sense_pass(GLOBAL_STATE)) {
        ESP_LOGE(TAG, "FAN test failed!");
        display_msg("FAN:WARN", GLOBAL_STATE);        
        tests_done(GLOBAL_STATE, TESTS_FAILED);
    }

    tests_done(GLOBAL_STATE, TESTS_PASSED);
    return;
    
}

static void tests_done(GlobalState * GLOBAL_STATE, bool test_result) {

    // Create event group for the System task
    xSystemEventGroup = xEventGroupCreate();

    if (test_result == TESTS_PASSED) {
        ESP_LOGI(TAG, "SELF TESTS PASS -- Press RESET to continue");
    } else {
        ESP_LOGI(TAG, "SELF TESTS FAIL -- Press RESET to continue");
    }
    
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
        case DEVICE_GAMMA:
            if (OLED_status()) {
                OLED_clearLine(2);
                if (test_result == TESTS_PASSED) {
                    OLED_writeString(0, 2, "TESTS PASS!");
                } else {
                    OLED_writeString(0, 2, "TESTS FAIL!");
                }
                OLED_clearLine(3);
                OLED_writeString(0, 3, "LONG PRESS BOOT");
            }
            break;
        default:
    }

    //wait here for a long press to reboot
    while (1) {

        EventBits_t uxBits = xEventGroupWaitBits(
            xSystemEventGroup,
            EVENT_LONG_PRESS,
            pdTRUE,  // Clear bits on exit
            pdFALSE, // Wait for any bit
            portMAX_DELAY //wait forever
        );

        if (uxBits & EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "Long press detected, rebooting");
            nvs_config_set_u16(NVS_CONFIG_SELF_TEST, 0);
            esp_restart();
        }

    }
}

void vButtonTimerCallback(TimerHandle_t xTimer) {
    // Timer callback, set the long press event bit
    xEventGroupSetBits(xSystemEventGroup, EVENT_LONG_PRESS);
}

// Interrupt handler for BUTTON_BOOT
void IRAM_ATTR button_boot_isr_handler(void* arg) {
    if (gpio_get_level(BUTTON_BOOT) == 0) {
        // Button pressed, start the timer
        if (!button_pressed) {
            button_pressed = true;
            xTimerStartFromISR(xButtonTimer, NULL);
        }
    } else {
        // Button released, stop the timer and check the duration
        if (button_pressed) {
            button_pressed = false;
            if (xTimerIsTimerActive(xButtonTimer)) {
                xTimerStopFromISR(xButtonTimer, NULL);
                //xEventGroupSetBitsFromISR(xSystemEventGroup, EVENT_SHORT_PRESS, NULL); //we don't care about a short press
            }
        }
    }
}

static void configure_button_boot_interrupt(void) {
    // Configure the GPIO pin as input
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,  // Interrupt on both edges
        .mode = GPIO_MODE_INPUT,         // Set as input mode
        .pin_bit_mask = (1ULL << BUTTON_BOOT),  // Bit mask of the pin to configure
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // Disable pull-down mode
        .pull_up_en = GPIO_PULLUP_ENABLE,       // Enable pull-up mode
    };
    gpio_config(&io_conf);

    // Install the ISR service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

    // Attach the interrupt handler
    gpio_isr_handler_add(BUTTON_BOOT, button_boot_isr_handler, NULL);

    ESP_LOGI(TAG, "BUTTON_BOOT interrupt configured");
}
