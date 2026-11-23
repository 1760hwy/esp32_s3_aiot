#include <stdlib.h>
#include <string.h>

#include "app_wifi.h"

#include "esp_check.h"
#include "esp_event_base.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "APP_WIFI";

static bool s_connected = false;
static bool s_initialized = false;
static esp_netif_t *s_wifi_netif = NULL;
static char s_current_ssid[33] = {0};
static char s_current_ip[16] = {0};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "Wi-Fi STA 就绪");
            // 上电启动时，如果之前在 Flash 存过配置，自动尝试连接
            wifi_config_t saved_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &saved_cfg) == ESP_OK && strlen((char *)saved_cfg.sta.ssid) > 0) {
                strlcpy(s_current_ssid, (char *)saved_cfg.sta.ssid, sizeof(s_current_ssid));
                ESP_LOGI(TAG, "发现上次保存的 Wi-Fi 热点 [%s]，正在自动连接...", s_current_ssid);
                esp_wifi_connect();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            memset(s_current_ip, 0, sizeof(s_current_ip));
            ESP_LOGW(TAG, "Wi-Fi 连接断开");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_connected = true;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi 获取到 IP: %s (SSID: %s)", s_current_ip, s_current_ssid);
    }
}

bool app_wifi_is_connected(void)
{
    return s_connected;
}

bool app_wifi_get_ip_info(char *ip_buf, size_t ip_len, char *ssid_buf, size_t ssid_len)
{
    if (!s_connected) return false;
    if (ip_buf) strlcpy(ip_buf, s_current_ip, ip_len);
    if (ssid_buf) strlcpy(ssid_buf, s_current_ssid, ssid_len);
    return true;
}

esp_err_t app_wifi_connect_to(const char *ssid, const char *password)
{
    if (!s_initialized || ssid == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "开始连接 Wi-Fi: %s ...", ssid);
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }
    strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    return esp_wifi_connect();
}

uint16_t app_wifi_scan(wifi_scan_ap_info_t *ap_list, uint16_t max_aps)
{
    if (!s_initialized) return 0;

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    ESP_LOGI(TAG, "开始扫描周边 Wi-Fi...");
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true); // 阻塞扫描
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 扫描失败: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGI(TAG, "未发现周边 Wi-Fi");
        return 0;
    }

    uint16_t fetch_count = (ap_count > max_aps) ? max_aps : ap_count;
    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * fetch_count);
    if (!records) return 0;

    esp_wifi_scan_get_ap_records(&fetch_count, records);

    for (int i = 0; i < fetch_count; i++) {
        strlcpy(ap_list[i].ssid, (const char *)records[i].ssid, sizeof(ap_list[i].ssid));
        ap_list[i].rssi = records[i].rssi;
        ap_list[i].authmode = records[i].authmode;
    }

    free(records);
    ESP_LOGI(TAG, "扫描完成，发现 %d 个 Wi-Fi 热点", fetch_count);
    return fetch_count;
}

esp_err_t app_wifi_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // 1. NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    // 2. Netif
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Netif init failed");

    // 3. 事件循环 (允许已初始化)
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    // 4. 创建 STA 接口
    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) return ESP_FAIL;

    // 5. 初始化 WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "WiFi init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG, "WIFI event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
                        TAG, "IP event register failed");

    // 开启 FLASH 持久化存储 (断电不丢失 SSID 和密码)
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "WiFi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "WiFi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "WiFi start failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi 驱动初始化完成 (STA 模式就绪，支持扫描与动态配网)");
    return ESP_OK;
}
