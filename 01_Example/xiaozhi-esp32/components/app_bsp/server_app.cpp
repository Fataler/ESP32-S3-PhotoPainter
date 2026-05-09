#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <esp_check.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <mbedtls/md5.h>
#include <nvs.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "freertos/task.h"
#include "server_app.h"
#include "sdcard_bsp.h"
#include "button_bsp.h"
#include "mdns.h"
#include "user_app.h"
#include "power_bsp.h"
#include "ArduinoJson.h"

static const char *TAG = "server_bsp";

#define ServerPort_MIN(x, y) ((x < y) ? (x) : (y))
#define READ_LEN_MAX (10 * 1024) // Buffer area for receiving data
#define SEND_LEN_MAX (5 * 1024)  // Data for sending response
#define MAX_ADMIN_IMAGE_UPLOAD_BYTES (8 * 1024 * 1024)
#define MAX_ADMIN_THUMB_UPLOAD_BYTES (512 * 1024)
#define MAX_ADMIN_HTML_UPLOAD_BYTES (1024 * 1024)
#define MAX_DATAUP_UPLOAD_BYTES (2 * 1024 * 1024)

#define BSP_ESP_WIFI_SSID "esp_network"
#define BSP_ESP_WIFI_PASS "1234567890"
#define BSP_ESP_WIFI_CHANNEL 1
#define BSP_MAX_STA_CONN 4
#define USER_FOUNDATION_DIR "/sdcard/06_user_foundation_img"
#define USER_THUMB_DIR USER_FOUNDATION_DIR "/.thumbs"
#define USER_CONFIG_PATH USER_FOUNDATION_DIR "/config.txt"
#define SYS_HTML_DIR "/sdcard/03_sys_ap_html"

EventGroupHandle_t ServerPortGroups;
static CustomSDPort *SDPort_ = NULL;
static uint8_t netMode = 0;   //Default AP mode
static char pending_display_path[128] = {};
static bool pending_display_path_ready = false;
const char staresp[] = "1";
const char apresp[] = "0";

static bool is_supported_image_name(const char *name) {
    if (name == NULL || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    const char *ext = strrchr(name, '.');
    if (ext == NULL) {
        return false;
    }
    return strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".png") == 0;
}

