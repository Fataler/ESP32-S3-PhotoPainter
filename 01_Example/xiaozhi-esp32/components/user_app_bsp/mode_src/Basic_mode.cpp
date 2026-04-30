#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_log.h>
#include <esp_random.h>
#include "user_app.h"
#include "button_bsp.h"
#include "ai_app.h"
#include "list.h"
#include "ArduinoJson.h"


#define ext_wakeup_pin_1 GPIO_NUM_0
#define ext_wakeup_pin_3 GPIO_NUM_4 

static RTC_DATA_ATTR int basic_rtc_set_time = 13 * 60;// User sets the wake-up time in seconds. // The default is 60 seconds. It is awakened by a timer.
static RTC_DATA_ATTR int64_t basic_rtc_epoch = 0;
static RTC_DATA_ATTR bool basic_rtc_epoch_valid = false;
static uint8_t           Basic_sleep_arg = 0; // Parameters for low-power tasks
static SemaphoreHandle_t sleep_Semp;          // Binary call low-power task 
static uint8_t           wakeup_basic_flag = 0;
static list_t* ListHost;
static const char *BasicConfigPath = "/sdcard/06_user_foundation_img/config.txt";

static const char *get_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static list_node_t *find_node_by_name(const char *name) {
    if (name == NULL || ListHost == NULL) {
        return NULL;
    }
    for (list_node_t *node = ListHost->head; node != NULL; node = node->next) {
        CustomSDPortNode_t *sdcard_node = (CustomSDPortNode_t *)node->val;
        if (sdcard_node != NULL && strcmp(get_basename(sdcard_node->sdcard_name), name) == 0) {
            return node;
        }
    }
    return NULL;
}

static bool read_basic_config(JsonDocument &doc) {
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        return false;
    }
    int len = SDPort->SDPort_ReadOffset(BasicConfigPath, buffer, 4095, 0);
    if (len <= 0) {
        heap_caps_free(buffer);
        return false;
    }
    buffer[len] = '\0';
    DeserializationError error = deserializeJson(doc, (const char *)buffer);
    heap_caps_free(buffer);
    return !error;
}

static int parse_hhmm_minutes(const char *value, int fallback) {
    if (value == NULL || strlen(value) < 4) {
        return fallback;
    }
    int hour = 0;
    int minute = 0;
    if (sscanf(value, "%d:%d", &hour, &minute) != 2 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return fallback;
    }
    return hour * 60 + minute;
}

static bool is_night_minute(int minute, int day_start, int night_start) {
    if (day_start == night_start) {
        return false;
    }
    if (day_start < night_start) {
        return minute < day_start || minute >= night_start;
    }
    return minute >= night_start && minute < day_start;
}

static int seconds_until_minute(int current_minute, int target_minute) {
    int delta_minutes = target_minute - current_minute;
    if (delta_minutes <= 0) {
        delta_minutes += 24 * 60;
    }
    return delta_minutes * 60;
}

static void update_sleep_time_from_config(JsonDocument &doc) {
    int timer = doc["timer"] | basic_rtc_set_time;
    if (timer < 30 || timer > 86400) {
        timer = basic_rtc_set_time;
    }
    basic_rtc_set_time = timer;

    if (!doc["schedule"].is<JsonObject>()) {
        return;
    }
    JsonObject schedule = doc["schedule"].as<JsonObject>();
    if (!(schedule["enabled"] | false)) {
        return;
    }

    int64_t saved_epoch = schedule["savedEpoch"] | 0;
    if (!basic_rtc_epoch_valid && saved_epoch > 0) {
        basic_rtc_epoch = saved_epoch;
        basic_rtc_epoch_valid = true;
    }
    if (!basic_rtc_epoch_valid) {
        return;
    }

    int day_start = parse_hhmm_minutes(schedule["dayStart"] | "08:00", 8 * 60);
    int night_start = parse_hhmm_minutes(schedule["nightStart"] | "23:00", 23 * 60);
    int day_timer = schedule["dayTimer"] | timer;
    if (day_timer < 30 || day_timer > 86400) {
        day_timer = timer;
    }
    int timezone_offset = schedule["timezoneOffsetMinutes"] | 0;
    int local_minute = (int)(((basic_rtc_epoch + timezone_offset * 60) % 86400 + 86400) % 86400) / 60;

    if (is_night_minute(local_minute, day_start, night_start)) {
        basic_rtc_set_time = seconds_until_minute(local_minute, day_start);
    } else {
        basic_rtc_set_time = day_timer;
    }
}

