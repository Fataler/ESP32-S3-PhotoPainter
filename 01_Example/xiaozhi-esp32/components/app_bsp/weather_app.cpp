#include <stdio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "ArduinoJson.h"
#include "weather_app.h"

struct SpiRamAllocator : ArduinoJson::Allocator {
    void *allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }

    void deallocate(void *pointer) override {
        heap_caps_free(pointer);
    }

    void *reallocate(void *ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }
};

SpiRamAllocator allocator;
JsonDocument    doc(&allocator);

static bool copy_json_string(char *dst, size_t dst_size, const char *value, const char *field) {
    if (dst == NULL || dst_size == 0) {
        return false;
    }
    if (value == NULL) {
        ESP_LOGE("WeatherPort", "Missing JSON field: %s", field);
        dst[0] = '\0';
        return false;
    }
    snprintf(dst, dst_size, "%s", value);
    return true;
}

WeatherPort::WeatherPort() {
    WeatherData = (WeatherData_t *) heap_caps_malloc(sizeof(WeatherData_t), MALLOC_CAP_SPIRAM);
    assert(WeatherData);
}

WeatherPort::~WeatherPort() {

}

void WeatherPort::WeatherPort_log(void) {
    ESP_LOGI(TAG, "Timer:%s", WeatherData->calendar);

    ESP_LOGI(TAG, "Today's Date:%s", WeatherData->td_weather);
    ESP_LOGI(TAG, "Today's Temperature:%s", WeatherData->td_Temp);
    ESP_LOGI(TAG, "Today's Wind Direction:%s", WeatherData->td_fx);
    ESP_LOGI(TAG, "Today's Weekday:%s", WeatherData->td_week);
    ESP_LOGI(TAG, "Today's Humidity:%s", WeatherData->td_RH);
    ESP_LOGI(TAG, "Today's Type:%s", WeatherData->td_type);

    ESP_LOGI(TAG, "Tomorrow's Date:%s", WeatherData->tmr_weather);
    ESP_LOGI(TAG, "Tomorrow's Temperature:%s", WeatherData->tmr_Temp);
    ESP_LOGI(TAG, "Tomorrow's Wind Direction:%s", WeatherData->tmr_fx);
    ESP_LOGI(TAG, "Tomorrow's Weekday:%s", WeatherData->tmr_week);
    ESP_LOGI(TAG, "Tomorrow's Humidity:%s", WeatherData->tmr_RH);
    ESP_LOGI(TAG, "Tomorrow's Type:%s", WeatherData->tmr_type);

    ESP_LOGI(TAG, "Day After Tomorrow's Date:%s", WeatherData->tdat_weather);
    ESP_LOGI(TAG, "Day After Tomorrow's Temperature:%s", WeatherData->tdat_Temp);
    ESP_LOGI(TAG, "Day After Tomorrow's Wind Direction:%s", WeatherData->tdat_fx);
    ESP_LOGI(TAG, "Day After Tomorrow's Weekday:%s", WeatherData->tdat_week);
    ESP_LOGI(TAG, "Day After Tomorrow's Humidity:%s", WeatherData->tdat_RH);
    ESP_LOGI(TAG, "Day After Tomorrow's Type:%s", WeatherData->tdat_type);

    ESP_LOGI(TAG, "Three Days Later Date:%s", WeatherData->stdat_weather);
    ESP_LOGI(TAG, "Three Days Later Temperature:%s", WeatherData->stdat_Temp);
    ESP_LOGI(TAG, "Three Days Later Wind Direction:%s", WeatherData->stdat_fx);
    ESP_LOGI(TAG, "Three Days Later Weekday:%s", WeatherData->stdat_week);
    ESP_LOGI(TAG, "Three Days Later Humidity:%s", WeatherData->stdat_RH);
    ESP_LOGI(TAG, "Three Days Later Type:%s", WeatherData->stdat_type);
}