static bool contains_ignore_case(const char *text, const char *needle) {
    if (needle == NULL || needle[0] == '\0') {
        return true;
    }
    if (text == NULL) {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p != '\0'; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_in_place(char *text) {
    if (text == NULL) {
        return;
    }

    char *read = text;
    char *write = text;
    while (*read != '\0') {
        if (read[0] == '%' && isxdigit((unsigned char)read[1]) && isxdigit((unsigned char)read[2])) {
            int hi = hex_value(read[1]);
            int lo = hex_value(read[2]);
            *write++ = (char)((hi << 4) | lo);
            read += 3;
        } else if (*read == '+') {
            *write++ = ' ';
            read++;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static void sanitize_filename(const char *input, char *output, size_t output_size) {
    if (output_size == 0) {
        return;
    }

    size_t pos = 0;
    const char *base = strrchr(input ? input : "", '/');
    input = base ? base + 1 : input;

    for (size_t i = 0; input != NULL && input[i] != '\0' && pos + 1 < output_size; i++) {
        char c = input[i];
        if (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') {
            output[pos++] = c;
        } else {
            output[pos++] = '_';
        }
    }

    output[pos] = '\0';
    if (pos == 0 || strstr(output, "..") != NULL || !is_supported_image_name(output)) {
        snprintf(output, output_size, "web_%lld.bmp", (long long)(esp_timer_get_time() / 1000));
    }
}

static bool make_foundation_path(const char *filename, char *path, size_t path_size) {
    char clean_name[64];
    sanitize_filename(filename, clean_name, sizeof(clean_name));
    if (!is_supported_image_name(clean_name)) {
        return false;
    }
    int written = snprintf(path, path_size, "%s/%s", USER_FOUNDATION_DIR, clean_name);
    return written > 0 && (size_t)written < path_size;
}

static bool make_existing_foundation_path(const char *filename, char *path, size_t path_size) {
    if (filename == NULL || filename[0] == '\0' || strstr(filename, "..") != NULL ||
        strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL || !is_supported_image_name(filename)) {
        return false;
    }
    int written = snprintf(path, path_size, "%s/%s", USER_FOUNDATION_DIR, filename);
    return written > 0 && (size_t)written < path_size;
}

static bool make_thumb_path(const char *filename, char *path, size_t path_size) {
    if (filename == NULL || filename[0] == '\0' || strstr(filename, "..") != NULL ||
        strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL || !is_supported_image_name(filename)) {
        return false;
    }
    int written = snprintf(path, path_size, "%s/%s.jpg", USER_THUMB_DIR, filename);
    return written > 0 && (size_t)written < path_size;
}

static bool make_sys_html_path(const char *filename, char *path, size_t path_size) {
    static const char *allowed[] = {
        "index.html",
        "styles.min.css",
        "script.min.js",
        "bootstrap.min.css",
        "bootstrap.min.js",
        "placeholder.svg",
        "manifest.webmanifest",
        "apple-touch-icon.png"
    };
    if (filename == NULL || filename[0] == '\0' || strstr(filename, "..") != NULL ||
        strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL) {
        return false;
    }
    bool ok = false;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(filename, allowed[i]) == 0) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        return false;
    }
    int written = snprintf(path, path_size, "%s/%s", SYS_HTML_DIR, filename);
    return written > 0 && (size_t)written < path_size;
}

static esp_err_t file_md5_hex(const char *path, char *hex, size_t hex_size) {
    if (hex_size < 33) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[16] = {};
    uint8_t *buf = (uint8_t *)heap_caps_malloc(SEND_LEN_MAX, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);

    size_t offset = 0;
    while (1) {
        int read_len = SDPort_->SDPort_ReadOffset(path, buf, SEND_LEN_MAX, offset);
        if (read_len < 0) {
            mbedtls_md5_free(&ctx);
            heap_caps_free(buf);
            return ESP_FAIL;
        }
        if (read_len == 0) {
            break;
        }
        mbedtls_md5_update(&ctx, buf, read_len);
        offset += read_len;
    }

    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);
    heap_caps_free(buf);

    for (int i = 0; i < 16; i++) {
        snprintf(hex + (i * 2), hex_size - (i * 2), "%02x", digest[i]);
    }
    hex[32] = '\0';
    return ESP_OK;
}

static esp_err_t read_request_body(httpd_req_t *req, char *buf, size_t buf_size) {
    if (buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t remaining = req->content_len;
    if (remaining >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t offset = 0;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + offset, remaining);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        offset += ret;
        remaining -= ret;
    }
    buf[offset] = '\0';
    return ESP_OK;
}

static bool append_json_string(char *response, size_t response_size, size_t *used, const char *value) {
    if (*used + 2 >= response_size) {
        return false;
    }
    response[(*used)++] = '"';
    for (size_t i = 0; value != NULL && value[i] != '\0'; i++) {
        char c = value[i];
        if (c == '"' || c == '\\') {
            if (*used + 2 >= response_size) {
                return false;
            }
            response[(*used)++] = '\\';
            response[(*used)++] = c;
        } else {
            if (*used + 1 >= response_size) {
                return false;
            }
            response[(*used)++] = c;
        }
    }
    if (*used + 1 >= response_size) {
        return false;
    }
    response[(*used)++] = '"';
    response[*used] = '\0';
    return true;
}

static const char *charge_status_to_string(uint8_t status) {
    switch (status) {
        case 0: return "tri_charge";
        case 1: return "pre_charge";
        case 2: return "constant_charge";
        case 3: return "constant_voltage";
        case 4: return "charge_done";
        case 5: return "not_charging";
        default: return "unknown";
    }
}

static bool is_valid_hhmm_time(const char *value) {
    if (value == NULL || strlen(value) != 5 || value[2] != ':') {
        return false;
    }
    if (value[0] < '0' || value[0] > '9' || value[1] < '0' || value[1] > '9' ||
        value[3] < '0' || value[3] > '9' || value[4] < '0' || value[4] > '9') {
        return false;
    }
    int hour = (value[0] - '0') * 10 + (value[1] - '0');
    int minute = (value[3] - '0') * 10 + (value[4] - '0');
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

static void set_connection_close(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Connection", "close");
}

static esp_err_t close_session_after_send(httpd_req_t *req, esp_err_t err) {
    if (err == ESP_OK) {
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    }
    return err;
}

typedef struct {
    const char *uri_token;
    const char *path;
    const char *content_type;
} static_resource_t;

static const static_resource_t k_static_resources[] = {
    {"index.html", "/sdcard/03_sys_ap_html/index.html", "text/html"},
    {"bootstrap.min.css", "/sdcard/03_sys_ap_html/bootstrap.min.css", "text/css"},
    {"styles.min.css", "/sdcard/03_sys_ap_html/styles.min.css", "text/css"},
    {"placeholder.svg", "/sdcard/03_sys_ap_html/placeholder.svg", "image/svg+xml"},
    {"apple-touch-icon.png", "/sdcard/03_sys_ap_html/apple-touch-icon.png", "image/png"},
    {"bootstrap.min.js", "/sdcard/03_sys_ap_html/bootstrap.min.js", "text/javascript"},
    {"script.min.js", "/sdcard/03_sys_ap_html/script.min.js", "text/javascript"},
};

static bool is_read_not_found(int read_len) {
    return read_len == -ESP_ERR_NOT_FOUND;
}

static esp_err_t stream_sd_file(httpd_req_t *req, const char *path, const char *content_type, bool no_store) {
    struct stat st = {};
    if (stat(path, &st) != 0) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found"));
    }

    char *buf = (char *)heap_caps_malloc(SEND_LEN_MAX, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory"));
    }

    httpd_resp_set_type(req, content_type);
    if (no_store) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
    set_connection_close(req);

    size_t offset = 0;
    while (1) {
        int read_len = SDPort_->SDPort_ReadOffset(path, buf, SEND_LEN_MAX, offset);
        if (read_len < 0) {
            ESP_LOGE(TAG, "stream read failed for %s: %d", path, read_len);
            heap_caps_free(buf);
            return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD read failed"));
        }
        if (read_len == 0) {
            break;
        }
        esp_err_t err = httpd_resp_send_chunk(req, buf, read_len);
        if (err != ESP_OK) {
            heap_caps_free(buf);
            return err;
        }
        offset += read_len;
        taskYIELD();
    }

    heap_caps_free(buf);
    return close_session_after_send(req, httpd_resp_send_chunk(req, NULL, 0));
}

static bool image_magic_matches(const char *filename, const uint8_t *magic, size_t magic_len) {
    const char *ext = strrchr(filename ? filename : "", '.');
    if (ext == NULL || magic == NULL) {
        return false;
    }
    if (strcasecmp(ext, ".bmp") == 0) {
        return magic_len >= 2 && magic[0] == 'B' && magic[1] == 'M';
    }
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return magic_len >= 3 && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
    }
    if (strcasecmp(ext, ".png") == 0) {
        static const uint8_t png_sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        return magic_len >= sizeof(png_sig) && memcmp(magic, png_sig, sizeof(png_sig)) == 0;
    }
    return false;
}

static bool jpeg_magic_matches(const uint8_t *magic, size_t magic_len) {
    return magic_len >= 3 && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
}

static esp_err_t receive_upload_to_sd(httpd_req_t *req, const char *path, const char *filename,
                                      size_t max_bytes, bool validate_image, bool validate_jpeg,
                                      size_t *written_total_out) {
    if (req->content_len == 0 || req->content_len > max_bytes) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Upload too large"));
    }

    char tmp_path[160];
    int tmp_written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (tmp_written <= 0 || (size_t)tmp_written >= sizeof(tmp_path)) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Temp path too long"));
    }

    char *buf = (char *)heap_caps_malloc(READ_LEN_MAX, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory"));
    }

    unlink(tmp_path);
    if (SDPort_->SDPort_WriteOffset(tmp_path, NULL, 0, 0) < 0) {
        heap_caps_free(buf);
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD open failed"));
    }

    uint8_t magic[16] = {};
    size_t magic_len = 0;
    size_t remaining = req->content_len;
    size_t written_total = 0;
    uint8_t timeout_count = 0;

    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf, ServerPort_MIN(remaining, READ_LEN_MAX));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT && timeout_count++ < 10) {
                continue;
            }
            heap_caps_free(buf);
            unlink(tmp_path);
            set_connection_close(req);
            return close_session_after_send(req, httpd_resp_send_408(req));
        }
        if (magic_len < sizeof(magic)) {
            size_t take = ServerPort_MIN((size_t)ret, sizeof(magic) - magic_len);
            memcpy(magic + magic_len, buf, take);
            magic_len += take;
        }
        int wrote = SDPort_->SDPort_WriteOffset(tmp_path, buf, ret, 1);
        if (wrote != ret) {
            heap_caps_free(buf);
            unlink(tmp_path);
            set_connection_close(req);
            return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD write failed"));
        }
        written_total += wrote;
        remaining -= ret;
        taskYIELD();
    }
    heap_caps_free(buf);

    bool magic_ok = true;
    if (validate_image) {
        magic_ok = image_magic_matches(filename, magic, magic_len);
    } else if (validate_jpeg) {
        magic_ok = jpeg_magic_matches(magic, magic_len);
    }
    if (!magic_ok) {
        unlink(tmp_path);
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File signature does not match extension"));
    }

    unlink(path);
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename failed %s -> %s: errno=%d", tmp_path, path, errno);
        unlink(tmp_path);
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD rename failed"));
    }

    if (written_total_out != NULL) {
        *written_total_out = written_total;
    }
    return ESP_OK;
}