static bool should_skip_display_for_schedule(JsonDocument &doc) {
    update_sleep_time_from_config(doc);
    if (!doc["schedule"].is<JsonObject>() || !basic_rtc_epoch_valid) {
        return false;
    }
    JsonObject schedule = doc["schedule"].as<JsonObject>();
    if (!(schedule["enabled"] | false)) {
        return false;
    }
    int day_start = parse_hhmm_minutes(schedule["dayStart"] | "08:00", 8 * 60);
    int night_start = parse_hhmm_minutes(schedule["nightStart"] | "23:00", 23 * 60);
    int timezone_offset = schedule["timezoneOffsetMinutes"] | 0;
    int local_minute = (int)(((basic_rtc_epoch + timezone_offset * 60) % 86400 + 86400) % 86400) / 60;
    return is_night_minute(local_minute, day_start, night_start);
}

static void prepare_deep_sleep_timer(void) {
    if (basic_rtc_epoch_valid) {
        basic_rtc_epoch += basic_rtc_set_time;
    }
    esp_sleep_enable_timer_wakeup((uint64_t)basic_rtc_set_time * 1000000ULL);
}

static void write_last_image_name(const char *name) {
    if (name == NULL) {
        return;
    }
    JsonDocument doc;
    read_basic_config(doc);
    doc["last"] = name;
    if (!doc["timer"].is<int>()) {
        doc["timer"] = basic_rtc_set_time;
    }

    uint8_t *buffer = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        return;
    }
    size_t len = serializeJsonPretty(doc, (char *)buffer, 4096);
    if (len > 0) {
        SDPort->SDPort_WriteFile(BasicConfigPath, buffer, len);
    }
    heap_caps_free(buffer);
}

static list_node_t *select_playlist_node(JsonDocument &doc) {
    if (ListHost == NULL || ListHost->len == 0) {
        return NULL;
    }

    const char *order = doc["order"] | "sequential";
    const char *last = doc["last"] | "";
    JsonArray playlist = doc["playlist"].as<JsonArray>();
    bool has_playlist = doc["playlist"].is<JsonArray>() && playlist.size() > 0;

    if (has_playlist) {
        int valid_count = 0;
        int last_valid_index = -1;
        for (const char *name : playlist) {
            if (name != NULL && find_node_by_name(name) != NULL) {
                if (strcmp(name, last) == 0) {
                    last_valid_index = valid_count;
                }
                valid_count++;
            }
        }

        if (valid_count > 0) {
            int target_index = 0;
            if (strcmp(order, "random") == 0) {
                target_index = esp_random() % valid_count;
                if (valid_count > 1 && target_index == last_valid_index) {
                    target_index = (target_index + 1) % valid_count;
                }
            } else if (last_valid_index >= 0) {
                target_index = (last_valid_index + 1) % valid_count;
            }

            int seen = 0;
            for (const char *name : playlist) {
                list_node_t *node = find_node_by_name(name);
                if (node != NULL) {
                    if (seen == target_index) {
                        return node;
                    }
                    seen++;
                }
            }
        }
    }

    int target_index = 0;
    int last_index = -1;
    int index = 0;
    for (list_node_t *node = ListHost->head; node != NULL; node = node->next, index++) {
        CustomSDPortNode_t *sdcard_node = (CustomSDPortNode_t *)node->val;
        if (sdcard_node != NULL && strcmp(get_basename(sdcard_node->sdcard_name), last) == 0) {
            last_index = index;
            break;
        }
    }

    if (strcmp(order, "random") == 0) {
        target_index = esp_random() % ListHost->len;
        if (ListHost->len > 1 && target_index == last_index) {
            target_index = (target_index + 1) % ListHost->len;
        }
    } else if (last_index >= 0) {
        target_index = (last_index + 1) % ListHost->len;
    }

    return list_at(ListHost, target_index);
}