WeatherData_t* WeatherPort::WeatherPort_DecodingSring(const char *jsonstr) {
    DeserializationError error = deserializeJson(doc, jsonstr);
    heap_caps_free((void *) jsonstr);
    jsonstr = NULL;
    if (error) {
        ESP_LOGE(TAG, "Analysis failed");
        return NULL;
    }
    int         wendu_high = 0;
    int         wendu_low  = 0;
    int         month      = 0;
    int         day        = 0;
    int         Numshidu   = 0;
    const char *str        = doc["time"];
    if (str == NULL || !doc["data"]["forecast"].is<JsonArray>() || doc["data"]["forecast"].as<JsonArray>().size() < 4) {
        ESP_LOGE(TAG, "Weather JSON does not contain required fields");
        return NULL;
    }

    int s_year;
  	int s_month;
  	int s_day;
  	int s_hour;
  	int s_minute;
  	int s_second;
    sscanf(str, "%32[^ ]", WeatherData->calendar); /* Time retrieval - accurate to the day */
    
    sscanf(str, "%d-%d-%d %d:%d:%d", &s_year, &s_month,&s_day, &s_hour,&s_minute, &s_second);
    snprintf(WeatherData->td_weather, 19, "%02d-%02d", s_month, s_day);

    str = doc["data"]["forecast"][0]["high"] | "";
    sscanf(str, "%*[^0-9]%d", &wendu_high);
    str = doc["data"]["forecast"][0]["low"] | "";
    sscanf(str, "%*[^0-9]%d", &wendu_low);
    snprintf(WeatherData->td_Temp, 30, "%02d-%02d℃", wendu_low, wendu_high);

    str = doc["data"]["forecast"][0]["fx"];
    if (!copy_json_string(WeatherData->td_fx, sizeof(WeatherData->td_fx), str, "forecast[0].fx")) return NULL;

    str = doc["data"]["forecast"][0]["week"];
    if (!copy_json_string(WeatherData->td_week, sizeof(WeatherData->td_week), str, "forecast[0].week")) return NULL;

    str = doc["data"]["shidu"];
    if (str == NULL) return NULL;
    sscanf(str, "%d", &Numshidu);
    snprintf(WeatherData->td_RH, sizeof(WeatherData->td_RH), "%s", str);

    str = doc["data"]["forecast"][0]["type"];
    if (!copy_json_string(WeatherData->td_type, sizeof(WeatherData->td_type), str, "forecast[0].type")) return NULL;

    WeatherData->td_aqi = doc["data"]["forecast"][0]["aqi"];
    /*Tomorrow's Weather*/
    str = doc["data"]["forecast"][1]["ymd"] | "";
    sscanf(str, "%*[^-]-%d-%d", &month, &day);
    snprintf(WeatherData->tmr_weather, 19, "%02d-%02d", month, day);

    str = doc["data"]["forecast"][1]["high"] | "";
    sscanf(str, "%*[^0-9]%d", &wendu_high);
    str = doc["data"]["forecast"][1]["low"] | "";
    sscanf(str, "%*[^0-9]%d", &wendu_low);
    snprintf(WeatherData->tmr_Temp, 30, "%02d-%02d℃", wendu_low, wendu_high);

    str = doc["data"]["forecast"][1]["fx"];
    if (!copy_json_string(WeatherData->tmr_fx, sizeof(WeatherData->tmr_fx), str, "forecast[1].fx")) return NULL;

    str = doc["data"]["forecast"][1]["week"];
    if (!copy_json_string(WeatherData->tmr_week, sizeof(WeatherData->tmr_week), str, "forecast[1].week")) return NULL;

    snprintf(WeatherData->tmr_RH, 14, "%d%%", Numshidu + 1);

    str = doc["data"]["forecast"][1]["type"];
    if (!copy_json_string(WeatherData->tmr_type, sizeof(WeatherData->tmr_type), str, "forecast[1].type")) return NULL;

    WeatherData->tmr_aqi = doc["data"]["forecast"][1]["aqi"];
    /*Weather Forecast for the Day After Tomorrow*/
    str = doc["data"]["forecast"][2]["ymd"] | "";
    sscanf(str, "%*[^-]-%d-%d", &month, &day);
    snprintf(WeatherData->tdat_weather, 19, "%02d-%02d", month, day);

    str = doc["data"]["forecast"][2]["high"] | "";
    sscanf(str, "%*[^0-9]%d", &wendu_high);
    str = doc["data"]["forecast"][2]["low"] | "";
    sscanf(str, "%*[^0-9]%d", &wendu_low);
    snprintf(WeatherData->tdat_Temp, 30, "%02d-%02d℃", wendu_low, wendu_high);

    str = doc["data"]["forecast"][2]["fx"];
    if (!copy_json_string(WeatherData->tdat_fx, sizeof(WeatherData->tdat_fx), str, "forecast[2].fx")) return NULL;

    str = doc["data"]["forecast"][2]["week"];
    if (!copy_json_string(WeatherData->tdat_week, sizeof(WeatherData->tdat_week), str, "forecast[2].week")) return NULL;

    snprintf(WeatherData->tdat_RH, 14, "%d%%", Numshidu - 1);

    str = doc["data"]["forecast"][2]["type"];
    if (!copy_json_string(WeatherData->tdat_type, sizeof(WeatherData->tdat_type), str, "forecast[2].type")) return NULL;

    WeatherData->tdat_aqi = doc["data"]["forecast"][2]["aqi"];

    /*The Weather the Day After Tomorrow*/
    str = doc["data"]["forecast"][3]["ymd"] | "";
    sscanf(str, "%*[^-]-%d-%d", &month, &day);
    snprintf(WeatherData->stdat_weather, 19, "%02d-%02d", month, day);

    str = doc["data"]["forecast"][3]["high"] | "";
    sscanf(str, "%*[^0-9]%d", &wendu_high);
    str = doc["data"]["forecast"][3]["low"] | "";
    sscanf(str, "%*[^0-9]%d", &wendu_low);
    snprintf(WeatherData->stdat_Temp, 30, "%02d-%02d℃", wendu_low, wendu_high);

    str = doc["data"]["forecast"][3]["fx"];
    if (!copy_json_string(WeatherData->stdat_fx, sizeof(WeatherData->stdat_fx), str, "forecast[3].fx")) return NULL;

    str = doc["data"]["forecast"][3]["week"];
    if (!copy_json_string(WeatherData->stdat_week, sizeof(WeatherData->stdat_week), str, "forecast[3].week")) return NULL;

    snprintf(WeatherData->stdat_RH, 14, "%d%%", Numshidu - 2);

    str = doc["data"]["forecast"][3]["type"];
    if (!copy_json_string(WeatherData->stdat_type, sizeof(WeatherData->stdat_type), str, "forecast[3].type")) return NULL;

    WeatherData->stdat_aqi = doc["data"]["forecast"][3]["aqi"];


    WeatherPort_log();

    return WeatherData;
}