static void restart_after_response_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t set_reboot_mode1_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("PhotoPainter", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs_handle, "PhotPainterMode", 0x01);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, "Mode_Flag", 0x01);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

static void set_current_foundation_image(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return;
    }

    uint8_t *buffer = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        return;
    }

    JsonDocument config_doc;
    int len = SDPort_->SDPort_ReadOffset(USER_CONFIG_PATH, buffer, 4095, 0);
    if (len > 0) {
        buffer[len] = '\0';
        deserializeJson(config_doc, (const char *)buffer);
    }
    config_doc["current"] = name;
    config_doc["skipCurrentOnce"] = true;
    if (!config_doc["last"].is<const char *>()) {
        config_doc["last"] = name;
    }
    if (!config_doc["timer"].is<int>()) {
        config_doc["timer"] = 300;
    }

    size_t out_len = serializeJsonPretty(config_doc, (char *)buffer, 4096);
    if (out_len > 0) {
        SDPort_->SDPort_WriteFile(USER_CONFIG_PATH, buffer, out_len);
    }
    heap_caps_free(buffer);
}

static esp_err_t send_device_status(httpd_req_t *req) {
    PmicBatteryMetrics battery = Custom_PmicGetBatteryMetrics();
    char *response = (char *)heap_caps_malloc(512, MALLOC_CAP_8BIT);
    if (response == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_ERR_NO_MEM;
    }

    snprintf(response, 512,
        "{"
            "\"ok\":true,"
            "\"network\":{\"mode\":\"%s\",\"code\":%u},"
            "\"battery\":{\"available\":%s,\"connected\":%s,\"level\":%d,\"voltage_mv\":%u,"
                "\"charging\":%s,\"discharging\":%s,\"standby\":%s,\"vbus_good\":%s,"
                "\"charge_status\":\"%s\",\"charge_status_code\":%u},"
            "\"source\":\"pmic\""
        "}",
        Get_CurrentlyNetworkMode() ? "STA" : "AP",
        Get_CurrentlyNetworkMode() ? 1 : 0,
        battery.available ? "true" : "false",
        battery.connected ? "true" : "false",
        battery.percent,
        battery.voltage_mv,
        battery.charging ? "true" : "false",
        battery.discharging ? "true" : "false",
        battery.standby ? "true" : "false",
        battery.vbus_good ? "true" : "false",
        charge_status_to_string(battery.charge_status),
        battery.charge_status
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    set_connection_close(req);
    esp_err_t err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    heap_caps_free(response);
    return close_session_after_send(req, err);
}

/*callback fun*/
esp_err_t manifest_webmanifest_handler(httpd_req_t *req);
esp_err_t static_resource_unified_handler(httpd_req_t *req);
esp_err_t device_status_handler(httpd_req_t *req);
esp_err_t receive_data_redirect_handler(httpd_req_t *req);
esp_err_t admin_images_handler(httpd_req_t *req);
esp_err_t admin_image_get_handler(httpd_req_t *req);
esp_err_t admin_thumb_get_handler(httpd_req_t *req);
esp_err_t admin_thumb_upload_handler(httpd_req_t *req);
esp_err_t admin_image_upload_handler(httpd_req_t *req);
esp_err_t admin_image_delete_handler(httpd_req_t *req);
esp_err_t admin_image_set_now_handler(httpd_req_t *req);
esp_err_t admin_config_get_handler(httpd_req_t *req);
esp_err_t admin_config_post_handler(httpd_req_t *req);
esp_err_t admin_reboot_mode1_handler(httpd_req_t *req);
esp_err_t admin_html_info_handler(httpd_req_t *req);
esp_err_t admin_html_upload_handler(httpd_req_t *req);
esp_err_t unknown_uri_handler(httpd_req_t *req);
void sta_wifi_event_callback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void ap_wifi_event_callback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

static void customfree(char *res) {
    if(NULL != res) {
        heap_caps_free(res);
        res = NULL;
    }
}

static bool buffer_looks_like_json_manifest(const char *buf, size_t len) {
    size_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) {
        i++;
    }
    if (i + 3 <= len && (unsigned char)buf[i] == 0xEF && (unsigned char)buf[i + 1] == 0xBB && (unsigned char)buf[i + 2] == 0xBF) {
        i += 3;
        while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) {
            i++;
        }
    }
    return i < len && (buf[i] == '{' || buf[i] == '[');
}