static void boot_button_user_Task(void *arg) {
    uint8_t *wakeup_arg = (uint8_t *) arg;
    ePaperDisplay.EPD_Init();
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(BootButtonGroups, (0x01) | (0x02) , pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        if (get_bit_button(even, 0)) { //单击
            if (*wakeup_arg == 0) {
                if (pdTRUE == xSemaphoreTake(epaper_gui_semapHandle, 2000)) {                       
                    JsonDocument config_doc;
                    read_basic_config(config_doc);
                    if (should_skip_display_for_schedule(config_doc)) {
                        xSemaphoreGive(epaper_gui_semapHandle);
                        Basic_sleep_arg = 1;
                        xSemaphoreGive(sleep_Semp);
                        continue;
                    }
                    list_node_t *sdcard_node = select_playlist_node(config_doc);
                    if (sdcard_node != NULL) {
                        xEventGroupSetBits(Green_led_Mode_queue,set_bit_button(6));
                        Green_led_arg                   = 1;
                        CustomSDPortNode_t *sdcard_Name_node = (CustomSDPortNode_t *) sdcard_node->val;
                        ePaperDisplay.EPD_SDcardScaleIMGShakingColor(sdcard_Name_node->sdcard_name,0,0);
                        ePaperDisplay.EPD_Display();
                        write_last_image_name(get_basename(sdcard_Name_node->sdcard_name));
                        xSemaphoreGive(epaper_gui_semapHandle); 
                        Green_led_arg = 0;
                        Basic_sleep_arg = 1;
                        xSemaphoreGive(sleep_Semp);
                    }
                }
            }
        } else if(even & 0x02) { //长按 低功耗
            const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
            const uint64_t ext_wakeup_pin_3_mask = 1ULL << ext_wakeup_pin_3;
            ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(ext_wakeup_pin_1_mask | ext_wakeup_pin_3_mask, ESP_EXT1_WAKEUP_ANY_LOW)); 
            ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(ext_wakeup_pin_3));
            ESP_ERROR_CHECK(rtc_gpio_pullup_en(ext_wakeup_pin_3));
            JsonDocument config_doc;
            read_basic_config(config_doc);
            update_sleep_time_from_config(config_doc);
            prepare_deep_sleep_timer();
            //axp_basic_sleep_start();
            do {
                vTaskDelay(pdMS_TO_TICKS(50));
            } while (gpio_get_level(ext_wakeup_pin_1) == 0);
            esp_deep_sleep_start();
        }
    }
}

static void default_sleep_user_Task(void *arg) {
    uint8_t *sleep_arg = (uint8_t *) arg;
    for (;;) {
        if (pdTRUE == xSemaphoreTake(sleep_Semp, portMAX_DELAY)) {
            if (*sleep_arg == 1) {
                const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
                const uint64_t ext_wakeup_pin_3_mask = 1ULL << ext_wakeup_pin_3;
                ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(ext_wakeup_pin_1_mask | ext_wakeup_pin_3_mask,ESP_EXT1_WAKEUP_ANY_LOW)); 
                ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(ext_wakeup_pin_3));
                ESP_ERROR_CHECK(rtc_gpio_pullup_en(ext_wakeup_pin_3));
                JsonDocument config_doc;
                read_basic_config(config_doc);
                update_sleep_time_from_config(config_doc);
                prepare_deep_sleep_timer();
                //axp_basic_sleep_start(); 
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_deep_sleep_start();  
            }
        }
    }
}

static void get_wakeup_gpio(void) {
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    if (ESP_SLEEP_WAKEUP_EXT1 == wakeup_reason) {
        uint64_t wakeup_pins = esp_sleep_get_ext1_wakeup_status();
        if (wakeup_pins == 0)
            return;
        if (wakeup_pins & (1ULL << ext_wakeup_pin_1)) {
            xEventGroupSetBits(BootButtonGroups, set_bit_button(0)); 
        } else if (wakeup_pins & (1ULL << ext_wakeup_pin_3)) {
            return;
        }
    } else if (ESP_SLEEP_WAKEUP_TIMER == wakeup_reason) {
        xEventGroupSetBits(BootButtonGroups, set_bit_button(0)); 
    }
}

void User_Basic_mode_app_init(void) {
    ListHost = SDPort->SDPort_GetListHost();
    sleep_Semp  = xSemaphoreCreateBinary();
    BaseAIModel model(SDPort,decdither);
    xEventGroupSetBits(Red_led_Mode_queue, set_bit_button(0));  
    BaseAIModelConfig_t *AIModelConfig = NULL;
    AIModelConfig = model.BaseAIModel_SdcardReadAIModelConfig();
    if (AIModelConfig != NULL) {                            
        basic_rtc_set_time = AIModelConfig->time;
        ESP_LOGI("TIMER", "basic_rtc_set_time:%d", basic_rtc_set_time);
    }
    SDPort->SDPort_ScanListDir("/sdcard/06_user_foundation_img"); 
    ESP_LOGW("IMG","Values:%d",SDPort->Get_Sdcard_ImgValue());  
    xTaskCreate(boot_button_user_Task, "boot_button_user_Task", 6 * 1024, &wakeup_basic_flag, 3, NULL);
    xTaskCreate(default_sleep_user_Task, "default_sleep_user_Task", 4 * 1024, &Basic_sleep_arg, 3, NULL); 
    get_wakeup_gpio();
}