char* WeatherPort::WeatherPort_GetSdCardImageName(const char *instr) {
    if (instr == NULL) {
        snprintf(DirectoryImgName, sizeof(DirectoryImgName), "%s", "/sdcard/01_sys_init_img/08_yin.bmp");
        return DirectoryImgName;
    }
    if (!strcmp("大雨", instr)) {
        snprintf(DirectoryImgName, sizeof(DirectoryImgName), "%s", "/sdcard/01_sys_init_img/01_dayu.bmp");
    } else if (!strcmp("多云", instr)) {
        snprintf(DirectoryImgName, sizeof(DirectoryImgName), "%s", "/sdcard/01_sys_init_img/02_duoyun.bmp");
    } else if (!strcmp("雷雨", instr)) {
        snprintf(DirectoryImgName, sizeof(DirectoryImgName), "%s", "/sdcard/01_sys_init_img/03_leiyu.bmp");
    } else if (!strcmp("晴", instr)) {
        snprintf(DirectoryImgName, sizeof(DirectoryImgName), "%s", "/sdcard/01_sys_init_img/04_qin.bmp");
    } else if (!strcmp("小雨", instr)) {
        snprintf(DirectoryImgName, sizeof(DirectoryImgName), "%s", "/sdcard/01_sys_init_img/05_xiaoyu.bmp");
    } else if (!strcmp("下雪", instr)) {
        snprintf(DirectoryImgName, sizeof(DirectoryImgName), "%s", "/sdcard/01_sys_init_img/06_xiaxue.bmp");
    } else if (!strcmp("中雨", instr)) {
        snprintf(DirectoryImgName, sizeof(DirectoryImgName), "%s", "/sdcard/01_sys_init_img/07_zhongyu.bmp");
    } else {
        snprintf(DirectoryImgName, sizeof(DirectoryImgName), "%s", "/sdcard/01_sys_init_img/08_yin.bmp");
    }
    return DirectoryImgName;
}

WeatherAqi_t WeatherPort::WeatherPort_GetWeatherAQI(int aqi) {
    if (aqi <= 50) {
        snprintf(WeatherAqi.str, sizeof(WeatherAqi.str), "%s", "优");
        WeatherAqi.color = 0x06;
    } else if ((aqi > 50) && (aqi <= 100)) {
        snprintf(WeatherAqi.str, sizeof(WeatherAqi.str), "%s", "良");
        WeatherAqi.color = 0x02;
    } else if ((aqi > 100) && (aqi <= 150)) {
        snprintf(WeatherAqi.str, sizeof(WeatherAqi.str), "%s", "轻度污染");
        WeatherAqi.color = 0x03;
    } else {
        snprintf(WeatherAqi.str, sizeof(WeatherAqi.str), "%s", "严重污染");
        WeatherAqi.color = 0x03;
    }
    return WeatherAqi;
}

uint16_t WeatherPort::WeatherPort_ReassignCoordinates(uint16_t x, const char *str) {
    uint16_t x_or;
    uint16_t len = strlen(str) / 3;
    if (len == 1) {
        x_or = 32 + x;
        return x_or;
    } else if (len == 2) {
        x_or = 20 + x;
        return x_or;
    } else if (len == 3) {
        x_or = 11 + x;
        return x_or;
    }
    return x;
}