esp_err_t manifest_webmanifest_handler(httpd_req_t *req) {
    static const char k_default_manifest[] =
        "{\"name\":\"PhotoPainter Admin\",\"short_name\":\"PhotoPainter\","
        "\"start_url\":\"/index.html\",\"display\":\"standalone\","
        "\"background_color\":\"#f4f0e7\",\"theme_color\":\"#f4f0e7\","
        "\"icons\":[{\"src\":\"/apple-touch-icon.png\",\"sizes\":\"180x180\",\"type\":\"image/png\"}]}";
    const char path[] = "/sdcard/03_sys_ap_html/manifest.webmanifest";

    set_connection_close(req);
    httpd_resp_set_type(req, "application/manifest+json");

    char *resp_str = (char *) heap_caps_malloc(SEND_LEN_MAX + 1, MALLOC_CAP_SPIRAM);
    if (resp_str == NULL) {
        return close_session_after_send(req, httpd_resp_send(req, k_default_manifest, HTTPD_RESP_USE_STRLEN));
    }

    size_t str_respLen = 0;
    bool any_chunk = false;
    while (1) {
        int str_len = SDPort_->SDPort_ReadOffset(path, resp_str, SEND_LEN_MAX, str_respLen);
        if (str_len < 0) {
            customfree(resp_str);
            if (is_read_not_found(str_len)) {
                ESP_LOGW(TAG, "manifest.webmanifest missing; serving default");
                return close_session_after_send(req, httpd_resp_send(req, k_default_manifest, HTTPD_RESP_USE_STRLEN));
            }
            return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD read failed"));
        }
        if (str_len > 0) {
            if (!any_chunk && !buffer_looks_like_json_manifest(resp_str, str_len)) {
                ESP_LOGW(TAG, "manifest.webmanifest on SD is not JSON (wrong or corrupt file); serving default");
                customfree(resp_str);
                return close_session_after_send(req, httpd_resp_send(req, k_default_manifest, HTTPD_RESP_USE_STRLEN));
            }
            any_chunk = true;
            if (httpd_resp_send_chunk(req, resp_str, str_len) != ESP_OK) {
                customfree(resp_str);
                return ESP_FAIL;
            }
            str_respLen += str_len;
        } else {
            break;
        }
    }
    customfree(resp_str);
    if (!any_chunk) {
        ESP_LOGW(TAG, "manifest.webmanifest missing or empty; serving default");
        return close_session_after_send(req, httpd_resp_send(req, k_default_manifest, HTTPD_RESP_USE_STRLEN));
    }
    esp_err_t err = httpd_resp_send_chunk(req, NULL, 0);
    return close_session_after_send(req, err);
}

void ap_wifi_event_callback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        xEventGroupSetBits(ServerPortGroups, (0x01UL << 4));
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        xEventGroupSetBits(ServerPortGroups, (0x01UL << 5));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
    }
}

void sta_wifi_event_callback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(ServerPortGroups, GroupBit6); 
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE("wifi", "WiFi disconnected, trying to reconnect...");
        xEventGroupSetBits(ServerPortGroups, GroupBit5); 
    }
}

esp_err_t static_resource_unified_handler(httpd_req_t *req) {
    const char *uri = req->uri;                                     // The desired URI
    ESP_LOGI(TAG, "Return directly URL:%s",uri);

    for (size_t i = 0; i < sizeof(k_static_resources) / sizeof(k_static_resources[0]); i++) {
        if (strstr(uri, k_static_resources[i].uri_token)) {
            return stream_sd_file(req, k_static_resources[i].path, k_static_resources[i].content_type, false);
        }
    }

    if(strstr(uri,"/NetWorkStatus")) {
        set_connection_close(req);
        if(Get_CurrentlyNetworkMode()) {
            httpd_resp_send_chunk(req, staresp, HTTPD_RESP_USE_STRLEN);
        } else {
            httpd_resp_send_chunk(req, apresp, HTTPD_RESP_USE_STRLEN);
        }
    } else if(strstr(uri,"/DeviceStatus")) {
        return send_device_status(req);
    } else {     /*留给unknown_uri_handler处理*/
        return ESP_FAIL;
    }
    esp_err_t err = httpd_resp_send_chunk(req, NULL, 0);    // Send empty data to indicate completion of transmission
    return close_session_after_send(req, err);
}

esp_err_t device_status_handler(httpd_req_t *req) {
    return send_device_status(req);
}

esp_err_t admin_images_handler(httpd_req_t *req) {
    char query[192] = {};
    char value[64] = {};
    int offset = 0;
    int limit = 24;
    char search[64] = {};
    bool last_page = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "offset", value, sizeof(value)) == ESP_OK) {
            if (strcmp(value, "last") == 0) {
                last_page = true;
            } else {
                offset = atoi(value);
            }
        }
        if (httpd_query_key_value(query, "limit", value, sizeof(value)) == ESP_OK) {
            limit = atoi(value);
        }
        httpd_query_key_value(query, "q", search, sizeof(search));
        url_decode_in_place(search);
    }
    if (offset < 0) {
        offset = 0;
    }
    if (limit <= 0 || limit > 48) {
        limit = 24;
    }

    DIR *dir = opendir(USER_FOUNDATION_DIR);
    if (dir == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open image directory");
        return ESP_FAIL;
    }

    int matched = 0;
    long total_size = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR || !is_supported_image_name(entry->d_name) || strstr(entry->d_name, "sys_decode.bmp") || !contains_ignore_case(entry->d_name, search)) {
            continue;
        }

        char path[128];
        if (!make_existing_foundation_path(entry->d_name, path, sizeof(path))) {
            continue;
        }

        struct stat st = {};
        long size = 0;
        if (stat(path, &st) == 0) {
            size = st.st_size;
        }
        total_size += size;
        matched++;
    }

    if (last_page) {
        offset = matched > limit ? matched - limit : 0;
    }

    rewinddir(dir);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    set_connection_close(req);

    char chunk[256];
    int header_len = snprintf(chunk, sizeof(chunk),
                              "{\"ok\":true,\"dir\":\"%s\",\"offset\":%d,\"limit\":%d,\"total\":%d,\"totalSize\":%ld,\"images\":[",
                              USER_FOUNDATION_DIR, offset, limit, matched, total_size);
    if (header_len < 0 || (size_t)header_len >= sizeof(chunk) ||
        httpd_resp_send_chunk(req, chunk, header_len) != ESP_OK) {
        closedir(dir);
        return ESP_FAIL;
    }

    bool first = true;
    int seen = 0;
    int emitted = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR || !is_supported_image_name(entry->d_name) || strstr(entry->d_name, "sys_decode.bmp") || !contains_ignore_case(entry->d_name, search)) {
            continue;
        }

        char path[128];
        if (!make_existing_foundation_path(entry->d_name, path, sizeof(path))) {
            continue;
        }

        struct stat st = {};
        long size = 0;
        if (stat(path, &st) == 0) {
            size = st.st_size;
        }
        if (seen++ < offset) {
            continue;
        }
        if (emitted >= limit) {
            continue;
        }

        size_t used = 0;
        int written = snprintf(chunk, sizeof(chunk), "%s{\"name\":", first ? "" : ",");
        if (written < 0 || (size_t)written >= sizeof(chunk)) {
            break;
        }
        used = written;
        if (!append_json_string(chunk, sizeof(chunk), &used, entry->d_name)) {
            break;
        }
        written = snprintf(chunk + used, sizeof(chunk) - used, ",\"size\":%ld}", size);
        if (written < 0 || used + written >= sizeof(chunk)) {
            break;
        }
        used += written;
        if (httpd_resp_send_chunk(req, chunk, used) != ESP_OK) {
            closedir(dir);
            return ESP_FAIL;
        }
        first = false;
        emitted++;
    }
    closedir(dir);
    if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK) {
        return ESP_FAIL;
    }
    return close_session_after_send(req, httpd_resp_send_chunk(req, NULL, 0));
}

esp_err_t admin_image_get_handler(httpd_req_t *req) {
    char query[128] = {};
    char filename[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }
    url_decode_in_place(filename);

    char path[128];
    if (!make_existing_foundation_path(filename, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    struct stat st = {};
    if (stat(path, &st) != 0) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Image not found"));
    }

    const char *ext = strrchr(filename, '.');
    if (ext && strcasecmp(ext, ".bmp") == 0) {
        httpd_resp_set_type(req, "image/bmp");
    } else if (ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)) {
        httpd_resp_set_type(req, "image/jpeg");
    } else if (ext && strcasecmp(ext, ".png") == 0) {
        httpd_resp_set_type(req, "image/png");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }
    const char *content_type = ext && strcasecmp(ext, ".bmp") == 0 ? "image/bmp" :
                               ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) ? "image/jpeg" :
                               ext && strcasecmp(ext, ".png") == 0 ? "image/png" : "application/octet-stream";
    return stream_sd_file(req, path, content_type, true);
}

esp_err_t admin_thumb_get_handler(httpd_req_t *req) {
    char query[128] = {};
    char filename[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", filename, sizeof(filename)) != ESP_OK) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename"));
    }
    url_decode_in_place(filename);

    char path[140];
    if (!make_thumb_path(filename, path, sizeof(path))) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename"));
    }

    struct stat st = {};
    if (stat(path, &st) != 0) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Thumbnail not found"));
    }

    return stream_sd_file(req, path, "image/jpeg", true);
}

esp_err_t admin_thumb_upload_handler(httpd_req_t *req) {
    char query[128] = {};
    char filename[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }
    url_decode_in_place(filename);

    char path[140];
    if (!make_thumb_path(filename, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    mkdir(USER_THUMB_DIR, 0775);

    size_t written_total = 0;
    esp_err_t upload_err = receive_upload_to_sd(req, path, filename, MAX_ADMIN_THUMB_UPLOAD_BYTES, false, true, &written_total);
    if (upload_err != ESP_OK) {
        return upload_err;
    }

    char response[96];
    snprintf(response, sizeof(response), "{\"ok\":true,\"size\":%lu}", (unsigned long)written_total);
    httpd_resp_set_type(req, "application/json");
    set_connection_close(req);
    return close_session_after_send(req, httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN));
}

esp_err_t admin_image_upload_handler(httpd_req_t *req) {
    char query[128] = {};
    char filename[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", filename, sizeof(filename));
        url_decode_in_place(filename);
    }
    if (filename[0] == '\0') {
        snprintf(filename, sizeof(filename), "web_%lld.bmp", (long long)(esp_timer_get_time() / 1000));
    }

    char path[128];
    if (!make_foundation_path(filename, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    size_t written_total = 0;
    esp_err_t upload_err = receive_upload_to_sd(req, path, filename, MAX_ADMIN_IMAGE_UPLOAD_BYTES, true, false, &written_total);
    if (upload_err != ESP_OK) {
        return upload_err;
    }

    const char *name_out = filename;
    const char *slash = strrchr(path, '/');
    if (slash != NULL && slash[1] != '\0') {
        name_out = slash + 1;
    }
    char response[160];
    snprintf(response, sizeof(response), "{\"ok\":true,\"name\":\"%s\",\"size\":%lu}", name_out, (unsigned long)written_total);
    httpd_resp_set_type(req, "application/json");
    set_connection_close(req);
    return close_session_after_send(req, httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN));
}

esp_err_t admin_html_info_handler(httpd_req_t *req) {
    char query[128] = {};
    char filename[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", filename, sizeof(filename)) != ESP_OK) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename"));
    }
    url_decode_in_place(filename);

    char path[128];
    if (!make_sys_html_path(filename, path, sizeof(path))) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename"));
    }

    struct stat st = {};
    if (stat(path, &st) != 0) {
        char response[160];
        snprintf(response, sizeof(response), "{\"ok\":true,\"exists\":false,\"name\":\"%s\"}", filename);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN));
    }

    char md5[33] = {};
    if (file_md5_hex(path, md5, sizeof(md5)) != ESP_OK) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "MD5 failed"));
    }

    char response[192];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"exists\":true,\"name\":\"%s\",\"size\":%lu,\"md5\":\"%s\"}",
             filename, (unsigned long)st.st_size, md5);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    set_connection_close(req);
    return close_session_after_send(req, httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN));
}

esp_err_t admin_html_upload_handler(httpd_req_t *req) {
    char query[128] = {};
    char filename[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }
    url_decode_in_place(filename);

    char path[128];
    if (!make_sys_html_path(filename, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    size_t written_total = 0;
    esp_err_t upload_err = receive_upload_to_sd(req, path, filename, MAX_ADMIN_HTML_UPLOAD_BYTES, false, false, &written_total);
    if (upload_err != ESP_OK) {
        return upload_err;
    }

    char response[160];
    snprintf(response, sizeof(response), "{\"ok\":true,\"name\":\"%s\",\"size\":%lu}", filename, (unsigned long)written_total);
    httpd_resp_set_type(req, "application/json");
    set_connection_close(req);
    return close_session_after_send(req, httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN));
}

esp_err_t admin_image_delete_handler(httpd_req_t *req) {
    char body[160];
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char *name = doc["name"];
    char path[128];
    if (name == NULL || !make_existing_foundation_path(name, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    if (unlink(path) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    if (make_thumb_path(name, path, sizeof(path))) {
        unlink(path);
    }

    httpd_resp_set_type(req, "application/json");
    set_connection_close(req);
    return close_session_after_send(req, httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN));
}

esp_err_t admin_image_set_now_handler(httpd_req_t *req) {
    char body[160];
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char *name = doc["name"];
    char source_path[128];
    if (name == NULL || !make_existing_foundation_path(name, source_path, sizeof(source_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    struct stat st = {};
    if (stat(source_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    snprintf(pending_display_path, sizeof(pending_display_path), "%s", source_path);
    pending_display_path_ready = true;
    set_current_foundation_image(name);

    netMode = Get_CurrentlyNetworkMode();
    xEventGroupSetBits(ServerPortGroups, (0x1UL << 2));

    char response[160];
    snprintf(response, sizeof(response), "{\"ok\":true,\"name\":\"%s\",\"size\":%lu}", name, (unsigned long)st.st_size);
    httpd_resp_set_type(req, "application/json");
    set_connection_close(req);
    return close_session_after_send(req, httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN));
}

esp_err_t admin_reboot_mode1_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "admin requested reboot into mode 1");
    esp_err_t err = set_reboot_mode1_nvs();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return err;
    }

    httpd_resp_set_type(req, "application/json");
    set_connection_close(req);
    xTaskCreate(restart_after_response_task, "restart_mode1_task", 2048, NULL, 3, NULL);
    err = httpd_resp_send(req, "{\"ok\":true,\"mode\":1,\"restarting\":true}", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) {
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    }
    return err;
}

esp_err_t admin_config_get_handler(httpd_req_t *req) {
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_ERR_NO_MEM;
    }

    int timer = 300;
    JsonDocument doc;
    int len = SDPort_->SDPort_ReadOffset(USER_CONFIG_PATH, buffer, 4095, 0);
    if (len > 0) {
        buffer[len] = '\0';
        if (!deserializeJson(doc, (const char *)buffer)) {
            timer = doc["timer"] | timer;
        }
    }

    JsonDocument response_doc;
    response_doc["ok"] = true;
    response_doc["timer"] = timer;
    response_doc["order"] = doc["order"] | "sequential";
    response_doc["last"] = doc["last"] | "";
    response_doc["current"] = doc["current"] | "";
    response_doc["next"] = doc["next"] | "";
    if (doc["schedule"].is<JsonObject>()) {
        response_doc["schedule"] = doc["schedule"];
    }
    JsonArray playlist = response_doc["playlist"].to<JsonArray>();
    if (doc["playlist"].is<JsonArray>()) {
        for (const char *name : doc["playlist"].as<JsonArray>()) {
            if (name != NULL) {
                playlist.add(name);
            }
        }
    }

    size_t out_len = serializeJson(response_doc, (char *)buffer, 4096);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    set_connection_close(req);
    esp_err_t err = httpd_resp_send(req, (char *)buffer, out_len);
    heap_caps_free(buffer);
    return close_session_after_send(req, err);
}

esp_err_t admin_config_post_handler(httpd_req_t *req) {
    char body[4096];
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    JsonDocument patch_doc;
    if (deserializeJson(patch_doc, body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    int timer = patch_doc["timer"] | 0;
    if (timer < 30 || timer > 86400) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Timer must be 30..86400 seconds");
        return ESP_FAIL;
    }

    uint8_t *buffer = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_ERR_NO_MEM;
    }

    JsonDocument config_doc;
    int len = SDPort_->SDPort_ReadOffset(USER_CONFIG_PATH, buffer, 4095, 0);
    if (len > 0) {
        buffer[len] = '\0';
        deserializeJson(config_doc, (const char *)buffer);
    }
    config_doc["timer"] = timer;
    const char *order = patch_doc["order"] | "sequential";
    config_doc["order"] = strcmp(order, "random") == 0 ? "random" : "sequential";
    if (patch_doc["playlist"].is<JsonArray>()) {
        config_doc["playlist"].clear();
        JsonArray playlist = config_doc["playlist"].to<JsonArray>();
        for (const char *name : patch_doc["playlist"].as<JsonArray>()) {
            if (name != NULL && make_existing_foundation_path(name, (char *)buffer, 4096)) {
                playlist.add(name);
            }
        }
    }
    const char *last = patch_doc["last"];
    if (last != NULL) {
        if (last[0] == '\0') {
            config_doc["last"] = "";
        } else if (make_existing_foundation_path(last, (char *)buffer, 4096)) {
            config_doc["last"] = last;
        }
    }
    const char *current = patch_doc["current"];
    if (current != NULL) {
        if (current[0] == '\0') {
            config_doc["current"] = "";
        } else if (make_existing_foundation_path(current, (char *)buffer, 4096)) {
            config_doc["current"] = current;
        }
    }
    if (patch_doc["skipCurrentOnce"].is<bool>()) {
        config_doc["skipCurrentOnce"] = patch_doc["skipCurrentOnce"].as<bool>();
    }
    const char *next = patch_doc["next"];
    if (next != NULL) {
        if (next[0] == '\0') {
            config_doc.remove("next");
        } else if (make_existing_foundation_path(next, (char *)buffer, 4096)) {
            config_doc["next"] = next;
        }
    }
    if (patch_doc["schedule"].is<JsonObject>()) {
        JsonObject schedule_in = patch_doc["schedule"].as<JsonObject>();
        JsonObject schedule = config_doc["schedule"].to<JsonObject>();
        schedule["enabled"] = schedule_in["enabled"] | false;
        const char *day_start = schedule_in["dayStart"] | "08:00";
        const char *night_start = schedule_in["nightStart"] | "23:00";
        schedule["dayStart"] = is_valid_hhmm_time(day_start) ? day_start : "08:00";
        schedule["nightStart"] = is_valid_hhmm_time(night_start) ? night_start : "23:00";
        int day_timer = schedule_in["dayTimer"] | timer;
        if (day_timer < 30 || day_timer > 86400) {
            day_timer = timer;
        }
        schedule["dayTimer"] = day_timer;
        schedule["nightMode"] = "sleep_until_day";
        schedule["timezoneOffsetMinutes"] = schedule_in["timezoneOffsetMinutes"] | 0;
        schedule["savedEpoch"] = schedule_in["savedEpoch"] | 0;
        schedule["savedMillis"] = (long long)(esp_timer_get_time() / 1000);
    }

    size_t out_len = serializeJsonPretty(config_doc, (char *)buffer, 4096);
    if (out_len == 0 || SDPort_->SDPort_WriteFile(USER_CONFIG_PATH, buffer, out_len) != ESP_OK) {
        heap_caps_free(buffer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Config write failed");
        return ESP_FAIL;
    }
    heap_caps_free(buffer);

    bool reboot_mode1 = patch_doc["rebootMode1"] | false;
    esp_err_t reboot_err = ESP_OK;
    if (reboot_mode1) {
        ESP_LOGI(TAG, "AdminConfig requested reboot into mode 1");
        reboot_err = set_reboot_mode1_nvs();
        if (reboot_err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
            return reboot_err;
        }
    }

    char response[192];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"timer\":%d,\"order\":\"%s\",\"restarting\":%s}",
             timer, config_doc["order"].as<const char *>(), reboot_mode1 ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    set_connection_close(req);
    if (reboot_mode1) {
        xTaskCreate(restart_after_response_task, "restart_mode1_task", 2048, NULL, 3, NULL);
    }
    esp_err_t resp_err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    if (resp_err == ESP_OK && reboot_mode1) {
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
        return resp_err;
    }
    return close_session_after_send(req, resp_err);
}

esp_err_t receive_data_redirect_handler(httpd_req_t *req) {
    char       *buf        = (char *) heap_caps_malloc(READ_LEN_MAX + 1, MALLOC_CAP_SPIRAM);
    size_t      sdcard_len = 0;
    size_t      remaining  = req->content_len;
    const char *uri        = req->uri;
    int         ret;
    uint8_t     timeoutive = 0;      /*Expiry timeout and automatic logout*/
    bool        is_NetworkMode = 1;  /*Handling the flag bits for the ESP32 mode*/
    ESP_LOGW("TAG", "Receive url:%s,byte:%d", uri, remaining);
    if (buf == NULL) {
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory"));
    }
    if (req->content_len <= 1 || req->content_len > MAX_DATAUP_UPLOAD_BYTES) {
        customfree(buf);
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Upload too large"));
    }
    xEventGroupSetBits(ServerPortGroups, (0x1UL << 0)); 
    const char *target_path = "/sdcard/02_sys_ap_img/user_send.bmp";
    const char *tmp_path = "/sdcard/02_sys_ap_img/user_send.bmp.tmp";
    uint8_t bmp_magic[2] = {};
    size_t bmp_magic_len = 0;
    unlink(tmp_path);
    if (SDPort_->SDPort_WriteOffset(tmp_path, NULL, 0, 0) < 0) {
        customfree(buf);
        set_connection_close(req);
        return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD open failed"));
    }
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, ServerPort_MIN(remaining, READ_LEN_MAX))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                timeoutive++;
                if(timeoutive == 10) {
                    httpd_resp_send_408(req);
                    customfree(buf);
                    return ESP_FAIL;
                }
                continue;
            }
            customfree(buf);
            return ESP_FAIL;
        }
        int req_len = 0;
        if(is_NetworkMode) {
            is_NetworkMode = 0;
            netMode = buf[0];
            if (ret > 1 && bmp_magic_len < sizeof(bmp_magic)) {
                size_t take = ServerPort_MIN((size_t)ret - 1, sizeof(bmp_magic) - bmp_magic_len);
                memcpy(bmp_magic + bmp_magic_len, buf + 1, take);
                bmp_magic_len += take;
            }
            req_len = SDPort_->SDPort_WriteOffset(tmp_path, (buf + 1), (ret - 1), 1);
            if (req_len < 0 || req_len != ret - 1) {
                unlink(tmp_path);
                customfree(buf);
                set_connection_close(req);
                return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD write failed"));
            }
            sdcard_len += req_len; // Final comparison result
            remaining -= ret;      // Subtract the data that has already been received
        } else {
            if (bmp_magic_len < sizeof(bmp_magic)) {
                size_t take = ServerPort_MIN((size_t)ret, sizeof(bmp_magic) - bmp_magic_len);
                memcpy(bmp_magic + bmp_magic_len, buf, take);
                bmp_magic_len += take;
            }
            req_len = SDPort_->SDPort_WriteOffset(tmp_path, buf, ret, 1);
            if (req_len < 0 || req_len != ret) {
                unlink(tmp_path);
                customfree(buf);
                set_connection_close(req);
                return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD write failed"));
            }
            sdcard_len += req_len; // Final comparison result
            remaining -= ret;      // Subtract the data that has already been received
        }
    }
    xEventGroupSetBits(ServerPortGroups, (0x1UL << 1)); 
    esp_err_t resp_err = ESP_OK;
    if ((sdcard_len + 1) == req->content_len) {
        if (bmp_magic_len < sizeof(bmp_magic) || bmp_magic[0] != 'B' || bmp_magic[1] != 'M') {
            unlink(tmp_path);
            set_connection_close(req);
            customfree(buf);
            xEventGroupSetBits(ServerPortGroups, (0x1UL << 3));
            return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid BMP payload"));
        }
        unlink(target_path);
        if (rename(tmp_path, target_path) != 0) {
            unlink(tmp_path);
            set_connection_close(req);
            customfree(buf);
            xEventGroupSetBits(ServerPortGroups, (0x1UL << 3));
            return close_session_after_send(req, httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD rename failed"));
        }
        set_connection_close(req);
        resp_err = httpd_resp_send(req, "Data verification successful", strlen("Data verification successful"));
        xEventGroupSetBits(ServerPortGroups, (0x1UL << 2));
    } else {
        unlink(tmp_path);
        httpd_resp_send_408(req);
        xEventGroupSetBits(ServerPortGroups, (0x1UL << 3));
        resp_err = ESP_FAIL;
    } 
    ESP_LOGW(TAG,"netMode:%d",netMode);
    customfree(buf);
    return close_session_after_send(req, resp_err);
}

esp_err_t unknown_uri_handler(httpd_req_t *req) {
    const char *uri = req->uri; 
    httpd_method_t req_method = (httpd_method_t)req->method;
    if(req_method == HTTP_GET) {
        ESP_LOGW("err", "Request interface : GET,Return directly URL:%s", uri);
    } else if(req_method == HTTP_POST) {
        ESP_LOGW("err", "Request interface : POST,Return directly URL:%s", uri);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Resources do not exist");
    return ESP_OK;
}

void ServerPort_NetworkAPInit(void) {
    ServerPortGroups         = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    assert(esp_netif_create_default_wifi_ap());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &ap_wifi_event_callback,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_AP_STAIPASSIGNED,
                                                        &ap_wifi_event_callback,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {};
    snprintf((char *) wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%s", BSP_ESP_WIFI_SSID);
    snprintf((char *) wifi_config.ap.password, sizeof(wifi_config.ap.password), "%s", BSP_ESP_WIFI_PASS);
    wifi_config.ap.channel        = BSP_ESP_WIFI_CHANNEL;
    wifi_config.ap.max_connection = BSP_MAX_STA_CONN;
    wifi_config.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI("network", "wifi_init_softap finished. SSID:%s password:%s channel:%d", BSP_ESP_WIFI_SSID, BSP_ESP_WIFI_PASS, BSP_ESP_WIFI_CHANNEL);
}

uint8_t ServerPort_NetworkSTAInit(wifi_credential_t creden) {
    ServerPortGroups         = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    assert(esp_netif_create_default_wifi_sta());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t Instance_WIFI_IP;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_wifi_event_callback, NULL, &Instance_WIFI_IP);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_wifi_event_callback, NULL, &Instance_WIFI_IP);
    
    wifi_config_t wifi_config = {};
    memset(wifi_config.sta.ssid, 0, sizeof(wifi_config.sta.ssid));
    memset(wifi_config.sta.password, 0, sizeof(wifi_config.sta.password));
    memcpy(wifi_config.sta.ssid, creden.ssid, strnlen(creden.ssid, sizeof(wifi_config.sta.ssid) - 1));
    memcpy(wifi_config.sta.password, creden.password, strnlen(creden.password, sizeof(wifi_config.sta.password) - 1));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t even = xEventGroupWaitBits(ServerPortGroups, (GroupBit6), pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
    if(even & GroupBit6) {
        ESP_LOGW(TAG, "WiFi connected successfully");
        return 1;
    } else {
        ESP_LOGE(TAG, "WiFi connection timed out");
        return 0;
    }
}

void ServerPort_init(CustomSDPort *SDPort) {
    if(SDPort_ == NULL) {
        SDPort_ = SDPort;
    }
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn   = httpd_uri_match_wildcard; /*Wildcard enabling*/
    config.max_uri_handlers = 19;
    config.lru_purge_enable = true;
    config.stack_size     = 20480;
    config.recv_wait_timeout = 120;
    config.send_wait_timeout = 120;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    /*Event callback function*/
    httpd_uri_t uri_config = {};
    uri_config.uri         = "/DeviceStatus";
    uri_config.method      = HTTP_GET;
    uri_config.handler     = device_status_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminImages";
    uri_config.method      = HTTP_GET;
    uri_config.handler     = admin_images_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminImage";
    uri_config.method      = HTTP_GET;
    uri_config.handler     = admin_image_get_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminThumb";
    uri_config.method      = HTTP_GET;
    uri_config.handler     = admin_thumb_get_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminThumbUpload";
    uri_config.method      = HTTP_POST;
    uri_config.handler     = admin_thumb_upload_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminImageUpload";
    uri_config.method      = HTTP_POST;
    uri_config.handler     = admin_image_upload_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminHtmlUpload";
    uri_config.method      = HTTP_POST;
    uri_config.handler     = admin_html_upload_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminHtmlInfo";
    uri_config.method      = HTTP_GET;
    uri_config.handler     = admin_html_info_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminImageDelete";
    uri_config.method      = HTTP_POST;
    uri_config.handler     = admin_image_delete_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminImageSetNow";
    uri_config.method      = HTTP_POST;
    uri_config.handler     = admin_image_set_now_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminConfig";
    uri_config.method      = HTTP_GET;
    uri_config.handler     = admin_config_get_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminConfig";
    uri_config.method      = HTTP_POST;
    uri_config.handler     = admin_config_post_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminRebootMode1";
    uri_config.method      = HTTP_POST;
    uri_config.handler     = admin_reboot_mode1_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/AdminRebootMode1";
    uri_config.method      = HTTP_GET;
    uri_config.handler     = admin_reboot_mode1_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/manifest.webmanifest";
    uri_config.method      = HTTP_GET;
    uri_config.handler     = manifest_webmanifest_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);

    uri_config.uri         = "/*";
    uri_config.method      = HTTP_GET;
    uri_config.handler     = static_resource_unified_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);
    
    uri_config.uri         = "/dataUP";
    uri_config.method      = HTTP_POST;
    uri_config.handler     = receive_data_redirect_handler;
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);
    
    uri_config.uri         = "/*"; // Match all URLs that have not been handled by other handlers
    uri_config.method      = (httpd_method_t)(HTTP_GET | HTTP_POST);
    uri_config.handler     = unknown_uri_handler; // Callback for returning a 404 response
    uri_config.user_ctx    = NULL;
    httpd_register_uri_handler(server, &uri_config);
}

void ServerPort_SetNetworkSleep(void) {
    esp_wifi_stop();
    esp_wifi_deinit();
    vTaskDelay(pdMS_TO_TICKS(500));
}

bool ServerPort_TakePendingDisplayPath(char *path, size_t path_size) {
    if (path == NULL || path_size == 0 || !pending_display_path_ready) {
        return false;
    }

    snprintf(path, path_size, "%s", pending_display_path);
    pending_display_path[0] = '\0';
    pending_display_path_ready = false;
    return true;
}

uint8_t Get_NetworkMode(void) {
    return netMode;
}

void Mdns_init_config(void) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS Init failed: %s", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set("esp32-s3-photopainter");
    mdns_instance_name_set("ESP32-S3 WebServer");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGW(TAG, "mDns配置完成,可通过 http://esp32-s3-photopainter.local/index.html 访问");
}
